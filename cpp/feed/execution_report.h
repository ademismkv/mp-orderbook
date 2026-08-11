#pragma once
#include "order_book_v2.h"
#include <string>

// Execution Reports: the stage after Risk in ROADMAP.md's pipeline (Feed
// -> Sequencer -> Order Books -> Risk -> Execution Reports -> Market
// Data Feed) — what a real exchange sends back to a participant about
// their own order: was it accepted, was it rejected (and why), did it
// fill (and at what price/quantity).
//
// This is a replay of historical data, not a live participant submitting
// orders and waiting on a response — there's no real counterparty to
// deliver these to. What's real here is the mechanism: every order this
// engine processes through Sequencer -> Risk -> OrderBookV2 produces the
// same structured Ack/Reject/Fill records a live system would generate,
// via the same code path, not a separate reporting bolt-on that could
// drift out of sync with what actually happened to the order.

enum class ExecReportType { Ack, Reject, Fill };

struct ExecutionReport {
    OrderId order_id = 0;
    ExecReportType type = ExecReportType::Ack;
    std::string symbol;
    Quantity qty = 0;    // requested qty for Ack/Reject; filled qty for Fill
    Price price = 0;      // fill price for Fill; 0 otherwise
    std::string reason;   // populated for Reject
};
