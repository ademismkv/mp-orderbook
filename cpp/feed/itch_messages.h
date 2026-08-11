#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// Nasdaq TotalView-ITCH 5.0 message layouts, transcribed field-for-field
// from the official spec (nasdaqtrader.com/.../NQTVITCHSpecification.pdf,
// revision log's most recent entry: April 28, 2023 — the doc is still
// titled "5.0" but includes the LULD Auction Collar (Sept 2017),
// Operational Halt (March 2018), and Direct Listing with Capital Raise
// (April 2023) additions; Nasdaq deliberately did not bump the version
// string when those landed, see the doc's own May 3, 2018 revision note).
//
// Field-level source notes worth keeping close to the code, not just in a
// devlog entry, because a parser that silently ignores them is a parser
// that's silently wrong:
//   - Operational Halt's message type code is lowercase 'h' — every other
//     type code in the spec is uppercase. Confirmed against two indepedent
//     fetches of the spec text, not a transcription slip.
//   - Trade (Non-Cross) 'P': Order Reference Number has been zero-filled
//     since 2010-12-06, and Buy/Sell Indicator has always been 'B'
//     regardless of the resting side since 2014-07-14. Do not treat either
//     field as meaningful on modern data.
//   - Order Replace 'U' does not carry side, stock symbol, or attribution
//     — the spec says explicitly: "these fields are not included in the
//     message... firms should retain the side, stock symbol and MPID from
//     the original Add Order message." A parser-level struct can't enforce
//     that; whatever consumes OrderReplace must join it against prior
//     state, not assume the struct is self-contained.
//   - Cross Trade 'Q' Shares is 8 bytes; every other message's Shares
//     field is 4 bytes. Easy offset bug if copy-pasted from Add Order.
//
// Prices are carried as the wire's raw Price(4)/Price(8) integer ticks
// (4/8 implied decimal places respectively) — not converted to double,
// matching this repo's existing OrderBookV2::Price convention (see
// order_book_v2.h) of representing price as an integer tick count rather
// than a float, for exactly the reasons ADR-2 gives there.

namespace itch {

// ---- wire-format primitives: everything is big-endian per spec Section 3 ----

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((uint32_t(p[0]) << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
// Timestamp fields are 48-bit (6 bytes) — nanoseconds since midnight.
inline uint64_t be48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}
inline uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Alpha field: ASCII, right-padded with spaces (spec Section 3). Fixed-size
// storage so message structs stay POD-ish and cheap to copy; str() trims
// the padding on demand rather than paying for it on every parse.
template <size_t N>
struct Alpha {
    std::array<char, N> raw{};
    std::string str() const {
        size_t end = N;
        while (end > 0 && raw[end - 1] == ' ') --end;
        return std::string(raw.data(), end);
    }
};

template <size_t N>
inline Alpha<N> read_alpha(const uint8_t* p) {
    Alpha<N> a;
    for (size_t i = 0; i < N; ++i) a.raw[i] = static_cast<char>(p[i]);
    return a;
}

// Every message type shares these three fields at the same fixed offsets
// (1, 3, 5) — confirmed identical across all 23 message tables in the spec.
struct CommonHeader {
    uint16_t stock_locate;      // 0 for non-stock-specific messages
    uint16_t tracking_number;   // Nasdaq-internal
    uint64_t timestamp_ns;      // nanoseconds since midnight, 48-bit
};

inline CommonHeader read_common_header(const uint8_t* p) {
    return CommonHeader{be16(p + 1), be16(p + 3), be48(p + 5)};
}

// ---- message structs, one per type ----
// Comment on each gives: wire type code, total message length in bytes.

struct SystemEvent {                     // 'S', 12 bytes
    CommonHeader h;
    char event_code;   // O/S/Q/M/E/C — see spec 4.1
};
struct StockDirectory {                  // 'R', 39 bytes
    CommonHeader h;
    Alpha<8> stock;
    char market_category;
    char financial_status_indicator;
    uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    Alpha<2> issue_subtype;
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    uint32_t etp_leverage_factor;
    char inverse_indicator;
};
struct StockTradingAction {              // 'H', 25 bytes
    CommonHeader h;
    Alpha<8> stock;
    char trading_state;   // H/P/Q/T
    char reserved;
    Alpha<4> reason;
};
struct RegSHORestriction {               // 'Y', 20 bytes
    CommonHeader h;
    Alpha<8> stock;
    char reg_sho_action;   // '0'/'1'/'2'
};
struct MarketParticipantPosition {       // 'L', 26 bytes
    CommonHeader h;
    Alpha<4> mpid;
    Alpha<8> stock;
    char primary_market_maker;
    char market_maker_mode;
    char market_participant_state;
};
struct MWCBDeclineLevel {                // 'V', 35 bytes
    CommonHeader h;
    uint64_t level1;   // Price(8)
    uint64_t level2;
    uint64_t level3;
};
struct MWCBStatus {                      // 'W', 12 bytes
    CommonHeader h;
    char breached_level;   // '1'/'2'/'3'
};
struct IPOQuotingPeriodUpdate {          // 'K', 28 bytes
    CommonHeader h;
    Alpha<8> stock;
    uint32_t ipo_quotation_release_time;   // seconds since midnight — NOT ns, unlike the header timestamp
    char ipo_quotation_release_qualifier;  // 'A'/'C'
    uint32_t ipo_price;                    // Price(4); 0 if qualifier is 'C' (canceled/postponed)
};
struct LULDAuctionCollar {               // 'J', 35 bytes — added 2017-09-06
    CommonHeader h;
    Alpha<8> stock;
    uint32_t auction_collar_reference_price;
    uint32_t upper_auction_collar_price;
    uint32_t lower_auction_collar_price;
    uint32_t auction_collar_extension;
};
struct OperationalHalt {                 // 'h' (lowercase), 21 bytes — added 2018-03-03
    CommonHeader h;
    Alpha<8> stock;
    char market_code;               // Q/B/X
    char operational_halt_action;   // 'H'/'T'
};
struct AddOrderNoMPID {                  // 'A', 36 bytes
    CommonHeader h;
    uint64_t order_ref_number;
    char buy_sell_indicator;   // 'B'/'S'
    uint32_t shares;
    Alpha<8> stock;
    uint32_t price;   // Price(4)
};
struct AddOrderMPID {                    // 'F', 40 bytes
    CommonHeader h;
    uint64_t order_ref_number;
    char buy_sell_indicator;
    uint32_t shares;
    Alpha<8> stock;
    uint32_t price;
    Alpha<4> attribution;
};
struct OrderExecuted {                   // 'E', 31 bytes
    CommonHeader h;
    uint64_t order_ref_number;
    uint32_t executed_shares;
    uint64_t match_number;
};
struct OrderExecutedWithPrice {          // 'C', 36 bytes
    CommonHeader h;
    uint64_t order_ref_number;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable;   // 'Y'/'N' — ignore 'N' to avoid double-counting (spec's own recommendation)
    uint32_t execution_price;
};
struct OrderCancel {                     // 'X', 23 bytes — partial cancel
    CommonHeader h;
    uint64_t order_ref_number;
    uint32_t canceled_shares;
};
struct OrderDelete {                     // 'D', 19 bytes — full cancel
    CommonHeader h;
    uint64_t order_ref_number;
};
struct OrderReplace {                    // 'U', 35 bytes
    CommonHeader h;
    uint64_t original_order_ref_number;
    uint64_t new_order_ref_number;
    uint32_t shares;
    uint32_t price;
    // No side/stock/attribution — see file header comment. Caller must
    // carry those forward from the original Add Order.
};
struct TradeNonCross {                   // 'P', 44 bytes
    CommonHeader h;
    uint64_t order_ref_number;   // always 0 since 2010-12-06
    char buy_sell_indicator;     // always 'B' since 2014-07-14 — not meaningful
    uint32_t shares;
    Alpha<8> stock;
    uint32_t price;
    uint64_t match_number;
};
struct CrossTrade {                      // 'Q', 40 bytes
    CommonHeader h;
    uint64_t shares;   // 8 bytes here — unlike every other Shares field, which is 4
    Alpha<8> stock;
    uint32_t cross_price;
    uint64_t match_number;
    char cross_type;   // O/C/H/I
};
struct BrokenTrade {                     // 'B', 19 bytes
    CommonHeader h;
    uint64_t match_number;   // refers back to a prior Order Executed / Trade match number
};
struct NOII {                            // 'I', 50 bytes
    CommonHeader h;
    uint64_t paired_shares;
    uint64_t imbalance_shares;
    char imbalance_direction;   // B/S/N/O
    Alpha<8> stock;
    uint32_t far_price;
    uint32_t near_price;
    uint32_t current_reference_price;
    char cross_type;                    // O/C/H — no 'I' here, unlike Cross Trade's cross_type
    char price_variation_indicator;
};
struct RPII {                            // 'N', 20 bytes — legacy, program ceased 2014-12-31 but still specified
    CommonHeader h;
    Alpha<8> stock;
    char interest_flag;   // B/S/A/N
};
struct DirectListingCapitalRaise {       // 'O', 48 bytes — added 2023-04-28
    CommonHeader h;
    Alpha<8> stock;
    char open_eligibility_status;   // 'Y'/'N'
    uint32_t minimum_allowable_price;
    uint32_t maximum_allowable_price;
    uint32_t near_execution_price;
    uint64_t near_execution_time;
    uint32_t lower_price_range_collar;
    uint32_t upper_price_range_collar;
};

using Message = std::variant<
    SystemEvent, StockDirectory, StockTradingAction, RegSHORestriction,
    MarketParticipantPosition, MWCBDeclineLevel, MWCBStatus,
    IPOQuotingPeriodUpdate, LULDAuctionCollar, OperationalHalt,
    AddOrderNoMPID, AddOrderMPID, OrderExecuted, OrderExecutedWithPrice,
    OrderCancel, OrderDelete, OrderReplace, TradeNonCross, CrossTrade,
    BrokenTrade, NOII, RPII, DirectListingCapitalRaise>;

// Parses one message from `data[0..len)`. `data[0]` is the message type
// byte (this function does NOT expect a length prefix — that's the
// BinaryFile/MoldUDP64 framing layer's job, see itch_binaryfile_reader.h).
// Returns nullopt if the type byte isn't one of the 23 known types (a
// live feed can carry types added after this build's spec version — the
// correct behavior is to skip, not crash) or if `len` is shorter than the
// type's declared wire length (truncated/corrupt record).
std::optional<Message> parse_message(const uint8_t* data, size_t len);

// Direct scalar decoder for Add Order — No MPID ('A'), bypassing
// std::variant/std::optional construction. Exists so SIMD-vs-scalar
// benchmarks can isolate actual field-decode cost instead of being
// swamped by Message's variant-construction overhead (sizeof(Message) is
// 72 bytes vs. sizeof(AddOrderNoMPID) alone at 48 — a real, measured
// confound found while benchmarking itch_simd_neon.cpp, not a
// hypothetical one). parse_message()'s 'A' case calls this internally,
// so there's exactly one implementation to keep correct, not two that
// can drift apart.
AddOrderNoMPID decode_add_order_scalar(const uint8_t* data);

// The exact wire length (in bytes, including the 1-byte type code) that
// this message type occupies, or 0 if `type` isn't a recognized code.
// Used to cross-check against the BinaryFile/MoldUDP64 length field as a
// corruption sanity check independent of parse_message() itself.
size_t expected_length(char type);

} // namespace itch
