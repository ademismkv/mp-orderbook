#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "moldudp64.h"
#include "moldudp64_session.h"
#include "udp_multicast.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <sys/select.h>
#include <thread>
#include <vector>

// End-to-end test over REAL sockets: a sender thread publishes real ITCH
// message bytes wrapped in MoldUDP64 framing to a real UDP multicast group
// on loopback; a deliberately-withheld packet simulates a dropped
// datagram; a re-request server thread answers the resulting retransmit
// request over a real unicast socket, exactly like MoldUDP64's actual
// recovery path. The main thread is the Feed Handler: joins the
// multicast group with real IP_ADD_MEMBERSHIP, receives real datagrams,
// decodes MoldUDP64 framing, drives a moldudp64::Session, and hands
// completed messages to the ITCH 5.0 parser — the whole chain, not just
// the framing logic tested in isolation (see test_moldudp64.cpp).
//
// This is the honest version of "receive UDP multicast": there's no live
// exchange feed available to subscribe to, so loopback is the stand-in,
// but every socket call here is real — socket()/bind()/sendto()/
// recvfrom()/setsockopt(IP_ADD_MEMBERSHIP), not simulated.

namespace {
constexpr const char* kGroup = "239.255.13.37";
constexpr uint16_t kMcastPort = 17331;
constexpr uint16_t kRequestServerPort = 17332;
constexpr uint16_t kClientReplyPort = 17333;
constexpr int kNumMessages = 10;
constexpr int kDropPacketIndex = 2;   // 0-based: the 3rd packet (seq 5-6) never goes out over multicast

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

    // Real message bytes.
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
    std::memcpy(session_id.data(), "E2ETEST01", 9);

    // Pack into packets of 2 messages each, keyed by starting sequence —
    // this is also the re-request server's lookup table.
    std::map<uint64_t, std::vector<uint8_t>> packets_by_seq;
    for (int i = 0; i < kNumMessages; i += 2) {
        auto pkt = moldudp64::encode_downstream(session_id, static_cast<uint64_t>(i + 1),
                                                 {real_msgs[i], real_msgs[i + 1]});
        packets_by_seq[static_cast<uint64_t>(i + 1)] = std::move(pkt);
    }
    const uint64_t dropped_seq = static_cast<uint64_t>(kDropPacketIndex * 2 + 1);

    // --- re-request server thread: answers exactly one retransmit request ---
    std::thread server_thread([&] {
        netfeed::UnicastSocket server(kRequestServerPort);
        std::string from_ip;
        uint16_t from_port = 0;
        auto raw = server.receive_from(from_ip, from_port);
        moldudp64::RequestPacket req;
        if (moldudp64::decode_request(raw.data(), raw.size(), req)) {
            auto it = packets_by_seq.find(req.sequence_number);
            if (it != packets_by_seq.end()) {
                server.send_to(from_ip, from_port, it->second);
            }
        }
    });

    // --- sender thread: publishes everything except the dropped packet ---
    std::thread sender_thread([&] {
        netfeed::McastSender sender(kGroup, kMcastPort);
        // Give the receiver a moment to join the group before anything is sent.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (const auto& [seq, pkt] : packets_by_seq) {
            if (seq == dropped_seq) continue;
            sender.send(pkt);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // --- main thread: the actual Feed Handler receive path ---
    netfeed::McastReceiver mcast_recv(kGroup, kMcastPort);
    netfeed::UnicastSocket client(kClientReplyPort);

    std::vector<uint64_t> delivered_seqs;
    std::vector<std::vector<uint8_t>> delivered_bytes;
    int gap_requests_sent = 0;

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

    // select() loop over both real sockets, real receive path.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (static_cast<int>(delivered_seqs.size()) < kNumMessages &&
           std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mcast_recv.fd(), &rfds);
        FD_SET(client.fd(), &rfds);
        const int maxfd = std::max(mcast_recv.fd(), client.fd());
        timeval tv{0, 100000};   // 100ms
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
                session.on_packet(hdr, blocks);
            }
        }
    }

    sender_thread.join();
    server_thread.join();

    expect(delivered_seqs.size() == static_cast<size_t>(kNumMessages),
           "all messages delivered end-to-end over real sockets, including the retransmitted one");
    bool in_order = delivered_seqs.size() == static_cast<size_t>(kNumMessages);
    for (size_t i = 0; in_order && i < delivered_seqs.size(); ++i) in_order &= (delivered_seqs[i] == i + 1);
    expect(in_order, "delivered in strict sequence order despite real out-of-order UDP arrival");
    bool bytes_ok = delivered_bytes.size() == real_msgs.size();
    for (size_t i = 0; bytes_ok && i < real_msgs.size(); ++i) bytes_ok &= (delivered_bytes[i] == real_msgs[i]);
    expect(bytes_ok, "every delivered message's bytes match the real source exactly");
    expect(gap_requests_sent >= 1, "at least one real retransmit request was sent over a real unicast socket");
    expect(session.gaps_detected() >= 1, "session recorded the gap");

    // Confirm every delivered message also parses cleanly through the
    // real ITCH parser — the whole chain, not just MoldUDP64 framing.
    int parse_failures = 0;
    for (size_t i = 0; i < delivered_bytes.size(); ++i) {
        auto parsed = itch::parse_message(delivered_bytes[i].data(), delivered_bytes[i].size());
        if (!parsed) ++parse_failures;
    }
    expect(parse_failures == 0, "every message delivered over the real socket path parses via itch::parse_message");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
