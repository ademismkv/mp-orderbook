#pragma once
#include "itch_messages.h"
#include "order_book_v2.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Sequencer: the layer between the Feed Handler and the matching engine's
// order books — Feed → Sequencer → Order Books in ROADMAP.md's pipeline.
//
// The real problem this class exists to solve: after Add Order, ITCH
// messages don't carry the stock symbol. Order Cancel, Order Delete,
// Order Executed, and Order Replace only carry an order_ref_number — the
// spec says explicitly (see itch_messages.h's OrderReplace comment)
// "these fields are not included in the message... firms should retain
// the side, stock symbol and MPID from the original Add Order message."
// That's this class's job: track order_ref_number -> (symbol, side) from
// each Add Order, so every later message referencing that id can be
// routed to the correct per-symbol OrderBookV2 shard, consistent with
// ADR-1's "one thread owns one symbol's book, parallelism is across
// symbols" model already used by bench_threaded_scaling.cpp.
//
// Message-to-operation mapping follows the same interpretation
// cpp/tools/replay_lobster.cpp already established and verified against
// real NASDAQ data: Add Order calls add() and lets this engine's own
// match() decide what crosses (this engine independently re-derives
// matches, it doesn't trust the feed to tell it what matched). Order
// Executed / Order Executed With Price are NOT re-submitted as new
// operations — the real exchange already decided that fill happened, so
// they're applied as reduce() against the resting order's quantity, the
// same "don't leave phantom oversized resting orders in the book" fix
// replay_lobster.cpp's header comment documents. Order Cancel (partial)
// maps to reduce(), Order Delete to cancel(), Order Replace to a
// cancel-then-add carrying the original side/symbol forward.
class Sequencer {
public:
    // Resolves a symbol to the OrderBookV2 shard that owns it. Decoupled
    // from this class rather than owned by it, so callers choose their
    // own sharding/threading strategy — a single shared map for the
    // single-threaded wiring test today, per-thread SPSC-fed shards in a
    // real threaded build later (see ADR-1, bench_threaded_scaling.cpp).
    using BookForSymbol = std::function<OrderBookV2&(const std::string& symbol)>;

    struct Stats {
        uint64_t add_orders = 0;
        uint64_t cancels = 0;          // partial (Order Cancel 'X')
        uint64_t deletes = 0;          // full (Order Delete 'D')
        uint64_t executes = 0;         // Order Executed 'E' + printable Order Executed With Price 'C'
        uint64_t non_printable_skipped = 0;   // 'C' with printable='N' — spec's own recommendation to ignore
        uint64_t replaces = 0;
        uint64_t unrouted = 0;         // order_ref_number with no known symbol — Cancel/Delete/Executed/Replace
                                        // for an id this Sequencer never saw an Add Order for (e.g. mid-stream
                                        // start, or a real gap MoldUDP64 recovery didn't fully backfill)
        uint64_t other_message_types = 0;   // System Event, Stock Directory, etc. — real, valid, don't touch a book
        uint64_t trades_produced = 0;       // sum of trades this engine's own match() produced across all add()s
        uint64_t add_rejected = 0;          // OrderBookV2::add() threw — arena exhausted (demo-scale shard, see
                                             // wiring test) or the kMaxLevels price-band guard (see devlog day
                                             // 17 — a real 888888800-tick sentinel price found in real data)
    };

    void on_message(const itch::Message& msg, const BookForSymbol& book_for);

    const Stats& stats() const { return stats_; }
    size_t known_orders() const { return order_symbol_.size(); }

private:
    struct OrderInfo {
        std::string symbol;
        Side side;
    };
    std::unordered_map<uint64_t, OrderInfo> order_symbol_;
    std::vector<Trade> scratch_trades_;   // reused across add() calls — see order_book_v2.h's reusable-buffer note

    Stats stats_;
};
