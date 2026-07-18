#pragma once
// Hand-written stand-in for what cxx_build generates from rust/src/ffi.rs at
// Rust-build time. Field-for-field identical to the #[cxx::bridge] shared
// types declared there. Exists so the real, unmodified
// cpp/src/order_book_v2_ffi.cpp can be compiled and run in an environment
// with no cargo/rustc (this one) — see
// cpp/tests/test_order_book_v2_ffi_standalone.cpp for what it proves and
// what it deliberately does not.
//
// Update, day 8: this mock's global-scope types (no `namespace ffi {}`)
// were originally a guess at cxx_build's real output, made without ever
// having run cxx_build. That guess was wrong for one version of
// rust/src/ffi.rs, which declared `#[cxx::bridge(namespace = "ffi")]` —
// the first real `cargo build`, on the user's machine, failed with
// "no member named 'OrderBookV2Ffi' in namespace 'ffi'" because the real
// generated header put everything inside namespace ffi and this mock (and
// cpp/include/order_book_v2_ffi.h) did not. Fixed by dropping the
// namespace from the bridge macro instead, so as of that fix this mock's
// global scope is verified-accurate again, not just assumed.
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
