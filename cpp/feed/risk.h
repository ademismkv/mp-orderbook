#pragma once
#include "order_book_v2.h"

#include <string>
#include <unordered_map>

// Pre-trade risk: the stage between Sequencer and Order Books in
// ROADMAP.md's pipeline (Feed -> Sequencer -> Order Books -> Risk ->
// Execution Reports -> Market Data Feed). Runs BEFORE an order reaches
// OrderBookV2::add() — a real exchange rejects a bad order here, it
// doesn't let the book find out the hard way. OrderBookV2 has its own
// kMaxLevels backstop now (see devlog day 17), but that's a last line of
// defense for a bug-shaped problem, not a substitute for a real risk
// stage catching a bad *order* before it's ever submitted.
//
// Honest scope note on self-trade prevention: it needs to know WHO
// submitted an order. Real ITCH's Add Order — No MPID Attribution (the
// large majority of real order flow — 183,724 of 183,767 Add Orders in
// this repo's real sample) carries no participant identity at all; only
// Add Order — MPID Attribution (43 of 183,767) does. Self-trade
// prevention here only runs where the feed actually gives an identity to
// check — it's correctly scoped to what real data supports, not a no-op
// fake covering the anonymous majority. It's also a best-effort tracker,
// not a perfect position lifecycle: it updates on Add (establishes a
// resting position) and Order Delete (removes one), but doesn't attempt
// to detect an Order Executed draining a tracked order fully to zero —
// stated here rather than silently assumed correct in every case.
class RiskEngine {
public:
    struct Config {
        Quantity max_order_size = 1'000'000;    // fat-finger: reject a single order above this
        Price min_price = 1;                     // price must be positive
        Price max_price = 2'000'000'000;          // absolute sanity ceiling ($200,000.0000 in Price(4) ticks)
        double price_band_pct = 0.25;             // reject if price is more than this fraction away
                                                   // from the symbol's last known reference price
    };

    enum class RejectReason { None, OrderTooLarge, PriceOutOfBounds, PriceBandViolation, SelfTrade };

    struct Verdict {
        bool accepted = true;
        RejectReason reason = RejectReason::None;
        std::string detail;
    };

    // Out-of-line + delegating rather than a `Config cfg = {}` default
    // argument: GCC rejects aggregate-init default arguments referring to
    // a nested type's own default member initializers from inside the
    // enclosing class body ("default member initializer required before
    // the end of its enclosing class") — a real, reproducible compiler
    // quirk hit while building this, not a style preference.
    RiskEngine();
    explicit RiskEngine(Config cfg);

    // `mpid` is empty for Add Order — No MPID (the common case) — self-
    // trade prevention is skipped for those, not faked. `book` lets the
    // price-band check fall back to the symbol's current best bid/ask
    // when there's no trade history yet (a book with no trades at all
    // has no reference price to band against).
    Verdict check(const OrderRequest& req, const std::string& symbol, const std::string& mpid,
                   const OrderBookV2& book) const;

    // Sequencer calls this after a real trade executes, so future
    // price-band checks track a moving reference price instead of only
    // the first price this engine ever saw for that symbol.
    void on_trade(const std::string& symbol, Price price);

    // Sequencer calls these when an MPID-attributed order actually rests
    // or leaves the book (Add / Order Delete respectively — see class
    // comment on scope), keeping the self-trade check's view accurate.
    void note_resting(const std::string& symbol, const std::string& mpid, Side side);
    void note_removed(const std::string& symbol, const std::string& mpid, Side side);

private:
    static std::string key(const std::string& symbol, const std::string& mpid, Side side) {
        std::string k = symbol;
        k += '\x01';
        k += mpid;
        k += '\x01';
        k += (side == Side::Buy) ? 'B' : 'S';
        return k;
    }

    Config cfg_;
    std::unordered_map<std::string, Price> last_trade_price_;
    std::unordered_map<std::string, int> resting_count_;   // key(symbol,mpid,side) -> count
};
