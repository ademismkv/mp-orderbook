#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

// MoldUDP64 Protocol Specification V1.00 (nasdaqtrader.com/content/
// technicalsupport/specifications/dataproducts/moldudp64.pdf) — the real
// session/sequencing layer that Nasdaq's live ITCH multicast feed is
// wrapped in. This is the actual mechanism behind ROADMAP.md's "receive
// UDP multicast" / "handle packet gaps" / "recovery logic" items, not an
// invented scheme: sequence numbers detect loss, heartbeats carry the
// next-expected sequence number even during idle periods so loss can be
// detected without waiting for the next real message, and a separate
// Request/Downstream unicast exchange with a Re-request Server handles
// retransmission.
//
// Distinct from the ITCH 5.0 message layer (itch_messages.h) — MoldUDP64
// treats each message as an opaque byte blob; what's inside is the
// higher-level application's business (here, ITCH 5.0). Also distinct
// from the historical-file "BinaryFile" framing (itch_binaryfile_reader.h)
// used by Nasdaq's downloadable replay files — that's a simpler,
// session-less length-prefixed stream with no sequence numbers, gaps, or
// retransmission, because a file can't drop packets.
//
// All multi-byte fields are big-endian, per the spec's own "Assumptions"
// section — same convention ITCH itself uses.

namespace moldudp64 {

inline void put_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
inline void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v);
        v >>= 8;
    }
}
inline uint16_t get_be16(const uint8_t* p) { return static_cast<uint16_t>((uint32_t(p[0]) << 8) | p[1]); }
inline uint64_t get_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

constexpr size_t kSessionLen = 10;
using SessionId = std::array<uint8_t, kSessionLen>;

// Message Count field's two reserved values (spec "Header" section).
constexpr uint16_t kHeartbeat = 0;
constexpr uint16_t kEndOfSession = 0xFFFF;

constexpr size_t kDownstreamHeaderLen = kSessionLen + 8 + 2;   // 20 bytes
constexpr size_t kRequestLen = kSessionLen + 8 + 2;            // 20 bytes

struct DownstreamHeader {
    SessionId session{};
    uint64_t sequence_number = 0;   // sequence of the FIRST message in this packet
    uint16_t message_count = 0;     // 0 = heartbeat, 0xFFFF = end of session, else real count
};

// A decoded message block, viewed in place (pointer + length into the
// original packet buffer) — a receiver on the hot path shouldn't have to
// allocate per message block just to hand bytes to the ITCH parser.
struct MessageBlockView {
    const uint8_t* data;
    uint16_t length;
};

// Decodes a Downstream Packet's header and message blocks in place.
// Returns false if `len` is shorter than the header, or a message block's
// declared length runs past the end of the buffer — a corrupt/truncated
// packet, which the caller should drop and let gap recovery handle rather
// than try to salvage a partial parse of.
inline bool decode_downstream(const uint8_t* buf, size_t len, DownstreamHeader& out_header,
                               std::vector<MessageBlockView>& out_blocks) {
    out_blocks.clear();
    if (len < kDownstreamHeaderLen) return false;

    std::memcpy(out_header.session.data(), buf, kSessionLen);
    out_header.sequence_number = get_be64(buf + kSessionLen);
    out_header.message_count = get_be16(buf + kSessionLen + 8);

    if (out_header.message_count == kHeartbeat || out_header.message_count == kEndOfSession) {
        return true;   // no message blocks to parse
    }

    size_t offset = kDownstreamHeaderLen;
    for (uint16_t i = 0; i < out_header.message_count; ++i) {
        if (offset + 2 > len) return false;
        const uint16_t mlen = get_be16(buf + offset);
        offset += 2;
        if (offset + mlen > len) return false;
        out_blocks.push_back(MessageBlockView{buf + offset, mlen});
        offset += mlen;
    }
    return true;
}

// Encodes a Downstream Packet carrying real message payloads — used by
// the simulated feed sender (see udp_multicast.h) and by tests that wrap
// real ITCH message bytes in synthetic MoldUDP64 framing (we have no live
// multicast capture to draw packets from directly).
inline std::vector<uint8_t> encode_downstream(const SessionId& session, uint64_t sequence_number,
                                               const std::vector<std::vector<uint8_t>>& messages) {
    std::vector<uint8_t> out(kDownstreamHeaderLen);
    std::memcpy(out.data(), session.data(), kSessionLen);
    put_be64(out.data() + kSessionLen, sequence_number);
    put_be16(out.data() + kSessionLen + 8, static_cast<uint16_t>(messages.size()));

    for (const auto& m : messages) {
        const size_t base = out.size();
        out.resize(base + 2 + m.size());
        put_be16(out.data() + base, static_cast<uint16_t>(m.size()));
        std::memcpy(out.data() + base + 2, m.data(), m.size());
    }
    return out;
}

inline std::vector<uint8_t> encode_control(const SessionId& session, uint64_t next_expected_seq,
                                            uint16_t message_count_value) {
    std::vector<uint8_t> out(kDownstreamHeaderLen);
    std::memcpy(out.data(), session.data(), kSessionLen);
    put_be64(out.data() + kSessionLen, next_expected_seq);
    put_be16(out.data() + kSessionLen + 8, message_count_value);
    return out;
}
inline std::vector<uint8_t> encode_heartbeat(const SessionId& session, uint64_t next_expected_seq) {
    return encode_control(session, next_expected_seq, kHeartbeat);
}
inline std::vector<uint8_t> encode_end_of_session(const SessionId& session, uint64_t next_expected_seq) {
    return encode_control(session, next_expected_seq, kEndOfSession);
}

struct RequestPacket {
    SessionId session{};
    uint64_t sequence_number = 0;          // first requested sequence number
    uint16_t requested_message_count = 0;
};

inline std::vector<uint8_t> encode_request(const RequestPacket& r) {
    std::vector<uint8_t> out(kRequestLen);
    std::memcpy(out.data(), r.session.data(), kSessionLen);
    put_be64(out.data() + kSessionLen, r.sequence_number);
    put_be16(out.data() + kSessionLen + 8, r.requested_message_count);
    return out;
}
inline bool decode_request(const uint8_t* buf, size_t len, RequestPacket& out) {
    if (len < kRequestLen) return false;
    std::memcpy(out.session.data(), buf, kSessionLen);
    out.sequence_number = get_be64(buf + kSessionLen);
    out.requested_message_count = get_be16(buf + kSessionLen + 8);
    return true;
}

} // namespace moldudp64
