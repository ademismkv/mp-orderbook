#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "market_data.h"
#include "sequencer.h"
#include "spsc_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// ADR-1's threading model ("symbol -> hash -> shard -> single writer, no
// shared mutable state between shards") has, until now, only ever been
// proven against a SYNTHETIC workload (bench_threaded_scaling.cpp) — never
// demonstrated against the real Feed -> Sequencer -> Risk -> Order Books
// pipeline this repo actually built. This test closes that gap: it runs
// the real 691,421-message ITCH file through the pipeline TWICE — once
// single-threaded (the existing, already-verified reference behavior, same
// as test_sequencer_wiring.cpp), once split across K worker threads, each
// owning a disjoint subset of the real 828 symbols — and requires the two
// runs to agree exactly: same aggregate Sequencer::Stats, same total
// market data event count, and 0 mismatches in every symbol's final
// best-bid/ask, across all 828 real symbols.
//
// Why this is safe to shard at all, checked against the real code (not
// assumed): Sequencer's own on_message() only ever touches its
// order_ref_number -> symbol routing table and the OrderBookV2 for that
// one symbol — never another symbol's state. RiskEngine's internal state
// (risk.h) is keyed entirely by symbol (last_trade_price_[symbol], and a
// composite key starting with symbol for self-trade tracking) — there is
// no cross-symbol interaction anywhere in either class. That means K
// independent (Sequencer, RiskEngine, OrderBookV2-shard-map) instances,
// each fed only the messages for the symbols it owns, produce IDENTICAL
// results to one shared instance seeing everything — this test is the
// actual proof of that claim, not just an assertion of it.
//
// Architecture: a single ROUTER thread parses the real file exactly as
// today's sequential pipeline does, but instead of calling
// Sequencer::on_message() directly, it determines which symbol each
// message belongs to (Add Order messages carry the symbol directly; every
// later message type only carries an order_ref_number, so the router
// keeps its own lightweight order_ref_number -> symbol table — a smaller,
// routing-only version of what Sequencer itself already tracks internally
// for the same reason, see sequencer.h's class comment) and pushes the
// RAW itch::Message into hash(symbol) % K's SpscRingBuffer. This preserves
// each symbol's exact original relative message order (the router is
// single-threaded and processes the file strictly in order; a SPSC ring
// is FIFO) — the actual property price-time priority and
// cancel/replace/delete ordering depend on. K worker threads each drain
// their own ring buffer and call the real, UNMODIFIED
// Sequencer::on_message() — this test does not reimplement or shortcut
// any matching/risk logic, it only reroutes real messages to real,
// per-shard instances of the same code the sequential path already uses.

namespace {
constexpr int kShards = 4;
constexpr size_t kRingCapacity = 1u << 16;   // power of two, required by SpscRingBuffer

int g_failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

struct TrueState {
    bool has_bid = false, has_ask = false;
    Price best_bid = 0, best_ask = 0;
};

// Lightweight, routing-only order_ref_number -> symbol table. Deliberately
// NOT Sequencer::OrderInfo (which also carries side/mpid for risk/MD
// purposes) — the router only ever needs to know which shard owns a given
// order_ref_number, nothing else about it.
std::optional<std::string> route_symbol_for(const itch::Message& msg,
                                             std::unordered_map<uint64_t, std::string>& routes) {
    return std::visit(
        [&](const auto& m) -> std::optional<std::string> {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, itch::AddOrderNoMPID> || std::is_same_v<T, itch::AddOrderMPID>) {
                std::string sym = m.stock.str();
                routes[m.order_ref_number] = sym;
                return sym;
            } else if constexpr (std::is_same_v<T, itch::OrderCancel> || std::is_same_v<T, itch::OrderExecuted> ||
                                  std::is_same_v<T, itch::OrderExecutedWithPrice>) {
                auto it = routes.find(m.order_ref_number);
                return it != routes.end() ? std::optional<std::string>(it->second) : std::nullopt;
            } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
                auto it = routes.find(m.order_ref_number);
                if (it == routes.end()) return std::nullopt;
                std::string sym = it->second;
                routes.erase(it);   // mirrors Sequencer::on_message's own eviction on full delete
                return sym;
            } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
                auto it = routes.find(m.original_order_ref_number);
                if (it == routes.end()) return std::nullopt;
                std::string sym = it->second;
                routes.erase(it);
                routes[m.new_order_ref_number] = sym;   // new id routes to the same symbol going forward
                return sym;
            } else {
                // System Event, Stock Directory, Trade (non-cross), Cross
                // Trade, etc. — real, valid ITCH messages, none of which
                // touch order_symbol_ or a book in Sequencer::on_message
                // either. Safe to route arbitrarily (shard 0) since they
                // have no per-symbol side effects to preserve ordering
                // for.
                return std::nullopt;
            }
        },
        msg);
}

int shard_for(const std::string& symbol) { return static_cast<int>(std::hash<std::string>{}(symbol) % kShards); }
} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";
    // Optional cap on messages processed by BOTH passes — the full real
    // file (default) is the actual correctness proof; a smaller cap exists
    // only so a TSan-instrumented run (5-10x memory overhead on top of an
    // already twice-run, multi-shard pipeline) fits in a memory-constrained
    // CI/sandbox environment without needing a second, separately
    // maintained test. A structural data race in the router/ring-buffer/
    // worker protocol would reproduce at any message count that exercises
    // the same code paths, not just at full scale.
    const uint64_t max_messages = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : UINT64_MAX;

    // --- Pass 1: sequential reference (identical to test_sequencer_wiring.cpp) ---
    std::unordered_map<std::string, std::unique_ptr<OrderBookV2>> ref_shards;
    auto ref_book_for = [&](const std::string& symbol) -> OrderBookV2& {
        auto it = ref_shards.find(symbol);
        if (it == ref_shards.end()) it = ref_shards.emplace(symbol, std::make_unique<OrderBookV2>(128, 200)).first;
        return *it->second;
    };
    Sequencer ref_seq;
    RiskEngine ref_risk;
    uint64_t ref_md_events = 0;
    auto ref_md_cb = [&](const mdfeed::MDEvent&) { ++ref_md_events; };

    {
        itch::BinaryFileReader reader(path);
        uint64_t n = 0;
        while (n < max_messages) {
            auto raw = reader.next_raw();
            if (!raw) break;
            ++n;
            auto parsed = itch::parse_message(raw->data(), raw->size());
            if (!parsed) continue;
            ref_seq.on_message(*parsed, ref_book_for, &ref_risk, {}, ref_md_cb);
        }
    }
    std::unordered_map<std::string, TrueState> true_state;
    for (const auto& [sym, book] : ref_shards) {
        true_state[sym] = TrueState{book->has_bid(), book->has_ask(), book->has_bid() ? book->best_bid() : 0,
                                     book->has_ask() ? book->best_ask() : 0};
    }
    std::printf("=== Concurrent sharded pipeline: sequential reference ===\n");
    std::printf("symbols: %zu, add_orders: %llu, cancels: %llu, deletes: %llu, executes: %llu, replaces: %llu, "
                "trades: %llu, risk_rejected: %llu, md_events: %llu\n",
                ref_shards.size(), (unsigned long long)ref_seq.stats().add_orders,
                (unsigned long long)ref_seq.stats().cancels, (unsigned long long)ref_seq.stats().deletes,
                (unsigned long long)ref_seq.stats().executes, (unsigned long long)ref_seq.stats().replaces,
                (unsigned long long)ref_seq.stats().trades_produced,
                (unsigned long long)ref_seq.stats().risk_rejected, (unsigned long long)ref_md_events);

    // --- Pass 2: concurrent, symbol-sharded ---
    // unique_ptr, not a plain SpscRingBuffer value in the vector: the ring
    // buffer's internal std::atomic head_/tail_ make it neither copyable
    // nor movable, which std::vector's growth path requires even when no
    // reallocation ever actually happens at runtime.
    std::vector<std::unique_ptr<SpscRingBuffer<itch::Message>>> rings;
    rings.reserve(kShards);
    for (int i = 0; i < kShards; ++i) rings.push_back(std::make_unique<SpscRingBuffer<itch::Message>>(kRingCapacity));
    std::atomic<bool> router_done{false};
    std::atomic<bool> router_failed{false};

    std::vector<Sequencer> shard_seq(kShards);
    std::vector<RiskEngine> shard_risk(kShards);
    std::vector<std::unordered_map<std::string, std::unique_ptr<OrderBookV2>>> shard_books(kShards);
    std::vector<uint64_t> shard_md_events(kShards, 0);
    std::atomic<bool> worker_failed[kShards];
    for (auto& f : worker_failed) f.store(false);

    std::vector<std::thread> workers;
    for (int k = 0; k < kShards; ++k) {
        workers.emplace_back([&, k] {
            try {
                auto book_for = [&](const std::string& symbol) -> OrderBookV2& {
                    auto it = shard_books[k].find(symbol);
                    if (it == shard_books[k].end()) {
                        it = shard_books[k].emplace(symbol, std::make_unique<OrderBookV2>(128, 200)).first;
                    }
                    return *it->second;
                };
                auto md_cb = [&](const mdfeed::MDEvent&) { ++shard_md_events[k]; };
                itch::Message msg;
                while (true) {
                    if (rings[k]->pop(msg)) {
                        shard_seq[k].on_message(msg, book_for, &shard_risk[k], {}, md_cb);
                    } else if (router_done.load(std::memory_order_acquire)) {
                        if (!rings[k]->pop(msg)) break;   // confirmed empty after done — real drain, not a race
                        shard_seq[k].on_message(msg, book_for, &shard_risk[k], {}, md_cb);
                    } else {
                        std::this_thread::yield();
                    }
                }
            } catch (const std::exception& e) {
                std::printf("FAIL: shard %d worker threw: %s\n", k, e.what());
                worker_failed[k].store(true);
            }
        });
    }

    std::thread router_thread([&] {
        try {
            std::unordered_map<uint64_t, std::string> routes;
            itch::BinaryFileReader reader(path);
            uint64_t n = 0;
            while (n < max_messages) {
                auto raw = reader.next_raw();
                if (!raw) break;
                ++n;
                auto parsed = itch::parse_message(raw->data(), raw->size());
                if (!parsed) continue;
                auto sym = route_symbol_for(*parsed, routes);
                const int k = sym ? shard_for(*sym) : 0;   // unroutable/no-side-effect messages -> shard 0, harmless
                while (!rings[k]->push(*parsed)) std::this_thread::yield();   // real backpressure, never drop
            }
        } catch (const std::exception& e) {
            std::printf("FAIL: router thread threw: %s\n", e.what());
            router_failed.store(true);
        }
        router_done.store(true, std::memory_order_release);
    });

    router_thread.join();
    for (auto& t : workers) t.join();

    expect(!router_failed.load(), "router thread completed without throwing");
    bool any_worker_failed = false;
    for (int k = 0; k < kShards; ++k) any_worker_failed |= worker_failed[k].load();
    expect(!any_worker_failed, "every shard worker thread completed without throwing");

    // --- Aggregate and compare ---
    Sequencer::Stats agg{};
    uint64_t agg_md_events = 0;
    std::unordered_map<std::string, TrueState> reconstructed;
    int duplicate_symbol_across_shards = 0;
    for (int k = 0; k < kShards; ++k) {
        const auto& s = shard_seq[k].stats();
        agg.add_orders += s.add_orders;
        agg.cancels += s.cancels;
        agg.deletes += s.deletes;
        agg.executes += s.executes;
        agg.non_printable_skipped += s.non_printable_skipped;
        agg.replaces += s.replaces;
        agg.unrouted += s.unrouted;
        agg.other_message_types += s.other_message_types;
        agg.trades_produced += s.trades_produced;
        agg.add_rejected += s.add_rejected;
        agg.risk_rejected += s.risk_rejected;
        agg_md_events += shard_md_events[k];

        for (const auto& [sym, book] : shard_books[k]) {
            if (reconstructed.count(sym)) ++duplicate_symbol_across_shards;   // should be structurally impossible
            reconstructed[sym] = TrueState{book->has_bid(), book->has_ask(), book->has_bid() ? book->best_bid() : 0,
                                            book->has_ask() ? book->best_ask() : 0};
        }
    }
    std::printf("=== concurrent (%d shards) results ===\n", kShards);
    std::printf("symbols: %zu, add_orders: %llu, cancels: %llu, deletes: %llu, executes: %llu, replaces: %llu, "
                "trades: %llu, risk_rejected: %llu, md_events: %llu\n",
                reconstructed.size(), (unsigned long long)agg.add_orders, (unsigned long long)agg.cancels,
                (unsigned long long)agg.deletes, (unsigned long long)agg.executes,
                (unsigned long long)agg.replaces, (unsigned long long)agg.trades_produced,
                (unsigned long long)agg.risk_rejected, (unsigned long long)agg_md_events);

    expect(duplicate_symbol_across_shards == 0, "every symbol lived in exactly one shard (deterministic hashing)");
    expect(reconstructed.size() == ref_shards.size(), "same number of real symbols sharded as the sequential reference");
    expect(agg.add_orders == ref_seq.stats().add_orders, "add_orders matches the sequential reference exactly");
    expect(agg.cancels == ref_seq.stats().cancels, "cancels matches the sequential reference exactly");
    expect(agg.deletes == ref_seq.stats().deletes, "deletes matches the sequential reference exactly");
    expect(agg.executes == ref_seq.stats().executes, "executes matches the sequential reference exactly");
    expect(agg.non_printable_skipped == ref_seq.stats().non_printable_skipped,
           "non_printable_skipped matches the sequential reference exactly");
    expect(agg.replaces == ref_seq.stats().replaces, "replaces matches the sequential reference exactly");
    expect(agg.other_message_types == ref_seq.stats().other_message_types,
           "other_message_types matches the sequential reference exactly");
    expect(agg.trades_produced == ref_seq.stats().trades_produced,
           "trades_produced matches the sequential reference exactly");
    expect(agg.risk_rejected == ref_seq.stats().risk_rejected, "risk_rejected matches the sequential reference exactly");
    expect(agg.add_rejected == ref_seq.stats().add_rejected, "add_rejected matches the sequential reference exactly");
    expect(agg_md_events == ref_md_events, "total market data events matches the sequential reference exactly");

    int mismatches = 0;
    for (const auto& [sym, truth] : true_state) {
        auto it = reconstructed.find(sym);
        const TrueState got = (it != reconstructed.end()) ? it->second : TrueState{};
        if (got.has_bid != truth.has_bid || got.has_ask != truth.has_ask ||
            (got.has_bid && got.best_bid != truth.best_bid) || (got.has_ask && got.best_ask != truth.best_ask)) {
            ++mismatches;
            if (mismatches <= 5) {
                std::printf("MISMATCH %s: sequential(bid=%d:%lld ask=%d:%lld) concurrent(bid=%d:%lld ask=%d:%lld)\n",
                            sym.c_str(), truth.has_bid, (long long)truth.best_bid, truth.has_ask,
                            (long long)truth.best_ask, got.has_bid, (long long)got.best_bid, got.has_ask,
                            (long long)got.best_ask);
            }
        }
    }
    std::printf("symbol best-bid/ask mismatches: %d / %zu\n", mismatches, true_state.size());
    expect(mismatches == 0,
           "concurrent, symbol-sharded execution matches the sequential reference's final best bid/ask for every "
           "real symbol — ADR-1's threading model, proven against the real ITCH pipeline, not just a synthetic "
           "benchmark");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
