//! The `cxx` bridge itself — ADR-3's "control plane" boundary.
//!
//! This module declares shared types (not reused C++ enums — cxx requires
//! shared enums/structs to be defined inside the bridge macro itself, so
//! these are deliberately separate from ::Side / ::Type / ::OrderRequest /
//! ::Trade in cpp/include/order_book_v2.h) plus the opaque `OrderBookV2Ffi`
//! C++ type and the functions exposed on it. The actual field-by-field
//! translation into the real, already-verified OrderBookV2 lives in
//! cpp/src/order_book_v2_ffi.cpp — this file only declares the shape of the
//! boundary, it contains no matching logic.
//!
//! Compiled by rust/build.rs via `cxx_build::bridge("src/ffi.rs")`, which
//! also compiles cpp/src/order_book_v2.cpp and
//! cpp/src/order_book_v2_ffi.cpp directly (a separate, independent build
//! from the CMake one in cpp/ — this crate does not depend on CMake).

// NOTE: no `namespace = "..."` here, deliberately. cxx would otherwise
// generate every type in the bridge inside that namespace and expect the
// C++ side to match — but OrderBookV2Ffi (cpp/include/order_book_v2_ffi.h)
// and FfiSide/FfiOrderType/FfiOrderRequest/FfiTrade are declared at global
// scope there, not inside `namespace ffi {}`. An earlier version of this
// file specified `namespace = "ffi"`, which compiled fine against this
// crate's own hand-written mock headers (cpp/tests/ffi_mock/) because
// those mocks made the identical mistake — but failed the moment a real
// `cargo build` ran cxx_build's actual codegen against the real C++
// header, with errors like "no member named 'OrderBookV2Ffi' in namespace
// 'ffi'". Fixed by dropping the namespace here instead of adding one to
// the C++ side, since that's the smaller, lower-risk change.
#[cxx::bridge]
mod bridge {
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiSide {
        Buy,
        Sell,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiOrderType {
        Limit,
        Market,
        Cancel,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiOrderRequest {
        id: u64,
        side: FfiSide,
        kind: FfiOrderType,
        price: i64,
        qty: u64,
    }

    #[derive(Debug, Clone, Copy)]
    struct FfiTrade {
        maker_id: u64,
        taker_id: u64,
        price: i64,
        qty: u64,
    }

    unsafe extern "C++" {
        include!("order_book_v2_ffi.h");

        type OrderBookV2Ffi;

        fn make_order_book(arena_capacity: u32, initial_window: i64) -> UniquePtr<OrderBookV2Ffi>;

        fn add(self: Pin<&mut OrderBookV2Ffi>, req: FfiOrderRequest) -> Vec<FfiTrade>;
        fn cancel(self: Pin<&mut OrderBookV2Ffi>, id: u64) -> bool;
        fn reduce(self: Pin<&mut OrderBookV2Ffi>, id: u64, delta: u64) -> bool;

        fn best_bid(self: &OrderBookV2Ffi) -> i64;
        fn best_ask(self: &OrderBookV2Ffi) -> i64;
        fn has_bid(self: &OrderBookV2Ffi) -> bool;
        fn has_ask(self: &OrderBookV2Ffi) -> bool;
        fn depth(self: &OrderBookV2Ffi) -> usize;
    }
}

pub use bridge::{make_order_book, FfiOrderRequest, FfiOrderType, FfiSide, FfiTrade, OrderBookV2Ffi};
