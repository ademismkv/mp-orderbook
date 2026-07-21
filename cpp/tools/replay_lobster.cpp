#include "order_book_v2.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// Replays a real LOBSTER-format message file (real NASDAQ TotalView-ITCH
// derived order events, no synthetic data) through OrderBookV2.
//
// LOBSTER message columns: time, event_type, order_id, size, price, direction
//   event_type: 1=new limit order, 2=partial cancel, 3=full delete,
//               4=visible execution, 5=hidden execution, 7=trading halt
//   direction:  1=buy-side order, -1=sell-side order (refers to the side
//               of the order named by order_id — the resting order for
//               cancels/executes, the new order for type 1)
//   price:      already an integer in 1/10000 dollars — used directly as
//               this engine's integer Price tick, no conversion needed.
//
// What this replay does and does NOT claim:
//   - Type 1/2/3 (new/partial-cancel/delete) are fed through this engine's
//     real add()/reduce()/cancel() — this engine independently decides
//     whether each type-1 order crosses the book, using the same code path
//     as everything else in this repo.
//   - Type 4/5 (executions) are NOT re-submitted as new operations — they
//     are NASDAQ's report of trades that already happened as a side effect
//     of some type-1 order crossing the book, not independent order-book
//     events from a submitter's perspective. Feeding them in separately
//     would double-count trades.
//   - Type 5 specifically (hidden order execution) represents liquidity
//     that was NEVER visible in this message stream in the first place —
//     that's the definition of a hidden order. No replay of the visible
//     message stream can reconstruct what a hidden order's price/size was.
//     This is a known, fundamental limitation of LOBSTER-based book
//     reconstruction generally, not specific to this implementation —
//     LOBSTER's own documentation notes the same thing.
//   - This program cross-checks its own trade volume against LOBSTER's
//     reported type-4 (visible) execution volume as a sanity signal, not
//     as a hard pass/fail assertion — exact agreement isn't guaranteed
//     (self-trades, LOBSTER's own reconstruction choices, and the type-5
//     gap above all mean some divergence is expected, not a bug).

struct LobsterEvent {
    double time;
    int type;
    uint64_t order_id;
    uint64_t size;
    int64_t price;
    int direction;
};

static bool parse_line(const std::string& line, LobsterEvent& ev) {
    std::stringstream ss(line);
    std::string field;
    if (!std::getline(ss, field, ',')) return false;
    ev.time = std::strtod(field.c_str(), nullptr);
    if (!std::getline(ss, field, ',')) return false;
    ev.type = std::atoi(field.c_str());
    if (!std::getline(ss, field, ',')) return false;
    ev.order_id = std::strtoull(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.size = std::strtoull(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.price = std::strtoll(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.direction = std::atoi(field.c_str());
    return true;
}

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/AAPL_2012-06-21_34200000_57600000_message_10.csv";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::fprintf(stderr, "could not open %s\n", path.c_str());
        return 1;
    }

    // Window sized to comfortably cover a real full trading day's range
    // for one name (this file spans $577.35-$588.32, i.e. ~109,700 ticks)
    // without needing a rebase — rebases are correct (tested) but there's
    // no reason to pay for them here.
    OrderBookV2 book(1u << 21, /*initial_window=*/150000);

    uint64_t type_counts[8] = {0};
    uint64_t lines = 0, parse_failures = 0;
    uint64_t engine_trades = 0, engine_trade_qty = 0;
    uint64_t lobster_visible_exec_qty = 0, lobster_hidden_exec_qty = 0;
    uint64_t invariant_violations = 0;
    uint64_t reduce_misses = 0, cancel_misses = 0;

    std::string line;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        LobsterEvent ev;
        if (!parse_line(line, ev)) {
            parse_failures++;
            continue;
        }
        lines++;
        if (ev.type >= 1 && ev.type <= 7) type_counts[ev.type]++;

        const Side side = (ev.direction == 1) ? Side::Buy : Side::Sell;

        switch (ev.type) {
            case 1: {  // new limit order
                OrderRequest req{ev.order_id, side, Type::Limit, ev.price, ev.size};
                for (auto& t : book.add(req)) {
                    engine_trades++;
                    engine_trade_qty += t.qty;
                }
                break;
            }
            case 2:  // partial cancel
                if (!book.reduce(ev.order_id, ev.size)) reduce_misses++;
                break;
            case 3:  // full delete
                if (!book.cancel(ev.order_id)) cancel_misses++;
                break;
            case 4:  // visible execution against a resting order
                // Deliberately NOT re-run through match() — that would
                // fabricate a second Trade for something that already
                // happened, double-counting this engine's own trade
                // ledger. But the resting order genuinely lost this much
                // quantity in reality, so apply that as a reduce() to keep
                // this engine's book state faithful to history — first
                // version of this replay skipped this entirely, which left
                // phantom, too-large resting orders sitting in the book
                // and caused this engine's independent matching to diverge
                // from what actually happened (see devlog).
                lobster_visible_exec_qty += ev.size;
                book.reduce(ev.order_id, ev.size);
                break;
            case 5:  // hidden execution — against liquidity never visible in this message stream in the first place
                // Can't reduce an order this engine never saw as a type-1
                // (that's the definition of hidden) — nothing to apply.
                // This is the one category of divergence from real history
                // that's structurally unrecoverable from this data source.
                lobster_hidden_exec_qty += ev.size;
                break;
            case 7:  // trading halt — no book operation
            default:
                break;
        }

        // Invariant check after every operation: the book must never be
        // crossed (best bid >= best ask) once both sides are populated.
        if (book.has_bid() && book.has_ask() && book.best_bid() >= book.best_ask()) {
            invariant_violations++;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    std::printf("=== LOBSTER replay: %s ===\n", path.c_str());
    std::printf("lines processed: %llu (parse failures: %llu)\n", (unsigned long long)lines, (unsigned long long)parse_failures);
    std::printf("event types — new:%llu partial_cancel:%llu delete:%llu visible_exec:%llu hidden_exec:%llu halt:%llu\n",
        (unsigned long long)type_counts[1], (unsigned long long)type_counts[2], (unsigned long long)type_counts[3],
        (unsigned long long)type_counts[4], (unsigned long long)type_counts[5], (unsigned long long)type_counts[7]);
    std::printf("wall time: %.3fs (%.3fM real events/sec)\n", wall, (lines / wall) / 1e6);
    std::printf("\n");
    std::printf("this engine's own trades:        count=%llu  total_qty=%llu\n", (unsigned long long)engine_trades, (unsigned long long)engine_trade_qty);
    std::printf("LOBSTER-reported visible exec qty: %llu  (sanity cross-check, not required to match exactly — see header comment)\n", (unsigned long long)lobster_visible_exec_qty);
    std::printf("LOBSTER-reported hidden exec qty:   %llu  (fundamentally unobservable in this message stream — cannot be replayed)\n", (unsigned long long)lobster_hidden_exec_qty);
    std::printf("\n");
    std::printf("book invariant violations (best_bid >= best_ask): %llu\n", (unsigned long long)invariant_violations);
    std::printf("reduce() on unknown/already-gone id: %llu   cancel() on unknown/already-gone id: %llu\n",
        (unsigned long long)reduce_misses, (unsigned long long)cancel_misses);
    std::printf("final state: depth=%zu best_bid=%lld best_ask=%lld\n",
        book.depth(), (long long)book.best_bid(), (long long)book.best_ask());
    std::printf("levels_ growth/rebase events (O(n)-in-open-orders path, ADR-2): %llu\n",
        (unsigned long long)book.level_array_growths());

    return (invariant_violations == 0) ? 0 : 1;
}
