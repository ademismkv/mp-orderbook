//! Pre-trade risk pre-check — ADR-3's "open decision" resolved.
//!
//! ADR-3 leaves open whether pre-trade risk is synchronous-blocking on the
//! matching thread, or an async pre-check that rejects bad orders before
//! they ever reach the per-symbol ring buffer. This module implements the
//! latter: it runs on the ingestion side, before an order is handed to
//! `MatchingEngine`, and never touches the matching thread's own latency
//! budget. Cross-symbol/portfolio-level checks are explicitly out of scope
//! here too (ADR-1: "handled at the gateway/risk layer, not inside
//! matching") — this module *is* that gateway layer, but only the
//! single-order slice of it (no position tracking, no netting, no margin).
//!
//! Three checks, each independently meaningful on its own:
//! - max order size (per order, symbol-agnostic)
//! - max notional (price * qty)
//! - price collar (how far the order's price may sit from a reference price,
//!   in basis points) — the classic "fat finger" guard
//! Symbol allowlist is enforced separately since it's a routing decision,
//! not a sizing one.

use std::collections::HashSet;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RiskReject {
    SymbolNotAllowed,
    OrderTooLarge { qty: u64, limit: u64 },
    NotionalTooLarge { notional: i64, limit: i64 },
    PriceOutsideCollar { price: i64, reference: i64, collar_bps: u32 },
}

#[derive(Debug, Clone)]
pub struct RiskLimits {
    pub max_order_qty: u64,
    /// Same price-tick units as `Price` elsewhere in this repo (ticks of
    /// $0.0001) multiplied by quantity — so this is in ten-thousandths of a
    /// dollar, not dollars. Callers building this from a dollar figure
    /// should multiply by 10_000 first.
    pub max_notional: i64,
    /// Basis points away from the reference price before an order is
    /// rejected. 500 = 5%.
    pub price_collar_bps: u32,
    /// Empty means "no allowlist configured" (every symbol passes this
    /// check) — this is a deliberate default-open choice, not an oversight;
    /// callers that want default-closed behavior should populate this
    /// before accepting any orders.
    pub allowed_symbols: HashSet<String>,
}

impl RiskLimits {
    pub fn new(max_order_qty: u64, max_notional: i64, price_collar_bps: u32) -> Self {
        Self {
            max_order_qty,
            max_notional,
            price_collar_bps,
            allowed_symbols: HashSet::new(),
        }
    }

    pub fn allow_symbol(mut self, symbol: impl Into<String>) -> Self {
        self.allowed_symbols.insert(symbol.into());
        self
    }
}

/// The minimal context a risk check needs. Deliberately smaller than
/// `fix::ParsedFixMessage` or `MatchingEngine`'s `OrderRequest` — this
/// module doesn't need order type or FIX-specific fields, just the numbers
/// a risk check actually evaluates.
pub struct RiskCheckInput {
    pub symbol: String,
    pub qty: u64,
    /// For limit orders, the limit price. For market orders, callers should
    /// pass the current reference price itself (there's no independent
    /// price to collar-check against) — the notional/size checks still
    /// apply either way.
    pub price: i64,
}

/// Runs all checks in a fixed order, returning the first failure. Order
/// matters for debuggability (a caller fixing symbol allowlisting first,
/// then size, is a more sensible loop than getting all four errors at
/// once) but has no correctness implications — all four are independent.
///
/// `reference_price` is the book's current best bid/ask (or last trade);
/// `None` means the book has no price yet (e.g. first order of the day),
/// in which case the collar check is skipped since there's nothing to
/// compare against — symbol/size/notional checks still run.
pub fn check(
    limits: &RiskLimits,
    input: &RiskCheckInput,
    reference_price: Option<i64>,
) -> Result<(), RiskReject> {
    if !limits.allowed_symbols.is_empty() && !limits.allowed_symbols.contains(&input.symbol) {
        return Err(RiskReject::SymbolNotAllowed);
    }

    if input.qty > limits.max_order_qty {
        return Err(RiskReject::OrderTooLarge {
            qty: input.qty,
            limit: limits.max_order_qty,
        });
    }

    let notional = input.price.saturating_mul(input.qty as i64);
    if notional > limits.max_notional {
        return Err(RiskReject::NotionalTooLarge {
            notional,
            limit: limits.max_notional,
        });
    }

    if let Some(reference) = reference_price {
        if reference > 0 {
            let diff = (input.price - reference).unsigned_abs() as i128;
            let allowed = (reference as i128).abs() * limits.price_collar_bps as i128 / 10_000;
            if diff > allowed {
                return Err(RiskReject::PriceOutsideCollar {
                    price: input.price,
                    reference,
                    collar_bps: limits.price_collar_bps,
                });
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limits() -> RiskLimits {
        RiskLimits::new(10_000, 1_000_000_000, 500).allow_symbol("AAPL")
    }

    #[test]
    fn accepts_order_within_all_limits() {
        let input = RiskCheckInput {
            symbol: "AAPL".into(),
            qty: 100,
            price: 5_850_000,
        };
        assert_eq!(check(&limits(), &input, Some(5_850_000)), Ok(()));
    }

    #[test]
    fn rejects_disallowed_symbol() {
        let input = RiskCheckInput {
            symbol: "TSLA".into(),
            qty: 100,
            price: 5_850_000,
        };
        assert_eq!(
            check(&limits(), &input, Some(5_850_000)),
            Err(RiskReject::SymbolNotAllowed)
        );
    }

    #[test]
    fn rejects_oversized_order() {
        let input = RiskCheckInput {
            symbol: "AAPL".into(),
            qty: 20_000,
            price: 5_850_000,
        };
        assert_eq!(
            check(&limits(), &input, Some(5_850_000)),
            Err(RiskReject::OrderTooLarge { qty: 20_000, limit: 10_000 })
        );
    }

    #[test]
    fn rejects_price_outside_collar() {
        // reference $585.00, 5% collar -> allowed band is roughly [$555.75, $614.25]
        let input = RiskCheckInput {
            symbol: "AAPL".into(),
            qty: 10,
            price: 8_000_000, // $800
        };
        let result = check(&limits(), &input, Some(5_850_000));
        assert!(matches!(result, Err(RiskReject::PriceOutsideCollar { .. })));
    }

    #[test]
    fn skips_collar_check_with_no_reference_price() {
        let input = RiskCheckInput {
            symbol: "AAPL".into(),
            qty: 10,
            price: 999_999_999,
        };
        // no reference price yet (empty book) -> collar can't be evaluated;
        // notional check still applies and this qty*price exceeds it
        assert!(matches!(
            check(&limits(), &input, None),
            Err(RiskReject::NotionalTooLarge { .. })
        ));
    }
}
