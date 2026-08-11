#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "market_data.h"
#include "moldudp64.h"
#include "moldudp64_session.h"
#include "sequencer.h"
#include "udp_multicast.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sys/select.h>
#include <thread>
#include <unordered_map>
#include <vector>

// End-to-end test of ROADMAP.md's last pipeline stage: Sequencer -> Order
// Books -> Risk -> Execution Reports -> MARKET DATA FEED. Runs the full
// real 691,421-message file through the Sequencer (as in day 17/18),
// capturing every public market data event it emits, then actually
// publishes those events over a real UDP multicast socket wrapped in real
// MoldUDP64 framing (reusing day 14/15's moldudp64.h and udp_multicast.h
// verbatim) — including deliberately dropping several real packets to
// exercise the real gap-detection/retransmission path at this message
// volume, not just the single-drop toy case day 15 proved the mechanism
// with. A second thread subscribes independently, decodes ONLY the market
// data wire bytes (no access to the Sequencer's internal state), and
// reconstructs a shadow order book — the actual test: does the public feed
// alone carry enough information for a subscriber to know every symbol's
// best bid/ask, matching the real engine's true state exactly.
namespace {
constexpr const char* kGroup = "239.255.13.38";
constexpr uint16_t kMcastPort = 17341;
constexpr uint16_t kRequestServerPort = 17342;
constexpr uint16_t kClientReplyPort = 17343;
// 27 events/packet keeps the MoldUDP64 payload at 20 + 27*50 = 1370 bytes —
// comfortably under 1472, the largest UDP payload a standard 1500-byte
// Ethernet MTU can carry without IP fragmentation. This used to be 200
// (10,420 bytes/packet), which worked on this repo's Linux sandbox but
// hit "Message too long" (EMSGSIZE) on a real Mac: macOS enforces a
// stricter default multicast datagram ceiling than Linux's, and real
// multicast market data feeds deliberately avoid fragmentation anyway —
// a single lost IP fragment silently kills the whole reassembled
// datagram, so staying under one MTU isn't just portability, it's the
// actual correct practice for a real feed. See devlog day 19 addendum.
constexpr size_t kEventsPerPacket = 27;
constexpr int kNumDrops = 5;

int g_failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

struct ShadowOrder {
    Side side;
    Price price;
    Quantity qty;
};
} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    // --- Phase 1: run the real pipeline, capture every market data event ---
    std::unordered_map<std::string, std::unique_ptr<OrderBookV2>> shards;
    auto book_for = [&](const std::string& symbol) -> OrderBookV2& {
        auto it = shards.find(symbol);
        if (it == shards.end()) it = shards.emplace(symbol, std::make_unique<OrderBookV2>(128, 200)).first;
        return *it->second;
    };

    Sequencer seq;
    RiskEngine risk;
    std::vector<mdfeed::MDEvent> events;
    events.reserve(500000);
    auto md_cb = [&](const mdfeed::MDEvent& e) { events.push_back(e); };

    itch::BinaryFileReader reader(path);
    uint64_t total_messages = 0, parse_failures = 0;
    while (auto raw = reader.next_raw()) {
        ++total_messages;
        auto parsed = itch::parse_message(raw->data(), raw->size());
        if (!parsed) {
            ++parse_failures;
            continue;
        }
        seq.on_message(*parsed, book_for, &risk, {}, md_cb);
    }

    std::printf("=== Market Data Feed: real pipeline run ===\n");
    std::printf("total messages: %llu (parse failures: %llu)\n", (unsigned long long)total_messages,
                (unsigned long long)parse_failures);
    std::printf("symbols sharded: %zu\n", shards.size());
    std::printf("market data events captured: %zu\n", events.size());

    // Snapshot the TRUE final per-symbol book state — what a subscriber
    // reconstructing from the feed alone needs to match.
    struct TrueState {
        bool has_bid, has_ask;
        Price best_bid, best_ask;
    };
    std::unordered_map<std::string, TrueState> true_state;
    for (const auto& [sym, book] : shards) {
        true_state[sym] = TrueState{book->has_bid(), book->has_ask(),
                                     book->has_bid() ? book->best_bid() : 0, book->has_ask() ? book->best_ask() : 0};
    }

    // --- Phase 2: pre-encode every event into real MoldUDP64 downstream packets ---
    moldudp64::SessionId session_id{};
    std::memcpy(session_id.data(), "MDFEED001", 9);

    std::map<uint64_t, std::vector<uint8_t>> packets_by_seq;
    {
        uint64_t seq = 1;
        for (size_t i = 0; i < events.size(); i += kEventsPerPacket) {
            std::vector<std::vector<uint8_t>> batch;
            const size_t end = std::min(events.size(), i + kEventsPerPacket);
            for (size_t j = i; j < end; ++j) batch.push_back(mdfeed::encode(events[j]));
            packets_by_seq[seq] = moldudp64::encode_downstream(session_id, seq, batch);
            seq += batch.size();
        }
    }
    const uint64_t total_events = events.size();
    std::printf("packets to publish: %zu\n", packets_by_seq.size());

    // Deliberately drop several real packets, spread across the run, to
    // exercise real gap detection/retransmission at scale rather than the
    // single-drop case day 15 proved the mechanism with.
    std::vector<uint64_t> all_seqs;
    all_seqs.reserve(packets_by_seq.size());
    for (const auto& [s, _] : packets_by_seq) all_seqs.push_back(s);
    std::set<uint64_t> dropped_seqs;
    if (all_seqs.size() > static_cast<size_t>(kNumDrops) + 2) {
        for (int i = 1; i <= kNumDrops; ++i) {
            dropped_seqs.insert(all_seqs[all_seqs.size() * i / (kNumDrops + 1)]);
        }
    }
    std::printf("packets deliberately dropped (real recovery must fill these): %zu\n", dropped_seqs.size());

    // --- re-request server thread: answers retransmit requests for as long
    // as the subscriber is still catching up (deliberate drops PLUS
    // whatever real loopback loss actually occurs at this volume — real
    // UDP has no delivery guarantee, and pretending only the deliberate
    // drops happen would be dishonest about what "real sockets" means).
    // Real socket errors (EMSGSIZE, ENODEV, etc.) must surface as a clean,
    // diagnosable test failure — not std::terminate. A std::thread whose
    // function lets an exception escape calls std::terminate and aborts
    // the WHOLE process with no useful output, which is exactly what
    // happened when a too-large packet tripped McastSender::send() on a
    // real Mac (see devlog day 19 addendum). Every thread body below is
    // wrapped accordingly.
    std::atomic<bool> stop_server{false};
    std::atomic<int> requests_answered{0};
    std::atomic<bool> server_failed{false};
    std::thread server_thread([&] {
        try {
            netfeed::UnicastSocket server(kRequestServerPort);
            while (!stop_server.load()) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(server.fd(), &rfds);
                timeval tv{0, 50000};   // 50ms — tighter than the old 200ms so a real gap
                                        // recovery round trip doesn't stack up multiple
                                        // polling waits into visible extra latency.
                if (::select(server.fd() + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
                std::string from_ip;
                uint16_t from_port = 0;
                auto raw = server.receive_from(from_ip, from_port);
                moldudp64::RequestPacket req;
                if (moldudp64::decode_request(raw.data(), raw.size(), req)) {
                    auto it = packets_by_seq.find(req.sequence_number);
                    if (it != packets_by_seq.end()) {
                        server.send_to(from_ip, from_port, it->second);
                        ++requests_answered;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::printf("FAIL: re-request server thread threw: %s\n", e.what());
            server_failed.store(true);
        }
    });

    // --- sender thread: publishes the real feed over real UDP multicast.
    // Paced in small bursts (1ms sleep every 5 packets, ~7KB/ms ≈ 7MB/s)
    // rather than one sleep per packet — with 27-event packets there are
    // ~8x more of them than the old 200-event batching, and sleeping on
    // every single one would spend most of the run in intentional sleep
    // for no reliability benefit. The target rate mirrors what was
    // already proven not to overrun the receiver's socket buffer at the
    // old (larger) packet size.
    std::atomic<bool> sender_failed{false};
    std::thread sender_thread([&] {
        try {
            netfeed::McastSender sender(kGroup, kMcastPort);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int n = 0;
            for (const auto& [s, pkt] : packets_by_seq) {
                if (dropped_seqs.count(s)) continue;
                sender.send(pkt);
                if (++n % 5 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sender.send(moldudp64::encode_end_of_session(session_id, total_events + 1));
        } catch (const std::exception& e) {
            std::printf("FAIL: sender thread threw: %s\n", e.what());
            sender_failed.store(true);
        }
    });

    // --- main thread: the market data SUBSCRIBER — knows nothing but the wire feed ---
    netfeed::McastReceiver mcast_recv(kGroup, kMcastPort);
    int rcvbuf = 8 * 1024 * 1024;
    ::setsockopt(mcast_recv.fd(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    socklen_t rcvbuf_len = sizeof(rcvbuf);
    int actual_rcvbuf = 0;
    ::getsockopt(mcast_recv.fd(), SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &rcvbuf_len);
    std::printf("receive socket buffer: requested %d, kernel gave %d\n", rcvbuf, actual_rcvbuf);
    netfeed::UnicastSocket client(kClientReplyPort);

    std::unordered_map<std::string, std::unordered_map<OrderId, ShadowOrder>> shadow_books;
    uint64_t delivered_count = 0;
    int gap_requests_sent = 0;

    moldudp64::Session session(
        session_id,
        [&](uint64_t /*seq*/, const uint8_t* data, uint16_t len) {
            ++delivered_count;
            auto ev = mdfeed::decode(data, len);
            if (!ev) return;
            const std::string symbol = mdfeed::symbol_str(ev->symbol);
            auto& book = shadow_books[symbol];
            switch (ev->type) {
            case mdfeed::EventType::Add:
                book[ev->order_id] = ShadowOrder{ev->side, ev->price, ev->qty};
                break;
            case mdfeed::EventType::Cancel:
            case mdfeed::EventType::Trade: {
                auto it = book.find(ev->order_id);
                if (it == book.end()) break;   // defensive — see market_data.h scope note
                if (ev->qty >= it->second.qty) book.erase(it);
                else it->second.qty -= ev->qty;
                break;
            }
            case mdfeed::EventType::Delete:
                book.erase(ev->order_id);
                break;
            case mdfeed::EventType::Replace:
                book.erase(ev->order_id);
                book[ev->new_order_id] = ShadowOrder{ev->side, ev->price, ev->qty};
                break;
            }
        },
        [&](const moldudp64::RequestPacket& req) {
            ++gap_requests_sent;
            client.send_to("127.0.0.1", kRequestServerPort, moldudp64::encode_request(req));
        });

    // Stop condition is delivered_count reaching the real total, NOT
    // session.ended() alone — end-of-session's own sequence number can
    // arrive (and set ended()) before every outstanding gap it implies has
    // actually been retransmitted and delivered; stopping on ended() alone
    // would race a still-in-flight recovery and misreport it as data loss.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (delivered_count < total_events && !sender_failed.load() && !server_failed.load() &&
           std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mcast_recv.fd(), &rfds);
        FD_SET(client.fd(), &rfds);
        const int maxfd = std::max(mcast_recv.fd(), client.fd());
        timeval tv{0, 50000};   // 50ms — see server thread's matching comment above
        if (::select(maxfd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;

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
            if (moldudp64::decode_downstream(raw.data(), raw.size(), hdr, blocks)) session.on_packet(hdr, blocks);
        }
    }

    sender_thread.join();
    stop_server.store(true);
    server_thread.join();

    std::printf("\n=== subscriber (market-data-only view) results ===\n");
    std::printf("delivered: %llu / %llu\n", (unsigned long long)delivered_count, (unsigned long long)total_events);
    std::printf("gap requests sent: %d, answered: %d (packets deliberately dropped: %zu; the rest, if any, is real "
                "loopback UDP loss under load — not simulated)\n",
                gap_requests_sent, requests_answered.load(), dropped_seqs.size());
    std::printf("shadow-reconstructed symbols: %zu (true symbols: %zu)\n", shadow_books.size(), true_state.size());

    expect(!sender_failed.load(), "sender thread completed without a real socket error");
    expect(!server_failed.load(), "re-request server thread completed without a real socket error");

    // End-of-session is sent exactly once, with no retry, unlike real data
    // packets (which are individually recoverable via a Request Packet).
    // Losing it over real UDP is a real, honest possibility — it just
    // means the LAST thing that can be lost is "nothing more is coming",
    // not any actual data, since that's tracked independently below by
    // delivered_count. Not a correctness failure of the feed itself.
    std::printf("end-of-session received: %s\n", session.ended() ? "yes" : "no (lost over real UDP — informational, "
                                                                            "not a data-completeness failure)");
    expect(delivered_count == total_events,
           "every published market data event eventually delivered exactly once, via real gap recovery "
           "for whatever was actually lost over real UDP");
    expect(gap_requests_sent >= static_cast<int>(dropped_seqs.size()),
           "at least one real gap-recovery request sent per deliberately dropped packet");
    expect(session.gaps_detected() >= dropped_seqs.size(),
           "session recorded at least the deliberate gaps (real loopback loss may add more, honestly)");

    int mismatches = 0;
    for (const auto& [sym, truth] : true_state) {
        bool has_bid = false, has_ask = false;
        Price best_bid = 0, best_ask = 0;
        auto it = shadow_books.find(sym);
        if (it != shadow_books.end()) {
            for (const auto& [oid, o] : it->second) {
                if (o.qty == 0) continue;
                if (o.side == Side::Buy) {
                    if (!has_bid || o.price > best_bid) {
                        has_bid = true;
                        best_bid = o.price;
                    }
                } else {
                    if (!has_ask || o.price < best_ask) {
                        has_ask = true;
                        best_ask = o.price;
                    }
                }
            }
        }
        if (has_bid != truth.has_bid || has_ask != truth.has_ask || (has_bid && best_bid != truth.best_bid) ||
            (has_ask && best_ask != truth.best_ask)) {
            ++mismatches;
            if (mismatches <= 5) {
                std::printf("MISMATCH %s: true(bid=%d:%lld ask=%d:%lld) shadow(bid=%d:%lld ask=%d:%lld)\n",
                            sym.c_str(), truth.has_bid, (long long)truth.best_bid, truth.has_ask,
                            (long long)truth.best_ask, has_bid, (long long)best_bid, has_ask, (long long)best_ask);
            }
        }
    }
    std::printf("symbol best-bid/ask mismatches: %d / %zu\n", mismatches, true_state.size());
    expect(mismatches == 0,
           "shadow book reconstructed from ONLY the public market data feed matches the true engine's "
           "best bid/ask for every symbol");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
