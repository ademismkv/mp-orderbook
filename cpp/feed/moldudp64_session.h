#pragma once
#include "moldudp64.h"

#include <algorithm>
#include <functional>
#include <map>
#include <vector>

namespace moldudp64 {

// Tracks one MoldUDP64 downstream session's sequencing state, independent
// of any actual socket (see udp_multicast.h for the real receive path).
// Feed it packets via on_packet() in whatever order they arrive — real UDP
// gives no ordering guarantee, and retransmissions from a re-request
// server arrive out of band — and it delivers messages in strict sequence
// order via the message callback, holding out-of-order arrivals until the
// gap before them fills, and invoking the request callback exactly when
// the spec's own receiver flowchart says to (step 5: "If the sequence
// number does not match the next expected sequence number, send a
// Request Packet").
//
// This is the real mechanism behind ROADMAP.md's "handle packet gaps" /
// "recovery logic" items.
class Session {
public:
    // seq: absolute sequence number of this message. data/len: the raw
    // message bytes (view only — valid for the duration of the callback).
    using MessageCallback = std::function<void(uint64_t seq, const uint8_t* data, uint16_t len)>;
    // Invoked when a gap is detected and a retransmission should be
    // requested. Real socket code supplies this in production; tests can
    // just record calls and hand back synthetic retransmissions.
    using RequestCallback = std::function<void(const RequestPacket&)>;

    // start_seq: the sequence number this session should next expect.
    // Defaults to 1 (a subscriber joining at the very start of the
    // session, per the spec's own examples). A subscriber recovering from
    // a snapshot instead starts at snapshot_as_of_seq + 1 — see
    // cpp/feed/snapshot.h — so it correctly treats every live packet at or
    // below the snapshot's sequence as already-known (not a gap to fill),
    // while still detecting and recovering any real gap between the
    // snapshot and the first live packet it actually receives.
    Session(SessionId session, MessageCallback on_message, RequestCallback on_request_needed,
            uint64_t start_seq = 1)
        : session_(session), on_message_(std::move(on_message)), on_request_needed_(std::move(on_request_needed)),
          next_expected_(start_seq) {}

    // Feed one received Downstream Packet's already-decoded header and
    // message blocks (the session-id-matches-this-session check is the
    // caller's job — a mismatch is a protocol error the spec says to
    // "abort and report", not something this class should silently eat).
    void on_packet(const DownstreamHeader& hdr, const std::vector<MessageBlockView>& blocks) {
        if (hdr.message_count == kHeartbeat || hdr.message_count == kEndOfSession) {
            // Heartbeats/end-of-session carry the next expected sequence
            // number even when no messages are in flight — this is how a
            // gap during an idle period gets detected without waiting
            // indefinitely for the next real message.
            maybe_request_gap_(hdr.sequence_number);
            if (hdr.message_count == kEndOfSession) ended_ = true;
            return;
        }

        for (size_t i = 0; i < blocks.size(); ++i) {
            const uint64_t seq = hdr.sequence_number + i;
            if (seq < next_expected_) continue;   // already delivered — duplicate/overlapping retransmission
            pending_.emplace(seq, std::vector<uint8_t>(blocks[i].data, blocks[i].data + blocks[i].length));
        }

        // Only request a gap if this packet's start is actually ahead of
        // what we expect — an on-time or overlapping-from-behind packet
        // needs no request.
        if (!blocks.empty()) maybe_request_gap_(hdr.sequence_number);

        deliver_ready_();
    }

    uint64_t next_expected_sequence() const { return next_expected_; }
    bool ended() const { return ended_; }

    // How many times a gap was detected and a request emitted — a real,
    // measurable stat, same spirit as OrderBookV2::level_array_growths().
    uint64_t gaps_detected() const { return gaps_detected_; }

private:
    void maybe_request_gap_(uint64_t observed_seq) {
        if (observed_seq > next_expected_) {
            ++gaps_detected_;
            RequestPacket req;
            req.session = session_;
            req.sequence_number = next_expected_;
            req.requested_message_count = static_cast<uint16_t>(
                std::min<uint64_t>(observed_seq - next_expected_, 0xFFFFu));
            on_request_needed_(req);
        }
    }

    void deliver_ready_() {
        auto it = pending_.find(next_expected_);
        while (it != pending_.end()) {
            on_message_(it->first, it->second.data(), static_cast<uint16_t>(it->second.size()));
            pending_.erase(it);
            ++next_expected_;
            it = pending_.find(next_expected_);
        }
    }

    SessionId session_;
    MessageCallback on_message_;
    RequestCallback on_request_needed_;

    uint64_t next_expected_ = 1;   // MoldUDP64 sequence numbers start at 1, per spec examples
    bool ended_ = false;
    uint64_t gaps_detected_ = 0;

    // Messages received ahead of the current gap, held until it fills.
    std::map<uint64_t, std::vector<uint8_t>> pending_;
};

} // namespace moldudp64
