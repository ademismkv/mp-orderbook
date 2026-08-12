#pragma once
#include "market_data.h"
#include "moldudp64.h"   // reuse put_be64/get_be64 — same big-endian convention

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Real snapshot recovery for a subscriber that joins the Market Data Feed
// mid-stream, or falls behind by more than a point-to-point Request Packet
// can economically cover. This is a real gap this repo had — unlike
// MoldUDP64 "session establishment," which isn't a gap at all (see ADR-5).
// A subscriber that only ever consumes the live multicast feed has no way
// to learn about resting orders that were Added before it joined; forcing
// it to replay every historical event from sequence 1 doesn't scale — the
// exact problem real GLIMPSE-style snapshot services solve. See
// cpp/feed/tcp_socket.h for why this uses a real TCP connection instead of
// MoldUDP64/UDP for the exchange itself.
namespace snapshot {

struct RestingOrder {
    Side side;
    Price price;
    Quantity qty;
};

// One row of a full-book snapshot: which symbol, which resting order, its
// current side/price/qty — everything a subscriber needs to seed its own
// per-order shadow map without having seen that order's original Add
// event live.
struct SnapshotRow {
    std::array<char, 8> symbol{};
    OrderId order_id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity qty = 0;
};

constexpr size_t kRowLen = 8 + 8 + 1 + 8 + 8;   // 33 bytes: symbol, order_id, side, price, qty

inline std::vector<uint8_t> encode_row(const SnapshotRow& r) {
    std::vector<uint8_t> out(kRowLen);
    std::memcpy(out.data(), r.symbol.data(), 8);
    moldudp64::put_be64(out.data() + 8, static_cast<uint64_t>(r.order_id));
    out[16] = (r.side == Side::Buy) ? 'B' : 'S';
    moldudp64::put_be64(out.data() + 17, static_cast<uint64_t>(r.price));
    moldudp64::put_be64(out.data() + 25, static_cast<uint64_t>(r.qty));
    return out;
}
inline SnapshotRow decode_row(const uint8_t* data) {
    SnapshotRow r;
    std::memcpy(r.symbol.data(), data, 8);
    r.order_id = static_cast<OrderId>(moldudp64::get_be64(data + 8));
    r.side = (data[16] == 'B') ? Side::Buy : Side::Sell;
    r.price = static_cast<Price>(moldudp64::get_be64(data + 17));
    r.qty = static_cast<Quantity>(moldudp64::get_be64(data + 25));
    return r;
}

// Full snapshot wire format: 8-byte "as of sequence number" + 4-byte row
// count (big-endian) + that many kRowLen rows, back to back. A subscriber
// that loads this and then starts its moldudp64::Session with
// start_seq = as_of_seq + 1 (see moldudp64_session.h) is caught up exactly
// as if it had replayed every event from sequence 1 itself, without
// actually needing to.
inline std::vector<uint8_t> encode_snapshot(uint64_t as_of_seq, const std::vector<SnapshotRow>& rows) {
    std::vector<uint8_t> out(12);
    moldudp64::put_be64(out.data(), as_of_seq);
    const uint32_t count = static_cast<uint32_t>(rows.size());
    out[8] = static_cast<uint8_t>(count >> 24);
    out[9] = static_cast<uint8_t>(count >> 16);
    out[10] = static_cast<uint8_t>(count >> 8);
    out[11] = static_cast<uint8_t>(count);
    out.reserve(out.size() + rows.size() * kRowLen);
    for (const auto& r : rows) {
        auto enc = encode_row(r);
        out.insert(out.end(), enc.begin(), enc.end());
    }
    return out;
}
inline bool decode_snapshot(const std::vector<uint8_t>& buf, uint64_t& out_as_of_seq,
                             std::vector<SnapshotRow>& out_rows) {
    if (buf.size() < 12) return false;
    out_as_of_seq = moldudp64::get_be64(buf.data());
    const uint32_t count = (static_cast<uint32_t>(buf[8]) << 24) | (static_cast<uint32_t>(buf[9]) << 16) |
                            (static_cast<uint32_t>(buf[10]) << 8) | static_cast<uint32_t>(buf[11]);
    if (buf.size() != 12 + static_cast<size_t>(count) * kRowLen) return false;
    out_rows.clear();
    out_rows.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        out_rows.push_back(decode_row(buf.data() + 12 + static_cast<size_t>(i) * kRowLen));
    }
    return true;
}

// Maintains full per-order resting-book state by applying the SAME
// mdfeed::MDEvent stream the live feed publishes, tagged with each
// event's MoldUDP64 sequence number. In a real deployment this runs as its
// own subscriber to the live feed (exactly like a shadow-book subscriber —
// see test_market_data_feed.cpp) — here it's driven directly off the same
// captured event stream a publisher already has, which is equivalent (same
// events, same order) and avoids a redundant real second live-feed
// subscription just to answer snapshot requests.
class Builder {
public:
    void apply(uint64_t seq, const mdfeed::MDEvent& e) {
        const std::string symbol = mdfeed::symbol_str(e.symbol);
        auto& book = books_[symbol];
        switch (e.type) {
        case mdfeed::EventType::Add:
            book[e.order_id] = RestingOrder{e.side, e.price, e.qty};
            break;
        case mdfeed::EventType::Cancel:
        case mdfeed::EventType::Trade: {
            auto it = book.find(e.order_id);
            if (it == book.end()) break;   // defensive — same scope note as market_data.h
            if (e.qty >= it->second.qty) book.erase(it);
            else it->second.qty -= e.qty;
            break;
        }
        case mdfeed::EventType::Delete:
            book.erase(e.order_id);
            break;
        case mdfeed::EventType::Replace:
            book.erase(e.order_id);
            book[e.new_order_id] = RestingOrder{e.side, e.price, e.qty};
            break;
        }
        last_seq_ = seq;
    }

    uint64_t sequence() const { return last_seq_; }

    // Seeds this builder's state directly from a decoded snapshot — used
    // by a subscriber that received one over a real TCP connection (see
    // tcp_socket.h) rather than derived it locally from a live event
    // stream. After this call, apply()ing live events with seq > as_of_seq
    // continues the same state exactly as if this builder had applied
    // every event from sequence 1 itself.
    void load(uint64_t as_of_seq, const std::vector<SnapshotRow>& rows) {
        books_.clear();
        for (const auto& r : rows) {
            books_[mdfeed::symbol_str(r.symbol)][r.order_id] = RestingOrder{r.side, r.price, r.qty};
        }
        last_seq_ = as_of_seq;
    }

    std::vector<SnapshotRow> rows() const {
        std::vector<SnapshotRow> out;
        for (const auto& [symbol, book] : books_) {
            const auto sym_padded = mdfeed::symbol_pad(symbol);
            for (const auto& [oid, o] : book) {
                if (o.qty == 0) continue;
                out.push_back(SnapshotRow{sym_padded, oid, o.side, o.price, o.qty});
            }
        }
        return out;
    }

private:
    std::unordered_map<std::string, std::unordered_map<OrderId, RestingOrder>> books_;
    uint64_t last_seq_ = 0;
};

} // namespace snapshot
