#include "risk.h"
#include <cmath>

RiskEngine::RiskEngine() : RiskEngine(Config{}) {}
RiskEngine::RiskEngine(Config cfg) : cfg_(cfg) {}

RiskEngine::Verdict RiskEngine::check(const OrderRequest& req, const std::string& symbol, const std::string& mpid,
                                       const OrderBookV2& book) const {
    if (req.qty > cfg_.max_order_size) {
        return Verdict{false, RejectReason::OrderTooLarge,
                       "qty " + std::to_string(req.qty) + " exceeds max_order_size " +
                           std::to_string(cfg_.max_order_size)};
    }
    if (req.price < cfg_.min_price || req.price > cfg_.max_price) {
        return Verdict{false, RejectReason::PriceOutOfBounds,
                       "price " + std::to_string(req.price) + " outside [" + std::to_string(cfg_.min_price) + ", " +
                           std::to_string(cfg_.max_price) + "]"};
    }

    // Price band: compare against the symbol's last real trade price if
    // we have one, else the book's current best bid/ask (whichever side
    // this order would rest against isn't crossed by definition, so the
    // OPPOSITE side's touch is the more meaningful reference — but for a
    // one-sided or empty book there's nothing to band against yet, and
    // that's a real, correct case to skip, not an oversight: the very
    // first order in a symbol cannot be judged against a reference price
    // that doesn't exist.
    auto it = last_trade_price_.find(symbol);
    Price reference = 0;
    bool have_reference = false;
    if (it != last_trade_price_.end()) {
        reference = it->second;
        have_reference = true;
    } else if (book.has_bid() && book.has_ask()) {
        reference = (book.best_bid() + book.best_ask()) / 2;
        have_reference = true;
    } else if (book.has_bid()) {
        reference = book.best_bid();
        have_reference = true;
    } else if (book.has_ask()) {
        reference = book.best_ask();
        have_reference = true;
    }
    if (have_reference && reference > 0) {
        const double deviation = std::abs(static_cast<double>(req.price) - static_cast<double>(reference)) /
                                  static_cast<double>(reference);
        if (deviation > cfg_.price_band_pct) {
            return Verdict{false, RejectReason::PriceBandViolation,
                           "price " + std::to_string(req.price) + " is " + std::to_string(deviation * 100.0) +
                               "% away from reference " + std::to_string(reference) + " (band " +
                               std::to_string(cfg_.price_band_pct * 100.0) + "%)"};
        }
    }

    // Self-trade prevention — only for MPID-attributed orders, see file
    // header comment. Reject if this same participant already rests on
    // the opposite side of this symbol.
    if (!mpid.empty()) {
        const Side opposite = (req.side == Side::Buy) ? Side::Sell : Side::Buy;
        auto rit = resting_count_.find(key(symbol, mpid, opposite));
        if (rit != resting_count_.end() && rit->second > 0) {
            return Verdict{false, RejectReason::SelfTrade,
                           "mpid " + mpid + " already rests on the opposite side of " + symbol};
        }
    }

    return Verdict{true, RejectReason::None, ""};
}

void RiskEngine::on_trade(const std::string& symbol, Price price) { last_trade_price_[symbol] = price; }

void RiskEngine::note_resting(const std::string& symbol, const std::string& mpid, Side side) {
    if (mpid.empty()) return;
    ++resting_count_[key(symbol, mpid, side)];
}

void RiskEngine::note_removed(const std::string& symbol, const std::string& mpid, Side side) {
    if (mpid.empty()) return;
    auto it = resting_count_.find(key(symbol, mpid, side));
    if (it != resting_count_.end() && it->second > 0) --it->second;
}
