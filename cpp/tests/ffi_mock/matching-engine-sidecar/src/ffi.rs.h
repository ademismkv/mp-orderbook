#pragma once
// Hand-written stand-in for what cxx_build generates from rust/src/ffi.rs at
// Rust-build time. Field-for-field identical to the #[cxx::bridge] shared
// types declared there. Exists so the real, unmodified
// cpp/src/order_book_v2_ffi.cpp can be compiled and run in an environment
// with no cargo/rustc (this one) — see
// cpp/tests/test_order_book_v2_ffi_standalone.cpp for what it proves and
// what it deliberately does not.
#include <cstdint>

enum class FfiSide { Buy, Sell };
enum class FfiOrderType { Limit, Market, Cancel };

struct FfiOrderRequest {
    uint64_t id;
    FfiSide side;
    FfiOrderType kind;
    int64_t price;
    uint64_t qty;
};

struct FfiTrade {
    uint64_t maker_id;
    uint64_t taker_id;
    int64_t price;
    uint64_t qty;
};
