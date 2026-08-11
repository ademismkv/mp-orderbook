#pragma once
#include "order_book_v2.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

// Market Data Feed: the last stage in ROADMAP.md's pipeline (Feed ->
// Sequencer -> Order Books -> Risk -> Execution Reports -> Market Data
// Feed). Execution Reports (execution_report.h) are PRIVATE — sent back to
// the one participant who submitted an order, Ack/Reject/Fill. Market data
// is PUBLIC — a normalized, symbol-scoped view of every book-affecting
// event, published for anyone subscribed to see, same distinction a real
// exchange draws between drop-copy/FIX execution reports and its
// proprietary depth-of-book feed (which, for Nasdaq specifically, IS ITCH
// itself — the same wire protocol this repo's Feed Handler parses on the
// way in is also what an exchange publishes on the way out).
//
// Deliberately NOT literal ITCH bytes on the way out: real ITCH is 23
// message types accreted over two decades of the live spec; reusing it here
// would just be re-deriving itch_messages.h, not adding anything new. What
// IS reused, and matters more for the honesty of this repo, is the real
// transport already built and verified against real sockets: this feed
// travels over the exact same moldudp64.h framing (sequence numbers, gap
// detection, request/retransmission) and udp_multicast.h real UDP
// multicast sender/receiver from day 14/15 — a genuine publish/subscribe
// loop over real sockets, not a simulated callback pretending to be a feed.
namespace mdfeed {

enum class EventType : uint8_t { Add = 'A', Cancel = 'X', Delete = 'D', Trade = 'T', Replace = 'R' };

// One normalized, symbol-scoped market data event. Fixed-size (50 bytes)
// wire record — simple enough to encode/decode without a variant dispatch,
// the way itch_messages.h's 23-type discriminated union has to.
//
// Field meaning depends on `type`:
//   Add:     order_id = the new resting order, side/price/qty as submitted.
//   Cancel:  order_id = the affected resting order, qty = shares REMOVED
//            (not remaining) — a subscriber decrements, doesn't overwrite.
//            price/side carried forward from that order's own Add, not
//            repeated here (mirrors real ITCH's Order Cancel, which also
//            doesn't repeat price).
//   Delete:  order_id = the affected resting order, fully removed. price/
//            qty/side unused (0) — a subscriber already knows them from
//            that order's Add and should discard the entry entirely.
//   Trade:   order_id = the resting (maker) order that was filled,
//            new_order_id = the incoming (taker) order if known (0 for
//            Order Executed / Order Executed With Price, which — see
//            sequencer.cpp — only ever reduce an existing resting order
//            and never carry a taker's id). price = execution price (0 if
//            genuinely unknown — see plain Order Executed's honest gap,
//            documented in sequencer.cpp). qty = shares executed.
//   Replace: order_id = the OLD resting order (now gone), new_order_id =
//            the new one that replaced it, price/qty/side = the NEW
//            order's terms — mirrors real ITCH Order Replace exactly.
struct MDEvent {
    EventType type = EventType::Add;
    Side side = Side::Buy;
    uint64_t timestamp_ns = 0;
    OrderId order_id = 0;
    OrderId new_order_id = 0;   // Replace only; else 0
    std::array<char, 8> symbol{};   // space-padded, same convention as itch::Alpha<8>
    Price price = 0;
    Quantity qty = 0;
};

constexpr size_t kRecordLen = 50;

inline void put_be64_(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}
inline uint64_t get_be64_(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

inline std::vector<uint8_t> encode(const MDEvent& e) {
    std::vector<uint8_t> out(kRecordLen);
    out[0] = static_cast<uint8_t>(e.type);
    out[1] = (e.side == Side::Buy) ? 'B' : 'S';
    put_be64_(out.data() + 2, e.timestamp_ns);
    put_be64_(out.data() + 10, static_cast<uint64_t>(e.order_id));
    put_be64_(out.data() + 18, static_cast<uint64_t>(e.new_order_id));
    std::memcpy(out.data() + 26, e.symbol.data(), 8);
    put_be64_(out.data() + 34, static_cast<uint64_t>(e.price));   // bit pattern; sign doesn't matter, prices are >= 0
    put_be64_(out.data() + 42, static_cast<uint64_t>(e.qty));
    return out;
}

inline std::optional<MDEvent> decode(const uint8_t* data, size_t len) {
    if (len != kRecordLen) return std::nullopt;
    MDEvent e;
    e.type = static_cast<EventType>(data[0]);
    e.side = (data[1] == 'B') ? Side::Buy : Side::Sell;
    e.timestamp_ns = get_be64_(data + 2);
    e.order_id = static_cast<OrderId>(get_be64_(data + 10));
    e.new_order_id = static_cast<OrderId>(get_be64_(data + 18));
    std::memcpy(e.symbol.data(), data + 26, 8);
    e.price = static_cast<Price>(get_be64_(data + 34));
    e.qty = static_cast<Quantity>(get_be64_(data + 42));
    return e;
}

inline std::string symbol_str(const std::array<char, 8>& raw) {
    size_t end = 8;
    while (end > 0 && raw[end - 1] == ' ') --end;
    return std::string(raw.data(), end);
}
inline std::array<char, 8> symbol_pad(const std::string& s) {
    std::array<char, 8> a{};
    a.fill(' ');
    std::memcpy(a.data(), s.data(), std::min<size_t>(8, s.size()));
    return a;
}

} // namespace mdfeed
