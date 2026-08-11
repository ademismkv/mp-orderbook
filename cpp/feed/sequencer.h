#pragma once
#include "execution_report.h"
#include "itch_messages.h"
#include "order_book_v2.h"
#include "risk.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Sequencer: the layer between the Feed Handler and the matching engine's
// order books — Feed -> Sequencer -> Order Books -> Risk -> Execution
// Reports in ROADMAP.md's pipeline. Risk and Execution Reports are
// optional parameters (both default to off) so this stays the same class
// used by the day-17 wiring test, not a second implementation that could
// drift from it.
//
// The real problem this class exists to solve: after Add Order, ITCH
// messages don't carry the stock symbol. Order Cancel, Order Delete,
// Order Executed, and Order Replace only carry an order_ref_number — the
// spec says explicitly (see itch_messages.h's OrderReplace comment)
// "these fields are not included in the message... firms should retain
// the side, stock symbol and MPID from the original Add Order message."
// That's this class's job: track order_ref_number -> (symbol, side,
// mpid) from each Add Order, so every later message referencing that id
// can be routed to the correct per-symbol OrderBookV2 shard, consistent
// with ADR-1's "one thread owns one symbol's book, parallelism is across
// symbols" model already used by bench_threaded_scaling.cpp.
//
// Message-to-operation mapping follows the same interpretation
// cpp/tools/replay_lobster.cpp already established and verified against
// real NASDAQ data: Add Order calls add() and lets this engine's own
// match() decide what crosses. Order Executed / Order Executed With
// Price are NOT re-submitted as new operations — the real exchange
// already decided that fill happened, so they're applied as reduce()
// against the resting order's quantity. Order Cancel (partial) maps to
// reduce(), Order Delete to cancel(), Order Replace to a cancel-then-add
// carrying the original side/symbol/mpid forward.
class Sequencer {
public:
    using BookForSymbol = std::function<OrderBookV2&(const std::string& symbol)>;
    using ExecutionReportCallback = std::function<void(const ExecutionReport&)>;

    struct Stats {
        uint64_t add_orders = 0;
        uint64_t cancels = 0;          // partial (Order Cancel 'X')
        uint64_t deletes = 0;          // full (Order Delete 'D')
        uint64_t executes = 0;         // Order Executed 'E' + printable Order Executed With Price 'C'
        uint64_t non_printable_skipped = 0;   // 'C' with printable='N' — spec's own recommendation to ignore
        uint64_t replaces = 0;
        uint64_t unrouted = 0;         // order_ref_number with no known symbol
        uint64_t other_message_types = 0;   // System Event, Stock Directory, etc. — don't touch a book
        uint64_t trades_produced = 0;       // sum of trades this engine's own match() produced across all add()s
        uint64_t add_rejected = 0;          // OrderBookV2::add() threw — arena exhausted or kMaxLevels (see day 17)
        uint64_t risk_rejected = 0;         // RiskEngine declined the order before it ever reached the book
    };

    // `risk` may be nullptr — no risk checking, same behavior as before
    // Risk existed. `report_cb` may be empty — no reports emitted.
    void on_message(const itch::Message& msg, const BookForSymbol& book_for, RiskEngine* risk = nullptr,
                     const ExecutionReportCallback& report_cb = {});

    const Stats& stats() const { return stats_; }
    size_t known_orders() const { return order_symbol_.size(); }

private:
    struct OrderInfo {
        std::string symbol;
        Side side;
        std::string mpid;   // empty for Add Order — No MPID; see risk.h's self-trade scope note
    };
    std::unordered_map<uint64_t, OrderInfo> order_symbol_;
    std::vector<Trade> scratch_trades_;   // reused across add() calls — see order_book_v2.h's reusable-buffer note

    Stats stats_;
};
