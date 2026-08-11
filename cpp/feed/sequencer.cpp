#include "sequencer.h"
#include <type_traits>

namespace {
inline Side side_from_indicator(char c) { return c == 'B' ? Side::Buy : Side::Sell; }
} // namespace

void Sequencer::on_message(const itch::Message& msg, const BookForSymbol& book_for, RiskEngine* risk,
                            const ExecutionReportCallback& report_cb) {
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, itch::AddOrderNoMPID> || std::is_same_v<T, itch::AddOrderMPID>) {
                const std::string symbol = m.stock.str();
                const Side side = side_from_indicator(m.buy_sell_indicator);
                std::string mpid;
                if constexpr (std::is_same_v<T, itch::AddOrderMPID>) mpid = m.attribution.str();

                OrderBookV2& book = book_for(symbol);
                OrderRequest req;
                req.id = m.order_ref_number;
                req.side = side;
                req.type = Type::Limit;
                req.price = static_cast<Price>(m.price);   // Price(4) ticks — matches this engine's convention directly, see replay_lobster.cpp
                req.qty = m.shares;

                if (risk) {
                    auto verdict = risk->check(req, symbol, mpid, book);
                    if (!verdict.accepted) {
                        stats_.risk_rejected++;
                        if (report_cb) {
                            report_cb(ExecutionReport{req.id, ExecReportType::Reject, symbol, req.qty, 0,
                                                       verdict.detail});
                        }
                        return;   // never reaches order_symbol_ or the book — genuinely never submitted
                    }
                }

                order_symbol_[m.order_ref_number] = OrderInfo{symbol, side, mpid};
                try {
                    book.add(req, scratch_trades_);
                    stats_.add_orders++;
                    stats_.trades_produced += scratch_trades_.size();
                    if (risk) risk->note_resting(symbol, mpid, side);
                    if (report_cb) report_cb(ExecutionReport{req.id, ExecReportType::Ack, symbol, req.qty, 0, ""});
                    for (const auto& t : scratch_trades_) {
                        if (risk) risk->on_trade(symbol, t.price);
                        if (report_cb) {
                            report_cb(ExecutionReport{req.id, ExecReportType::Fill, symbol, t.qty, t.price, ""});
                        }
                    }
                } catch (const std::runtime_error& e) {
                    stats_.add_rejected++;
                    order_symbol_.erase(m.order_ref_number);   // never actually rested — don't leave a dangling route
                    if (report_cb) report_cb(ExecutionReport{req.id, ExecReportType::Reject, symbol, req.qty, 0, e.what()});
                }

            } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
                auto it = order_symbol_.find(m.order_ref_number);
                if (it == order_symbol_.end()) {
                    stats_.unrouted++;
                    return;
                }
                book_for(it->second.symbol).reduce(m.order_ref_number, m.canceled_shares);
                stats_.cancels++;

            } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
                auto it = order_symbol_.find(m.order_ref_number);
                if (it == order_symbol_.end()) {
                    stats_.unrouted++;
                    return;
                }
                book_for(it->second.symbol).cancel(m.order_ref_number);
                if (risk) risk->note_removed(it->second.symbol, it->second.mpid, it->second.side);
                order_symbol_.erase(it);
                stats_.deletes++;

            } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
                auto it = order_symbol_.find(m.order_ref_number);
                if (it == order_symbol_.end()) {
                    stats_.unrouted++;
                    return;
                }
                // Real exchange already decided this fill happened —
                // reduce the resting order's quantity rather than ask
                // this engine's match() to re-derive it. Same
                // interpretation replay_lobster.cpp uses for LOBSTER's
                // type-4 visible execution events.
                book_for(it->second.symbol).reduce(m.order_ref_number, m.executed_shares);
                // Plain Order Executed carries no price field (only the
                // resting order's original price applies, which this
                // message doesn't repeat) — nothing to feed the
                // price-band reference here. Order Executed With Price
                // and Trade messages do carry a real execution price.
                stats_.executes++;

            } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
                if (m.printable == 'N') {
                    // Spec's own recommendation: "NASDAQ recommends firms
                    // ignore messages marked non-printable to prevent
                    // double counting" (e.g. a cross bulk-printed later).
                    stats_.non_printable_skipped++;
                    return;
                }
                auto it = order_symbol_.find(m.order_ref_number);
                if (it == order_symbol_.end()) {
                    stats_.unrouted++;
                    return;
                }
                book_for(it->second.symbol).reduce(m.order_ref_number, m.executed_shares);
                if (risk) risk->on_trade(it->second.symbol, static_cast<Price>(m.execution_price));
                stats_.executes++;

            } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
                auto it = order_symbol_.find(m.original_order_ref_number);
                if (it == order_symbol_.end()) {
                    stats_.unrouted++;
                    return;
                }
                const OrderInfo info = it->second;   // copy — about to erase the map entry
                OrderBookV2& book = book_for(info.symbol);
                book.cancel(m.original_order_ref_number);
                if (risk) risk->note_removed(info.symbol, info.mpid, info.side);
                order_symbol_.erase(it);

                OrderRequest req;
                req.id = m.new_order_ref_number;
                req.side = info.side;       // carried forward — Order Replace doesn't carry side, see itch_messages.h
                req.type = Type::Limit;
                req.price = static_cast<Price>(m.price);
                req.qty = m.shares;

                if (risk) {
                    auto verdict = risk->check(req, info.symbol, info.mpid, book);
                    if (!verdict.accepted) {
                        stats_.risk_rejected++;
                        if (report_cb) {
                            report_cb(ExecutionReport{req.id, ExecReportType::Reject, info.symbol, req.qty, 0,
                                                       verdict.detail});
                        }
                        return;   // original already canceled — replace genuinely rejected, no new resting order
                    }
                }

                try {
                    book.add(req, scratch_trades_);
                    order_symbol_[m.new_order_ref_number] = info;   // same symbol/side/mpid, new id
                    stats_.replaces++;
                    stats_.trades_produced += scratch_trades_.size();
                    if (risk) risk->note_resting(info.symbol, info.mpid, info.side);
                    if (report_cb) report_cb(ExecutionReport{req.id, ExecReportType::Ack, info.symbol, req.qty, 0, ""});
                    for (const auto& t : scratch_trades_) {
                        if (risk) risk->on_trade(info.symbol, t.price);
                        if (report_cb) {
                            report_cb(ExecutionReport{req.id, ExecReportType::Fill, info.symbol, t.qty, t.price, ""});
                        }
                    }
                } catch (const std::runtime_error& e) {
                    stats_.add_rejected++;
                    if (report_cb) {
                        report_cb(ExecutionReport{req.id, ExecReportType::Reject, info.symbol, req.qty, 0, e.what()});
                    }
                }

            } else {
                // System Event, Stock Directory, Stock Trading Action,
                // Reg SHO, Market Participant Position, MWCB, IPO,
                // LULD, Operational Halt, Trade, Cross Trade, Broken
                // Trade, NOII, RPII, Direct Listing — real, valid ITCH
                // messages, none of which mutate an order book directly.
                stats_.other_message_types++;
            }
        },
        msg);
}
