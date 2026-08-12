#include "order_book_v2.h"
#include <cassert>
#include <iostream>
#include <random>

static uint64_t rng_id() {
    static std::mt19937_64 g{42};
    return g();
}

static OrderRequest lim(Side s, Price p, Quantity q) {
    return { rng_id(), s, Type::Limit, p, q };
}

void test_basic_match() {
    OrderBookV2 b;
    assert(b.add(lim(Side::Sell, 100, 10)).empty());  // rests
    auto t = b.add(lim(Side::Buy, 100, 5));
    assert(t.size() == 1);
    assert(t[0].price == 100 && t[0].qty == 5);
    assert(b.best_ask() == 100);  // 5 left
}

void test_price_time_priority() {
    OrderBookV2 b;
    b.add({1, Side::Sell, Type::Limit, 100, 5});
    b.add({2, Side::Sell, Type::Limit, 100, 5});
    b.add({3, Side::Sell, Type::Limit, 101, 5});
    auto t = b.add({4, Side::Buy, Type::Limit, 101, 8});
    assert(t.size() == 2);
    assert(t[0].maker_id == 1 && t[0].qty == 5);
    assert(t[1].maker_id == 2 && t[1].qty == 3);
}

void test_market_sweep() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    b.add(lim(Side::Sell, 101, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::Market, 0, 7});
    assert(t.size() == 2);
    assert(t[0].price == 100 && t[0].qty == 5);
    assert(t[1].price == 101 && t[1].qty == 2);
}

void test_no_cross_rests() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add(lim(Side::Buy, 99, 5));
    assert(t.empty());
    assert(b.best_bid() == 99);
    assert(b.best_ask() == 100);
}

void test_full_fill_does_not_rest() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add(lim(Side::Buy, 100, 5));
    assert(t.size() == 1 && t[0].qty == 5);
    assert(b.depth() == 0);
    assert(!b.has_bid());
    assert(!b.has_ask());
}

// v2-specific: force a front rebase (price below the initial window) and a
// back growth (price above it), verify the book is still correct after
// both, including that open orders' cached level_idx survived the shift.
void test_rebase_grows_correctly() {
    OrderBookV2 b(1u << 10, /*initial_window=*/100);  // tiny window on purpose
    b.add(lim(Side::Buy, 1000, 3));    // establishes base_ around 1000
    b.add(lim(Side::Buy, 900, 4));     // below window -> front rebase
    b.add(lim(Side::Sell, 1200, 6));   // above window -> back growth
    assert(b.best_bid() == 1000);
    assert(b.best_ask() == 1200);
    assert(b.depth() == 3);
}

// Same rebase scenario but tracking IDs explicitly, to prove level_idx
// bookkeeping survives a front rebase.
void test_rebase_preserves_open_orders() {
    OrderBookV2 b(1u << 10, 100);
    OrderRequest o1{101, Side::Buy, Type::Limit, 1000, 3};
    b.add(o1);
    OrderRequest o2{102, Side::Buy, Type::Limit, 900, 4};  // triggers front rebase
    b.add(o2);

    // o1 (id 101) should still be findable/cancellable after the rebase
    assert(b.cancel(101) == true);
    assert(b.best_bid() == 900);  // only o2 left
    assert(b.depth() == 1);
}

// reduce() must shrink the resting order's quantity without touching its
// place in the FIFO queue — a later-arriving order at the same price must
// still trade after the reduced one, not before it.
void test_reduce_keeps_time_priority() {
    OrderBookV2 b;
    b.add({1, Side::Sell, Type::Limit, 100, 10});
    assert(b.reduce(1, 4) == true);   // 10 -> 6, id 1 keeps its queue position
    b.add({2, Side::Sell, Type::Limit, 100, 5});  // arrives after the reduce

    auto t = b.add({3, Side::Buy, Type::Limit, 100, 8});
    // id 1 (6 left) must fill first, then 2 from id 2 — NOT id 2 first
    assert(t.size() == 2);
    assert(t[0].maker_id == 1 && t[0].qty == 6);
    assert(t[1].maker_id == 2 && t[1].qty == 2);
}

// reduce() past the remaining quantity must behave exactly like cancel().
void test_reduce_to_zero_acts_like_cancel() {
    OrderBookV2 b;
    b.add({1, Side::Sell, Type::Limit, 100, 5});
    assert(b.reduce(1, 5) == true);   // reduces to exactly zero
    assert(b.depth() == 0);
    assert(!b.has_ask());

    b.add({2, Side::Sell, Type::Limit, 100, 5});
    assert(b.reduce(2, 999) == true);  // delta larger than remaining qty
    assert(b.depth() == 0);
}

void test_reduce_unknown_id_returns_false() {
    OrderBookV2 b;
    assert(b.reduce(999, 1) == false);
}

// IOC: fills what's available immediately, discards the remainder, never
// rests — depth must stay at 0 even though 2 units went unfilled.
void test_ioc_discards_remainder() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::IOC, 100, 7});
    assert(t.size() == 1 && t[0].qty == 5);
    assert(b.depth() == 0);
    assert(!b.has_bid());
}

// IOC with nothing to match against: no trades, nothing rests either.
void test_ioc_no_cross_does_nothing() {
    OrderBookV2 b;
    auto t = b.add({rng_id(), Side::Buy, Type::IOC, 100, 5});
    assert(t.empty());
    assert(b.depth() == 0);
}

// FOK: the resting liquidity (5) is less than the requested qty (10) at a
// crossable price — must reject entirely, no partial fill, nothing rests.
void test_fok_rejects_when_not_fully_fillable() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::FOK, 100, 10});
    assert(t.empty());
    assert(b.depth() == 1);   // the resting sell is untouched
    assert(b.best_ask() == 100);
}

// FOK: exactly enough liquidity across two price levels within the limit
// price — must fully fill, using both levels.
void test_fok_fills_when_fully_available() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    b.add(lim(Side::Sell, 101, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::FOK, 101, 10});
    assert(t.size() == 2);
    assert(t[0].price == 100 && t[0].qty == 5);
    assert(t[1].price == 101 && t[1].qty == 5);
    assert(b.depth() == 0);
}

// Post-Only: doesn't cross the resting ask -> rests directly, no trade.
void test_post_only_rests_when_no_cross() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::PostOnly, 99, 5});
    assert(t.empty());
    assert(b.best_bid() == 99);
    assert(b.depth() == 2);
}

// Post-Only: would cross the resting ask on arrival -> rejected outright,
// nothing rests, the resting ask is untouched (no trade happened either).
void test_post_only_rejects_when_crossing() {
    OrderBookV2 b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::PostOnly, 101, 5});
    assert(t.empty());
    assert(!b.has_bid());
    assert(b.depth() == 1);
    assert(b.best_ask() == 100);
}

int main() {
    test_basic_match();
    test_price_time_priority();
    test_market_sweep();
    test_no_cross_rests();
    test_full_fill_does_not_rest();
    test_rebase_grows_correctly();
    test_rebase_preserves_open_orders();
    test_reduce_keeps_time_priority();
    test_reduce_to_zero_acts_like_cancel();
    test_reduce_unknown_id_returns_false();
    test_ioc_discards_remainder();
    test_ioc_no_cross_does_nothing();
    test_fok_rejects_when_not_fully_fillable();
    test_fok_fills_when_fully_available();
    test_post_only_rests_when_no_cross();
    test_post_only_rejects_when_crossing();
    std::cout << "all 16 v2 tests passed\n";
    return 0;
}
