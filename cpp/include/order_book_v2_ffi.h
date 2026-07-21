#pragma once
// FFI adapter — the boundary described in ADR-3.
//
// This deliberately does NOT touch OrderBookV2 itself (already fuzz-tested
// against v1 and clean under ThreadSanitizer/AddressSanitizer — see ADR-2 and
// the devlog). It only translates between cxx's generated mirror types
// (FfiOrderRequest, FfiTrade, FfiSide, FfiOrderType — declared in
// rust/src/ffi.rs) and the real ::OrderRequest / ::Trade / ::Side / ::Type
// used everywhere else in cpp/. If this file has a bug, the matching logic
// underneath is still provably correct; only the translation layer is new
// and unverified.
//
// Included from two places:
//   1. cpp/src/order_book_v2_ffi.cpp (the implementation)
//   2. rust/build.rs, via cxx_build's generated bridge, when compiling the
//      Rust sidecar — see rust/src/ffi.rs's `include!(...)`.
//
// NOTE: this header deliberately does NOT include the cxx_build-generated
// matching-engine-sidecar/src/ffi.rs.h. cxx's `include!("order_book_v2_ffi.h")`
// directive makes cxx_build insert `#include "order_book_v2_ffi.h"` directly
// into its generated ffi.rs.cc, and expects THIS header to already fully
// declare `class OrderBookV2Ffi` by the time ffi.rs.h is included afterward
// (ffi.rs.h ends with `using OrderBookV2Ffi = ::OrderBookV2Ffi;`, which
// requires the real class to already exist). An earlier version of this file
// included ffi.rs.h itself, here, before its own class declaration — with
// `#pragma once` on both sides, that circular include meant ffi.rs.h's alias
// line ran before this file's `class OrderBookV2Ffi` (further down this same
// file) had been seen, producing a real `cargo build` error: "no type named
// 'OrderBookV2Ffi' in the global namespace" followed by a conflicting-alias
// error. Forward declarations below are enough for the method signatures in
// this header (by-value parameters/returns of an incomplete type are legal
// in a *declaration*); the complete struct definitions only need to be
// visible where fields are actually read or written — see
// cpp/src/order_book_v2_ffi.cpp, which includes ffi.rs.h directly, after
// this header, once OrderBookV2Ffi already exists.
struct FfiOrderRequest;
struct FfiTrade;

#include "order_book_v2.h"
#include "rust/cxx.h"

#include <cstdint>
#include <memory>

class OrderBookV2Ffi {
public:
    OrderBookV2Ffi(uint32_t arena_capacity, int64_t initial_window);

    rust::Vec<FfiTrade> add(FfiOrderRequest req);
    bool cancel(uint64_t id);
    bool reduce(uint64_t id, uint64_t delta);

    int64_t best_bid() const;
    int64_t best_ask() const;
    bool    has_bid() const;
    bool    has_ask() const;
    size_t  depth() const;

private:
    OrderBookV2 inner_;
    // Reused across add() calls so inner_'s fills don't force a fresh
    // std::vector<Trade> allocation on every call (day 11 profiling fix —
    // see order_book_v2.h's add(req, out_trades) overload).
    std::vector<Trade> trades_scratch_;
};

// Factory — cxx doesn't support constructors directly, so Rust calls this
// free function to get a UniquePtr<OrderBookV2Ffi>. See rust/src/ffi.rs.
std::unique_ptr<OrderBookV2Ffi> make_order_book(uint32_t arena_capacity, int64_t initial_window);
