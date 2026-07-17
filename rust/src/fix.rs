//! Minimal FIX 4.4 inbound parser.
//!
//! Scope is deliberately narrow, per ADR-4 ("FIX support, when built: inbound
//! only, 4.4, enough fields to place/cancel/modify a limit order — not a
//! full spec implementation"). Handles exactly three message types:
//! NewOrderSingle (35=D), OrderCancelRequest (35=F), and
//! OrderCancelReplaceRequest (35=G). No session layer (logon/heartbeat/
//! sequence numbers), no checksum validation, no outbound execution reports
//! — this is the order-entry parsing slice only.

use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FixParseError {
    Empty,
    MissingTag(u32),
    UnknownMsgType(String),
    BadInteger(u32),
    BadPrice(u32),
    BadSide(String),
    BadOrdType(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FixSide {
    Buy,
    Sell,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FixOrdType {
    Limit,
    Market,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParsedFixMessage {
    NewOrderSingle {
        cl_ord_id: String,
        symbol: String,
        side: FixSide,
        ord_type: FixOrdType,
        price: Option<i64>, // ticks of $0.0001 — see parse_price(); None for Market
        qty: u64,
    },
    CancelRequest {
        cl_ord_id: String,
        orig_cl_ord_id: String,
        symbol: String,
        side: FixSide,
    },
    CancelReplaceRequest {
        cl_ord_id: String,
        orig_cl_ord_id: String,
        symbol: String,
        side: FixSide,
        ord_type: FixOrdType,
        price: Option<i64>,
        qty: u64,
    },
}

/// Splits a raw FIX message into tag->value pairs. Accepts the real SOH
/// (0x01) delimiter used on the wire; if none is present, falls back to
/// '|' so messages are readable/writable as plain Rust string literals in
/// tests and docs — a common convention, not part of the FIX spec itself.
fn split_fields(raw: &str) -> HashMap<u32, String> {
    let delim = if raw.contains('\u{1}') { '\u{1}' } else { '|' };
    raw.split(delim)
        .filter_map(|field| {
            let mut parts = field.splitn(2, '=');
            let tag = parts.next()?.trim();
            let value = parts.next()?;
            tag.parse::<u32>().ok().map(|t| (t, value.to_string()))
        })
        .collect()
}

fn get<'a>(fields: &'a HashMap<u32, String>, tag: u32) -> Result<&'a str, FixParseError> {
    fields
        .get(&tag)
        .map(|s| s.as_str())
        .ok_or(FixParseError::MissingTag(tag))
}

fn parse_side(raw: &str) -> Result<FixSide, FixParseError> {
    match raw {
        "1" => Ok(FixSide::Buy),
        "2" => Ok(FixSide::Sell),
        other => Err(FixParseError::BadSide(other.to_string())),
    }
}

fn parse_ord_type(raw: &str) -> Result<FixOrdType, FixParseError> {
    match raw {
        "1" => Ok(FixOrdType::Market),
        "2" => Ok(FixOrdType::Limit),
        other => Err(FixParseError::BadOrdType(other.to_string())),
    }
}

fn parse_qty(fields: &HashMap<u32, String>) -> Result<u64, FixParseError> {
    get(fields, 38)?
        .parse::<u64>()
        .map_err(|_| FixParseError::BadInteger(38))
}

/// Tag 44 (Price) is a decimal string, e.g. "585.0600". Converts to integer
/// ticks of $0.0001 — the same Price convention OrderBookV2 and the LOBSTER
/// replay tool (cpp/tools/replay_lobster.cpp) already use, so a
/// FIX-originated order and a replayed historical order land in the same
/// integer price space without any further conversion downstream.
fn parse_price(raw: &str) -> Result<i64, FixParseError> {
    let mut parts = raw.splitn(2, '.');
    let whole = parts.next().unwrap_or("0");
    let frac_raw = parts.next().unwrap_or("");
    let mut frac = frac_raw.to_string();
    while frac.len() < 4 {
        frac.push('0');
    }
    frac.truncate(4);

    let whole_ticks: i64 = whole.parse().map_err(|_| FixParseError::BadPrice(44))?;
    let frac_ticks: i64 = frac.parse().map_err(|_| FixParseError::BadPrice(44))?;
    let sign = if whole.starts_with('-') { -1 } else { 1 };
    Ok(whole_ticks * 10_000 + sign * frac_ticks)
}

/// Parses one inbound FIX 4.4 message. See module docs for the exact scope
/// cut (three message types, order-entry fields only).
pub fn parse(raw: &str) -> Result<ParsedFixMessage, FixParseError> {
    if raw.trim().is_empty() {
        return Err(FixParseError::Empty);
    }
    let fields = split_fields(raw);
    let msg_type = get(&fields, 35)?;

    match msg_type {
        "D" => {
            let cl_ord_id = get(&fields, 11)?.to_string();
            let symbol = get(&fields, 55)?.to_string();
            let side = parse_side(get(&fields, 54)?)?;
            let ord_type = parse_ord_type(get(&fields, 40)?)?;
            let qty = parse_qty(&fields)?;
            let price = match ord_type {
                FixOrdType::Limit => Some(parse_price(get(&fields, 44)?)?),
                FixOrdType::Market => None,
            };
            Ok(ParsedFixMessage::NewOrderSingle {
                cl_ord_id,
                symbol,
                side,
                ord_type,
                price,
                qty,
            })
        }
        "F" => {
            let cl_ord_id = get(&fields, 11)?.to_string();
            let orig_cl_ord_id = get(&fields, 41)?.to_string();
            let symbol = get(&fields, 55)?.to_string();
            let side = parse_side(get(&fields, 54)?)?;
            Ok(ParsedFixMessage::CancelRequest {
                cl_ord_id,
                orig_cl_ord_id,
                symbol,
                side,
            })
        }
        "G" => {
            let cl_ord_id = get(&fields, 11)?.to_string();
            let orig_cl_ord_id = get(&fields, 41)?.to_string();
            let symbol = get(&fields, 55)?.to_string();
            let side = parse_side(get(&fields, 54)?)?;
            let ord_type = parse_ord_type(get(&fields, 40)?)?;
            let qty = parse_qty(&fields)?;
            let price = match ord_type {
                FixOrdType::Limit => Some(parse_price(get(&fields, 44)?)?),
                FixOrdType::Market => None,
            };
            Ok(ParsedFixMessage::CancelReplaceRequest {
                cl_ord_id,
                orig_cl_ord_id,
                symbol,
                side,
                ord_type,
                price,
                qty,
            })
        }
        other => Err(FixParseError::UnknownMsgType(other.to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_new_order_single_limit_buy() {
        let raw = "8=FIX.4.4|35=D|11=ORD1|55=AAPL|54=1|40=2|44=585.06|38=100|";
        let parsed = parse(raw).unwrap();
        assert_eq!(
            parsed,
            ParsedFixMessage::NewOrderSingle {
                cl_ord_id: "ORD1".into(),
                symbol: "AAPL".into(),
                side: FixSide::Buy,
                ord_type: FixOrdType::Limit,
                price: Some(5_850_600),
                qty: 100,
            }
        );
    }

    #[test]
    fn parses_market_order_with_no_price() {
        let raw = "8=FIX.4.4|35=D|11=ORD1|55=AAPL|54=2|40=1|38=50|";
        let parsed = parse(raw).unwrap();
        assert_eq!(
            parsed,
            ParsedFixMessage::NewOrderSingle {
                cl_ord_id: "ORD1".into(),
                symbol: "AAPL".into(),
                side: FixSide::Sell,
                ord_type: FixOrdType::Market,
                price: None,
                qty: 50,
            }
        );
    }

    #[test]
    fn parses_cancel_request() {
        let raw = "8=FIX.4.4|35=F|11=ORD2|41=ORD1|55=AAPL|54=1|";
        let parsed = parse(raw).unwrap();
        assert_eq!(
            parsed,
            ParsedFixMessage::CancelRequest {
                cl_ord_id: "ORD2".into(),
                orig_cl_ord_id: "ORD1".into(),
                symbol: "AAPL".into(),
                side: FixSide::Buy,
            }
        );
    }

    #[test]
    fn parses_cancel_replace_request() {
        let raw = "8=FIX.4.4|35=G|11=ORD3|41=ORD1|55=AAPL|54=1|40=2|44=586.00|38=75|";
        let parsed = parse(raw).unwrap();
        assert_eq!(
            parsed,
            ParsedFixMessage::CancelReplaceRequest {
                cl_ord_id: "ORD3".into(),
                orig_cl_ord_id: "ORD1".into(),
                symbol: "AAPL".into(),
                side: FixSide::Buy,
                ord_type: FixOrdType::Limit,
                price: Some(5_860_000),
                qty: 75,
            }
        );
    }

    #[test]
    fn rejects_missing_required_tag() {
        let raw = "8=FIX.4.4|35=D|11=ORD1|55=AAPL|54=1|40=2|38=100|"; // no 44 (Price)
        assert_eq!(parse(raw), Err(FixParseError::MissingTag(44)));
    }

    #[test]
    fn rejects_unknown_msg_type() {
        let raw = "8=FIX.4.4|35=Z|";
        assert_eq!(parse(raw), Err(FixParseError::UnknownMsgType("Z".into())));
    }

    #[test]
    fn rejects_empty_message() {
        assert_eq!(parse(""), Err(FixParseError::Empty));
        assert_eq!(parse("   "), Err(FixParseError::Empty));
    }

    #[test]
    fn accepts_real_soh_delimiter() {
        let raw = "8=FIX.4.4\u{1}35=D\u{1}11=ORD1\u{1}55=AAPL\u{1}54=1\u{1}40=2\u{1}44=585.06\u{1}38=100\u{1}";
        assert!(parse(raw).is_ok());
    }
}
