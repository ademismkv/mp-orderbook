// Standalone verification of the FFI adapter's C++ half
// (cpp/src/order_book_v2_ffi.cpp) WITHOUT requiring cargo/rustc.
//
// Compiles the real, unmodified adapter against a hand-written mock of the
// two headers cxx_build would otherwise generate (rust/cxx.h and
// matching-engine-sidecar/src/ffi.rs.h — see cpp/tests/ffi_mock/), field-for-
// field identical to what rust/src/ffi.rs actually declares.
//
// What this proves: the adapter's translation logic (FfiSide/FfiOrderType ->
// ::Side/::Type, trade collection into the Vec-like return type, pass-through
// of cancel/reduce/best_bid/best_ask/has_bid/has_ask/depth) compiles and
// behaves correctly against the real OrderBookV2.
//
// What this does NOT prove: that the real `cxx` bridge compiles. That still
// needs cargo, which is confirmed unavailable in this environment (see
// devlog/2026-07-17-day-6-ffi-fix-risk-dashboard.md and the fresh retest
// logged there) — no rustc, no cargo, rustup.rs/static.rust-lang.org/
// crates.io all blocked by the sandbox's network allowlist. Two independent
// verification claims; this test only covers the C++ side.

#include "order_book_v2_ffi.h"
#include <cassert>
#include <cstdio>

int main() {
    OrderBookV2Ffi book(1024, 2000);

    // Resting order, then a crossing order that should produce exactly one trade.
    FfiOrderRequest sell{1, FfiSide::Sell, FfiOrderType::Limit, 100, 10};
    auto t1 = book.add(sell);
    assert(t1.size() == 0);
    assert(book.has_ask());
    assert(book.best_ask() == 100);

    FfiOrderRequest buy{2, FfiSide::Buy, FfiOrderType::Limit, 100, 10};
    auto t2 = book.add(buy);
    assert(t2.size() == 1);
    assert(t2[0].maker_id == 1);
    assert(t2[0].taker_id == 2);
    assert(t2[0].qty == 10);
    assert(t2[0].price == 100);
    assert(!book.has_ask());
    assert(!book.has_bid());

    // Resting order, then reduce and cancel through the FFI-typed API.
    FfiOrderRequest rest{3, FfiSide::Buy, FfiOrderType::Limit, 90, 50};
    book.add(rest);
    assert(book.has_bid() && book.best_bid() == 90);
    assert(book.reduce(3, 20));
    assert(book.cancel(3));
    assert(!book.has_bid());
    assert(!book.reduce(999, 1)); // unknown id -> false, not a crash
    assert(!book.cancel(999));

    // depth() reflects resting orders only
    FfiOrderRequest r1{4, FfiSide::Buy, FfiOrderType::Limit, 80, 5};
    FfiOrderRequest r2{5, FfiSide::Sell, FfiOrderType::Limit, 120, 5};
    book.add(r1);
    book.add(r2);
    assert(book.depth() == 2);

    printf("all FFI-adapter standalone checks passed (compiled + run, no cargo needed)\n");
    return 0;
}
