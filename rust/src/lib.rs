//! Sidecar — wired to the C++ matching engine over `cxx` (ADR-3).
//!
//! `ffi` is the raw cxx bridge (src/ffi.rs, generated types + the opaque
//! `OrderBookV2Ffi` C++ type). `MatchingEngine` below is a thin, safe Rust
//! wrapper around the generated `UniquePtr<...>` so callers don't touch
//! `Pin`/`UniquePtr` directly — it forwards every call straight through the
//! FFI boundary into the real, already fuzz-tested + TSan/ASan-verified
//! OrderBookV2. It does not reimplement any matching logic.
//!
//! `fix` is a minimal FIX 4.4 inbound parser (NewOrderSingle / Cancel /
//! CancelReplace — enough to place, cancel, or modify a limit order, per
//! ADR-4's explicit scope cut: inbound only, not a full spec implementation).
//!
//! `risk` is the pre-trade risk pre-check ADR-3 leaves as an open decision,
//! resolved there as: async, ingestion-side, before an order reaches the
//! matching thread — not synchronous-blocking inside the matching thread's
//! own latency budget.
//!
//! Honest status: this crate has NOT been built in the dev sandbox this was
//! written in — there is no rustc/cargo here (see README). The trivial
//! skeleton that existed before this session built cleanly on the user's
//! machine with zero fixes needed; this FFI-wired version is meaningfully
//! more complex (cross-language codegen via cxx_build, a C++ compile step
//! launched from build.rs) and has a real chance of needing a build-fix-retry
//! loop the first time `cargo build` actually runs it. That loop — paste the
//! compiler error back, get a fix — is the expected next step, not a sign
//! something went wrong.

pub mod ffi;
pub mod fix;
pub mod risk;

use cxx::UniquePtr;
use ffi::{FfiOrderRequest, FfiOrderType, FfiSide, FfiTrade};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Buy,
    Sell,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OrderType {
    Limit,
    Market,
    Cancel,
}

#[derive(Debug, Clone, Copy)]
pub struct OrderRequest {
    pub id: u64,
    pub side: Side,
    pub kind: OrderType,
    pub price: i64,
    pub qty: u64,
}

#[derive(Debug, Clone, Copy)]
pub struct Trade {
    pub maker_id: u64,
    pub taker_id: u64,
    pub price: i64,
    pub qty: u64,
}

/// Safe wrapper around the FFI-owned C++ order book. One per symbol, same as
/// ADR-1's single-writer-per-symbol model on the C++ side — this type is not
/// `Sync`, and isn't meant to be shared across threads without the same
/// per-symbol-ownership discipline the rest of the engine already uses.
pub struct MatchingEngine {
    inner: UniquePtr<ffi::OrderBookV2Ffi>,
}

impl MatchingEngine {
    pub fn new(arena_capacity: u32, initial_window: i64) -> Self {
        Self {
            inner: ffi::make_order_book(arena_capacity, initial_window),
        }
    }

    pub fn add(&mut self, req: OrderRequest) -> Vec<Trade> {
        let ffi_req = FfiOrderRequest {
            id: req.id,
            side: match req.side {
                Side::Buy => FfiSide::Buy,
                Side::Sell => FfiSide::Sell,
            },
            kind: match req.kind {
                OrderType::Limit => FfiOrderType::Limit,
                OrderType::Market => FfiOrderType::Market,
                OrderType::Cancel => FfiOrderType::Cancel,
            },
            price: req.price,
            qty: req.qty,
        };

        self.inner
            .pin_mut()
            .add(ffi_req)
            .into_iter()
            .map(|t: FfiTrade| Trade {
                maker_id: t.maker_id,
                taker_id: t.taker_id,
                price: t.price,
                qty: t.qty,
            })
            .collect()
    }

    pub fn cancel(&mut self, id: u64) -> bool {
        self.inner.pin_mut().cancel(id)
    }

    pub fn reduce(&mut self, id: u64, delta: u64) -> bool {
        self.inner.pin_mut().reduce(id, delta)
    }

    pub fn best_bid(&self) -> i64 {
        self.inner.best_bid()
    }

    pub fn best_ask(&self) -> i64 {
        self.inner.best_ask()
    }

    pub fn has_bid(&self) -> bool {
        self.inner.has_bid()
    }

    pub fn has_ask(&self) -> bool {
        self.inner.has_ask()
    }

    pub fn depth(&self) -> usize {
        self.inner.depth()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crosses_and_reports_a_trade() {
        let mut eng = MatchingEngine::new(1024, 2000);
        eng.add(OrderRequest {
            id: 1,
            side: Side::Sell,
            kind: OrderType::Limit,
            price: 100,
            qty: 10,
        });
        let trades = eng.add(OrderRequest {
            id: 2,
            side: Side::Buy,
            kind: OrderType::Limit,
            price: 100,
            qty: 10,
        });
        assert_eq!(trades.len(), 1);
        assert_eq!(trades[0].qty, 10);
        assert_eq!(trades[0].maker_id, 1);
        assert_eq!(trades[0].taker_id, 2);
        assert!(!eng.has_bid());
        assert!(!eng.has_ask());
    }

    #[test]
    fn non_crossing_order_rests() {
        let mut eng = MatchingEngine::new(1024, 2000);
        let trades = eng.add(OrderRequest {
            id: 1,
            side: Side::Buy,
            kind: OrderType::Limit,
            price: 99,
            qty: 5,
        });
        assert!(trades.is_empty());
        assert!(eng.has_bid());
        assert_eq!(eng.best_bid(), 99);
    }
}
