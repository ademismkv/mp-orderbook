#include "itch_binaryfile_reader.h"
#include "moldudp64.h"
#include "moldudp64_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Verifies the MoldUDP64 session/gap-recovery state machine using REAL
// ITCH 5.0 message bytes as the payload (pulled from the same real Nasdaq
// sample as test_itch_parser) — the message content is real, wrapped in
// synthetic MoldUDP64 packet framing, since there's no live multicast
// capture available in this environment to draw actual wire packets from.
// That distinction is deliberate and stated here rather than glossed
// over: this test proves the sequencing/gap-detection/recovery logic is
// correct against the real spec, not that a live capture round-trips.
//
// Scenario: 8 real messages split across 4 packets (seq 1-2, 3-4, 5-6,
// 7-8). Packet 2 (seq 3-4) is withheld to simulate a dropped UDP packet,
// forcing packet 3 (seq 5-6) to arrive "early" — this must trigger a gap
// request for exactly [3, count=2), and the seq 5-6 messages must NOT be
// delivered yet. Feeding the withheld packet back in (as if a
// retransmission arrived) must then flush everything in strict order.
// A heartbeat and an end-of-session control packet close out the test.

namespace {
int failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    // Pull 8 real messages to use as payloads.
    itch::BinaryFileReader reader(path);
    std::vector<std::vector<uint8_t>> real_msgs;
    while (real_msgs.size() < 8) {
        auto raw = reader.next_raw();
        if (!raw) {
            std::printf("FATAL: sample file ran out before 8 messages\n");
            return 1;
        }
        real_msgs.push_back(std::move(*raw));
    }

    moldudp64::SessionId session{};
    std::memcpy(session.data(), "TESTSESS01", moldudp64::kSessionLen);

    // Pack into 4 downstream packets of 2 messages each.
    auto pkt = [&](int start_idx) {
        return moldudp64::encode_downstream(
            session, static_cast<uint64_t>(start_idx + 1),
            {real_msgs[start_idx], real_msgs[start_idx + 1]});
    };
    std::vector<uint8_t> packet1 = pkt(0);   // seq 1-2
    std::vector<uint8_t> packet2 = pkt(2);   // seq 3-4 — withheld first
    std::vector<uint8_t> packet3 = pkt(4);   // seq 5-6
    std::vector<uint8_t> packet4 = pkt(6);   // seq 7-8

    std::vector<uint64_t> delivered_seqs;
    std::vector<std::vector<uint8_t>> delivered_bytes;
    std::vector<moldudp64::RequestPacket> requests;

    moldudp64::Session sess(
        session,
        [&](uint64_t seq, const uint8_t* data, uint16_t len) {
            delivered_seqs.push_back(seq);
            delivered_bytes.emplace_back(data, data + len);
        },
        [&](const moldudp64::RequestPacket& r) { requests.push_back(r); });

    auto feed = [&](const std::vector<uint8_t>& raw) {
        moldudp64::DownstreamHeader hdr;
        std::vector<moldudp64::MessageBlockView> blocks;
        if (!moldudp64::decode_downstream(raw.data(), raw.size(), hdr, blocks)) {
            std::printf("FATAL: decode_downstream failed on a locally-encoded packet\n");
            std::exit(1);
        }
        sess.on_packet(hdr, blocks);
    };

    // Packet 1 (seq 1-2): in order, delivers immediately.
    feed(packet1);
    expect(delivered_seqs.size() == 2 && delivered_seqs[0] == 1 && delivered_seqs[1] == 2,
           "packet1 (seq 1-2) delivers immediately, in order");
    expect(requests.empty(), "no gap request after in-order packet1");

    // Packet 3 (seq 5-6) arrives before packet 2 (seq 3-4) — simulates
    // packet 2 being dropped. Must NOT deliver seq 5-6 yet, and must
    // request exactly [3, count=2).
    feed(packet3);
    expect(delivered_seqs.size() == 2, "seq 5-6 held back, not delivered ahead of the gap");
    expect(requests.size() == 1, "exactly one gap request emitted");
    if (!requests.empty()) {
        expect(requests[0].sequence_number == 3, "gap request starts at seq 3");
        expect(requests[0].requested_message_count == 2, "gap request asks for 2 messages");
    }
    expect(sess.gaps_detected() == 1, "gaps_detected() counter is 1");

    // Retransmission arrives (packet 2, seq 3-4) — must flush 3,4,5,6 in order.
    feed(packet2);
    expect(delivered_seqs.size() == 6, "retransmission flushes the held-back messages too");
    if (delivered_seqs.size() == 6) {
        bool in_order = true;
        for (size_t i = 0; i < 6; ++i) in_order &= (delivered_seqs[i] == i + 1);
        expect(in_order, "seq 1-6 delivered in strict order after gap fill");
        // Content check: delivered bytes must exactly match the real
        // message bytes pulled from the sample file, not just the count.
        bool bytes_match = true;
        for (size_t i = 0; i < 6; ++i) bytes_match &= (delivered_bytes[i] == real_msgs[i]);
        expect(bytes_match, "delivered message bytes match the real source bytes exactly");
    }

    // Heartbeat matching next_expected (7) — no gap, no new request.
    auto hb = moldudp64::encode_heartbeat(session, 7);
    feed(hb);
    expect(requests.size() == 1, "on-time heartbeat triggers no new gap request");

    // Packet 4 (seq 7-8): in order, delivers.
    feed(packet4);
    expect(delivered_seqs.size() == 8, "packet4 (seq 7-8) delivers after heartbeat");
    expect(sess.next_expected_sequence() == 9, "next_expected_sequence() advances to 9");

    // End of session, next expected seq 9 (matches — no gap).
    auto eos = moldudp64::encode_end_of_session(session, 9);
    feed(eos);
    expect(sess.ended(), "end-of-session packet sets ended()");
    expect(requests.size() == 1, "no spurious gap request on matching end-of-session");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
