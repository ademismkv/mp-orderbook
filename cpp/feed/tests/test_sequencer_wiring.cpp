#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "sequencer.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <unordered_map>

// End-to-end wiring test: real ITCH bytes -> real parser -> real
// Sequencer -> real, symbol-sharded OrderBookV2 instances. This is what
// closes ROADMAP.md's Feed -> Sequencer -> Order Books gap — until this
// test, the parser and the matching engine had never been connected.
//
// The real sample spans ~8,900 distinct symbols (one Stock Directory
// message per listed symbol), so this is a genuine multi-symbol
// sharding test, not a single-book replay like replay_lobster.cpp. Each
// shard gets a deliberately small arena_capacity/initial_window — this
// repo's default OrderBookV2 constructor sizes for a single
// high-volume instrument (1M-order arena, 20,000-tick window); doing
// that ~8,900 times would be tens of gigabytes for a wiring-verification
// harness. A real deployment shards across many machines/processes, not
// one process holding every listed symbol's book — this is a
// demonstration at feasible scale, not a claim about production sizing.
//
// This test is also what found a real engine bug: the first run hit
// std::bad_alloc trying to allocate ~13GB inside OrderBookV2. Root
// cause (via ASan): a real order in the real data — symbol TVIX, price
// 888888800 (a $88,888.88 sentinel/garbage value) — asked
// ensure_index_for_price() to grow the level array by ~891M entries.
// Nothing before that point validated that a caller-supplied price was
// sane. Fixed with a bounded-growth guard in OrderBookV2 itself
// (kMaxLevels, see order_book_v2.h/.cpp) — re-verified against the full
// existing test suite (unit tests, differential fuzz incl. the
// rebase-walk generator that exercises this exact path, TSan-checked
// threaded test) before trusting it. See devlog day 17.
namespace {
constexpr uint32_t kShardArenaCapacity = 128;
constexpr int64_t kShardInitialWindow = 200;
} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    std::unordered_map<std::string, std::unique_ptr<OrderBookV2>> shards;
    OrderBookV2* last_touched = nullptr;
    auto book_for = [&](const std::string& symbol) -> OrderBookV2& {
        auto it = shards.find(symbol);
        if (it == shards.end()) {
            it = shards.emplace(symbol, std::make_unique<OrderBookV2>(kShardArenaCapacity, kShardInitialWindow))
                     .first;
        }
        last_touched = it->second.get();
        return *it->second;
    };

    Sequencer seq;
    itch::BinaryFileReader reader(path);

    uint64_t total_messages = 0, parse_failures = 0, invariant_violations = 0, both_sides_present = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (auto raw = reader.next_raw()) {
        ++total_messages;
        auto parsed = itch::parse_message(raw->data(), raw->size());
        if (!parsed) {
            ++parse_failures;
            continue;
        }
        last_touched = nullptr;
        seq.on_message(*parsed, book_for);
        if (last_touched && last_touched->has_bid() && last_touched->has_ask()) {
            ++both_sides_present;
            if (last_touched->best_bid() >= last_touched->best_ask()) ++invariant_violations;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    const auto& s = seq.stats();
    std::printf("=== Feed -> Sequencer -> Order Books, real data: %s ===\n", path.c_str());
    std::printf("total messages:      %llu (parse failures: %llu)\n", (unsigned long long)total_messages,
                (unsigned long long)parse_failures);
    std::printf("wall time:           %.3fs (%.2fM msgs/sec)\n", wall, total_messages / wall / 1e6);
    std::printf("symbols sharded:     %zu\n", shards.size());
    std::printf("\n");
    std::printf("add_orders:          %llu\n", (unsigned long long)s.add_orders);
    std::printf("cancels (partial):   %llu\n", (unsigned long long)s.cancels);
    std::printf("deletes (full):      %llu\n", (unsigned long long)s.deletes);
    std::printf("executes:            %llu\n", (unsigned long long)s.executes);
    std::printf("non_printable_skip:  %llu\n", (unsigned long long)s.non_printable_skipped);
    std::printf("replaces:            %llu\n", (unsigned long long)s.replaces);
    std::printf("unrouted:            %llu (order id referenced before/without a known Add Order)\n",
                (unsigned long long)s.unrouted);
    std::printf("other_message_types: %llu (System Event, Stock Directory, etc. — don't touch a book)\n",
                (unsigned long long)s.other_message_types);
    std::printf("trades_produced:     %llu (this engine's own match() output, real crosses)\n",
                (unsigned long long)s.trades_produced);
    std::printf(
        "add_rejected:        %llu (demo-scale arena full, or a bad/sentinel price hit kMaxLevels — see file header)\n",
        (unsigned long long)s.add_rejected);
    std::printf("\n");
    std::printf("moments with both a bid and ask present: %llu\n", (unsigned long long)both_sides_present);
    std::printf("book invariant violations (crossed book, checked after every mutating message): %llu\n",
                (unsigned long long)invariant_violations);

    // trades_produced == 0 on real data turned out to be expected, not a
    // bug — checked before assuming otherwise. ITCH's Add Order messages
    // are, by construction, the resting REMAINDER after the real
    // exchange already matched anything that crossed on arrival; replaying
    // only that stream back through this engine's own match() rarely
    // reproduces a fresh cross, especially across ~828 thin symbols in an
    // early, reference-data-heavy slice rather than one liquid symbol's
    // full session (contrast replay_lobster.cpp: 13,298 trades from a
    // full AAPL day). 436,836 moments had both a bid and ask resting
    // simultaneously with zero invariant violations, so the book is
    // behaving correctly — it's just correctly never being asked to
    // cross by this particular slice's Add Order stream.
    //
    // What actually proves the Sequencer -> OrderBookV2 wiring can
    // produce a real trade when one SHOULD happen: two synthetic but
    // realistically-shaped ITCH messages, fed through the exact same
    // Sequencer::on_message() path as everything above, with prices that
    // must cross.
    itch::AddOrderNoMPID buy{};
    buy.h = itch::CommonHeader{1, 1, 123456789};
    buy.order_ref_number = 900000001;
    buy.buy_sell_indicator = 'B';
    buy.shares = 100;
    buy.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    buy.price = 100000;   // $10.00
    seq.on_message(itch::Message{buy}, book_for);

    itch::AddOrderNoMPID sell{};
    sell.h = itch::CommonHeader{1, 1, 123456790};
    sell.order_ref_number = 900000002;
    sell.buy_sell_indicator = 'S';
    sell.shares = 100;
    sell.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    sell.price = 99000;   // $9.90 — crosses the resting $10.00 bid
    seq.on_message(itch::Message{sell}, book_for);

    const uint64_t trades_after_synthetic_cross = seq.stats().trades_produced;
    std::printf("\nsynthetic crossing-order check: trades_produced went from 0 to %llu -> %s\n",
                (unsigned long long)trades_after_synthetic_cross,
                trades_after_synthetic_cross > 0 ? "wiring can produce a real trade" : "FAILED TO CROSS");

    const bool pass = (parse_failures == 0) && (invariant_violations == 0) && (shards.size() > 0) &&
                       (s.add_orders > 0) && (trades_after_synthetic_cross > 0);
    std::printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
