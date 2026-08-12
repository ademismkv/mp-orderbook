#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "moldudp64.h"
#include "moldudp64_session.h"
#include "udp_multicast.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/select.h>
#include <thread>
#include <vector>

// Real-socket exercise of a scenario test_udp_multicast_e2e.cpp doesn't
// cover: a gap discovered purely from a HEARTBEAT during an idle period,
// not from the arrival of the next real message. This is the actual reason
// MoldUDP64 heartbeats exist (moldudp64.h's own top comment: "heartbeats
// carry the next-expected sequence number even during idle periods so loss
// can be detected without waiting for the next real message") — until this
// test, that mechanism only had in-process coverage (test_moldudp64.cpp's
// "on-time heartbeat triggers no new gap request" case), never exercised
// with a real heartbeat packet sent over a real UDP multicast socket while
// the feed is genuinely idle.
//
// Scenario, over real sockets:
//   1. Sender publishes packet1 (seq 1-2) over real multicast.
//   2. Sender withholds packet2 (seq 3-4) — simulates a dropped datagram —
//      and sends no further real message packets. The feed is now idle.
//   3. Sender publishes several real MoldUDP64 heartbeat packets, each
//      carrying next_expected_seq=5 (the exchange's real position, since it
//      already assigned seq 3-4 even though that packet never reached the
//      wire in this simulation) — exactly what a live idle exchange feed
//      actually transmits, per the spec.
//   4. The FIRST such heartbeat must be what triggers gap detection (there
//      is no other message that could have) and a real Request Packet over
//      a real unicast socket.
//   5. A re-request server thread answers over a real unicast socket.
//   6. Once recovered, a final matching heartbeat (next_expected=5) must
//      NOT trigger a second request.
//   7. A real end-of-session packet (next_expected=5, matching) closes out
//      cleanly.

namespace {
constexpr const char* kGroup = "239.255.13.37";
constexpr uint16_t kMcastPort = 17341;
constexpr uint16_t kRequestServerPort = 17342;
constexpr uint16_t kClientReplyPort = 17343;
constexpr int kNumMessages = 4;   // seq 1-4; only seq 1-2 ever go out on time

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

    itch::BinaryFileReader reader(path);
    std::vector<std::vector<uint8_t>> real_msgs;
    while (real_msgs.size() < static_cast<size_t>(kNumMessages)) {
        auto raw = reader.next_raw();
        if (!raw) {
            std::printf("FATAL: sample ran out early\n");
            return 1;
        }
        real_msgs.push_back(std::move(*raw));
    }

    moldudp64::SessionId session_id{};
    std::memcpy(session_id.data(), "HBTEST01", 8);

    auto packet1 = moldudp64::encode_downstream(session_id, 1, {real_msgs[0], real_msgs[1]});
    auto packet2 = moldudp64::encode_downstream(session_id, 3, {real_msgs[2], real_msgs[3]});   // withheld first

    std::atomic<bool> server_failed{false};
    std::atomic<bool> sender_failed{false};

    // --- re-request server thread: answers the retransmit request for seq 3 ---
    std::thread server_thread([&] {
        try {
            netfeed::UnicastSocket server(kRequestServerPort);
            std::string from_ip;
            uint16_t from_port = 0;
            auto raw = server.receive_from(from_ip, from_port);
            moldudp64::RequestPacket req;
            if (moldudp64::decode_request(raw.data(), raw.size(), req) && req.sequence_number == 3) {
                server.send_to(from_ip, from_port, packet2);
            }
        } catch (const std::exception& e) {
            std::printf("FAIL: server thread threw: %s\n", e.what());
            server_failed = true;
        }
    });

    // --- sender thread: packet1, then idle-with-heartbeats, then EOS ---
    std::thread sender_thread([&] {
        try {
            netfeed::McastSender sender(kGroup, kMcastPort);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));   // let receiver join the group

            sender.send(packet1);   // seq 1-2, real, on time

            // packet2 (seq 3-4) is deliberately never sent — this is the
            // drop. Instead, the feed goes idle and the exchange keeps
            // emitting real heartbeats carrying its true position
            // (next_expected=5, since it already assigned seq 3-4
            // internally even though that packet never made it out).
            for (int i = 0; i < 5; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                sender.send(moldudp64::encode_heartbeat(session_id, 5));
            }

            // Give recovery time to complete, then close the session.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            sender.send(moldudp64::encode_end_of_session(session_id, 5));
        } catch (const std::exception& e) {
            std::printf("FAIL: sender thread threw: %s\n", e.what());
            sender_failed = true;
        }
    });

    // --- main thread: the real Feed Handler receive path ---
    netfeed::McastReceiver mcast_recv(kGroup, kMcastPort);
    netfeed::UnicastSocket client(kClientReplyPort);

    std::vector<uint64_t> delivered_seqs;
    std::vector<std::vector<uint8_t>> delivered_bytes;
    int gap_requests_sent = 0;
    int heartbeats_seen = 0;

    moldudp64::Session session(
        session_id,
        [&](uint64_t seq, const uint8_t* data, uint16_t len) {
            delivered_seqs.push_back(seq);
            delivered_bytes.emplace_back(data, data + len);
        },
        [&](const moldudp64::RequestPacket& req) {
            ++gap_requests_sent;
            client.send_to("127.0.0.1", kRequestServerPort, moldudp64::encode_request(req));
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (!session.ended() && !sender_failed.load() && !server_failed.load() &&
           std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mcast_recv.fd(), &rfds);
        FD_SET(client.fd(), &rfds);
        const int maxfd = std::max(mcast_recv.fd(), client.fd());
        timeval tv{0, 50000};   // 50ms
        const int r = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        for (int fd : {mcast_recv.fd(), client.fd()}) {
            if (!FD_ISSET(fd, &rfds)) continue;
            std::vector<uint8_t> raw = (fd == mcast_recv.fd()) ? mcast_recv.receive() : [&] {
                std::string ip;
                uint16_t port;
                return client.receive_from(ip, port);
            }();
            if (raw.empty()) continue;

            moldudp64::DownstreamHeader hdr;
            std::vector<moldudp64::MessageBlockView> blocks;
            if (moldudp64::decode_downstream(raw.data(), raw.size(), hdr, blocks)) {
                if (hdr.message_count == moldudp64::kHeartbeat) ++heartbeats_seen;
                session.on_packet(hdr, blocks);
            }
        }
    }

    sender_thread.join();
    server_thread.join();

    expect(!sender_failed.load(), "sender thread completed without throwing");
    expect(!server_failed.load(), "re-request server thread completed without throwing");
    expect(heartbeats_seen >= 1, "at least one real heartbeat packet was received over the real multicast socket");
    expect(delivered_seqs.size() == static_cast<size_t>(kNumMessages),
           "all 4 messages eventually delivered, including the two recovered via a heartbeat-triggered request");
    bool in_order = delivered_seqs.size() == static_cast<size_t>(kNumMessages);
    for (size_t i = 0; in_order && i < delivered_seqs.size(); ++i) in_order &= (delivered_seqs[i] == i + 1);
    expect(in_order, "delivered in strict sequence order");
    bool bytes_ok = delivered_bytes.size() == real_msgs.size();
    for (size_t i = 0; bytes_ok && i < real_msgs.size(); ++i) bytes_ok &= (delivered_bytes[i] == real_msgs[i]);
    expect(bytes_ok, "delivered message bytes match the real source exactly");
    expect(gap_requests_sent >= 1,
           "a real gap request was sent over a real unicast socket — and nothing but a heartbeat could have "
           "triggered it, since packet2 was never sent at all before the first heartbeat arrived");
    expect(session.gaps_detected() >= 1, "session recorded the gap");
    expect(session.ended(), "real end-of-session packet (matching next_expected) was received and processed");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
