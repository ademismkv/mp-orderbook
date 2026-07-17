#include "order_book.h"
#include <cassert>
#include <iostream>
#include <random>

static uint64_t rng_id() {
    static std::mt19937_64 g{42};
    return g();
}

static Order lim(Side s, double p, uint64_t q) {
    return { rng_id(), s, Type::Limit, p, q, 0 };
}

void test_basic_match() {
    OrderBook b;
    assert(b.add(lim(Side::Sell, 100, 10)).empty());  // rests
    auto t = b.add(lim(Side::Buy, 100, 5));
    assert(t.size() == 1);
    assert(t[0].price == 100 && t[0].qty == 5);
    assert(b.best_ask() == 100);  // 5 left
}

void test_price_time_priority() {
    OrderBook b;
    b.add({1, Side::Sell, Type::Limit, 100, 5, 0});
    b.add({2, Side::Sell, Type::Limit, 100, 5, 0});
    b.add({3, Side::Sell, Type::Limit, 101, 5, 0});
    auto t = b.add({4, Side::Buy, Type::Limit, 101, 8, 0});
    // FIFO at 100: id 1 fully, then 3 from id 2
    assert(t.size() == 2);
    assert(t[0].maker_id == 1 && t[0].qty == 5);
    assert(t[1].maker_id == 2 && t[1].qty == 3);
}

void test_market_sweep() {
    OrderBook b;
    b.add(lim(Side::Sell, 100, 5));
    b.add(lim(Side::Sell, 101, 5));
    auto t = b.add({rng_id(), Side::Buy, Type::Market, 0, 7, 0});
    // 5 @ 100, then 2 @ 101
    assert(t.size() == 2);
    assert(t[0].price == 100 && t[0].qty == 5);
    assert(t[1].price == 101 && t[1].qty == 2);
}

void test_no_cross_rests() {
    OrderBook b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add(lim(Side::Buy, 99, 5));
    assert(t.empty());
    assert(b.best_bid() == 99);
    assert(b.best_ask() == 100);
}

// Regression test: a taker that fills COMPLETELY must not also rest on the
// book. The original draft passed `Order taker` by value into match(), so
// add()'s own copy of taker.qty never reflected the fill and a phantom
// resting order got pushed at the full original quantity. Fixed by taking
// taker by reference in match(). None of the first 4 tests caught this
// because they only assert on the returned trades, not on book state after
// a full fill.
void test_full_fill_does_not_rest() {
    OrderBook b;
    b.add(lim(Side::Sell, 100, 5));
    auto t = b.add(lim(Side::Buy, 100, 5));  // exact full fill, no remainder
    assert(t.size() == 1 && t[0].qty == 5);
    assert(b.depth() == 0);       // nothing should be resting on either side
    assert(b.best_bid() == 0.0);  // no phantom bid left behind
    assert(b.best_ask() == 0.0);
}

int main() {
    test_basic_match();
    test_price_time_priority();
    test_market_sweep();
    test_no_cross_rests();
    test_full_fill_does_not_rest();
    std::cout << "all 5 tests passed\n";
    return 0;
}
