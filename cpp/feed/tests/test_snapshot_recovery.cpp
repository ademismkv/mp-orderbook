#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "market_data.h"
#include "moldudp64.h"
#include "moldudp64_session.h"
#include "sequencer.h"
#include "snapshot.h"
#include "tcp_socket.h"
#include "udp_multicast.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sys/select.h>
#include <thread>
#include <unordered_map>
#include <vector>

// Real snapshot recovery, end to end: a subscriber that joins the Market
// Data Feed MID-STREAM — it never sees the first half of the real event
// history at all, not even via retransmission — must still end up with the
// exact correct final book state, using nothing but a real TCP snapshot
// exchange (see snapshot.h, tcp_socket.h) plus the back half of the live
// UDP multicast feed. This is the real gap ADR-5/devlog day 22 identified:
// MoldUDP64 gap recovery (test_udp_multicast_e2e.cpp,
// test_udp_multicast_heartbeat.cpp) only recovers individual dropped
// packets within a session a subscriber is already participating in — it
// has no answer for "I joined after sequence N started" or "I fell so far
// behind that point-to-point retransmission of everything since sequence 1
// isn't practical." A real snapshot service is the actual answer, same as
// real Nasdaq systems (GLIMPSE) use.
//
// Scenario:
//   1. Run the full real pipeline (Sequencer -> Risk -> Order Books) over
//      the real 691,421-message ITCH file, capturing every market data
//      event with an assigned sequence number, same as
//      test_market_data_feed.cpp.
//   2. Split the event stream at its midpoint. A snapshot::Builder applies
//      ONLY the first half — this is the "as of sequence N" snapshot state
//      a real snapshot service would have built by continuously consuming
//      its own feed.
//   3. A real TCP server thread serves that snapshot, length-prefixed,
//      over one real connection.
//   4. A real UDP multicast sender publishes ONLY the second half of the
//      events (sequence N+1 onward) — the first half is never sent over
//      multicast at all in this test, faithfully simulating "gone by the
//      time this subscriber joined." A few packets in the live half are
//      deliberately dropped too, to prove ordinary gap recovery still
//      composes correctly with a session that didn't start at sequence 1.
//   5. The subscriber: connects over real TCP, loads the snapshot, starts
//      its moldudp64::Session with start_seq = snapshot_seq + 1, joins the
//      real multicast group, and reconstructs the rest via the live feed
//      plus real gap recovery for whatever was dropped.
//   6. Final check: the subscriber's reconstructed best-bid/ask for every
//      real symbol must exactly match the true engine's actual final
//      state — proving it ended up fully correct despite never having
//      received (or been able to request) the first half of history.

namespace {
constexpr const char* kGroup = "239.255.13.39";
constexpr uint16_t kMcastPort = 17351;
constexpr uint16_t kRequestServerPort = 17352;
constexpr uint16_t kClientReplyPort = 17353;
constexpr uint16_t kSnapshotTcpPort = 17354;
constexpr size_t kEventsPerPacket = 27;   // MTU-safe — see market_data.h / devlog day 19 addendum
constexpr int kNumDrops = 3;

int g_failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}
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
    uint64_t total_messages = 0;
    while (auto raw = reader.next_raw()) {
        ++total_messages;
        auto parsed = itch::parse_message(raw->data(), raw->size());
        if (!parsed) continue;
        seq.on_message(*parsed, book_for, &risk, {}, md_cb);
    }
    std::printf("=== Snapshot recovery: real pipeline run ===\n");
    std::printf("total messages: %llu, market data events: %zu\n", (unsigned long long)total_messages, events.size());

    struct TrueState {
        bool has_bid, has_ask;
        Price best_bid, best_ask;
    };
    std::unordered_map<std::string, TrueState> true_state;
    for (const auto& [sym, book] : shards) {
        true_state[sym] = TrueState{book->has_bid(), book->has_ask(),
                                     book->has_bid() ? book->best_bid() : 0, book->has_ask() ? book->best_ask() : 0};
    }

    // --- Phase 2: split the history; build the snapshot from the first half ---
    const size_t seq_split = events.size() / 2;
    snapshot::Builder snap_builder;
    for (size_t i = 0; i < seq_split; ++i) snap_builder.apply(static_cast<uint64_t>(i + 1), events[i]);
    const auto snapshot_rows = snap_builder.rows();
    const uint64_t snapshot_as_of = snap_builder.sequence();
    std::printf("snapshot built as of sequence %llu: %zu resting orders across %zu-event first half\n",
                (unsigned long long)snapshot_as_of, snapshot_rows.size(), seq_split);
    expect(snapshot_as_of == static_cast<uint64_t>(seq_split), "snapshot's as-of sequence matches the split point");

    // --- Phase 3: pre-encode the SECOND half only into real MoldUDP64 packets,
    // continuing the same global sequence numbering. The first half is never
    // published over multicast in this test at all. ---
    moldudp64::SessionId session_id{};
    std::memcpy(session_id.data(), "SNAPTEST1", 9);

    std::map<uint64_t, std::vector<uint8_t>> packets_by_seq;
    {
        uint64_t s = seq_split + 1;
        for (size_t i = seq_split; i < events.size(); i += kEventsPerPacket) {
            std::vector<std::vector<uint8_t>> batch;
            const size_t end = std::min(events.size(), i + kEventsPerPacket);
            for (size_t j = i; j < end; ++j) batch.push_back(mdfeed::encode(events[j]));
            packets_by_seq[s] = moldudp64::encode_downstream(session_id, s, batch);
            s += batch.size();
        }
    }
    const uint64_t total_events = events.size();
    std::printf("live packets to publish (second half only): %zu\n", packets_by_seq.size());

    std::vector<uint64_t> live_seqs;
    live_seqs.reserve(packets_by_seq.size());
    for (const auto& [s, _] : packets_by_seq) live_seqs.push_back(s);
    std::set<uint64_t> dropped_seqs;
    if (live_seqs.size() > static_cast<size_t>(kNumDrops) + 2) {
        for (int i = 1; i <= kNumDrops; ++i) {
            dropped_seqs.insert(live_seqs[live_seqs.size() * i / (kNumDrops + 1)]);
        }
    }
    std::printf("live packets deliberately dropped: %zu\n", dropped_seqs.size());

    // --- TCP snapshot server thread: serves exactly one connection, sends
    // a real 8-byte big-endian length prefix followed by the encoded
    // snapshot payload — TCP has no message boundaries of its own, so the
    // client's recv_exact(8) then recv_exact(length) needs to know how
    // much to read. ---
    std::atomic<bool> snapshot_server_failed{false};
    std::thread snapshot_server_thread([&] {
        try {
            netfeed::TcpListener listener(kSnapshotTcpPort);
            auto conn = listener.accept_one();
            auto payload = snapshot::encode_snapshot(snapshot_as_of, snapshot_rows);
            std::vector<uint8_t> len_prefix(8);
            moldudp64::put_be64(len_prefix.data(), payload.size());
            conn.send_all(len_prefix);
            conn.send_all(payload);
        } catch (const std::exception& e) {
            std::printf("FAIL: snapshot TCP server thread threw: %s\n", e.what());
            snapshot_server_failed.store(true);
        }
    });

    // --- re-request server thread: real UDP gap recovery for the live half ---
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
                timeval tv{0, 50000};
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

    // --- sender thread: publishes the live (second) half over real UDP multicast ---
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

    // --- main thread: the late-joining subscriber ---
    // Step A: real TCP snapshot fetch, BEFORE touching the multicast group
    // at all — mirrors a real subscriber's bootstrap order (load state,
    // then start consuming the live feed from that point forward).
    snapshot::Builder subscriber_book;
    uint64_t loaded_snapshot_seq = 0;
    {
        auto conn = netfeed::tcp_connect("127.0.0.1", kSnapshotTcpPort);
        auto len_prefix = conn.recv_exact(8);
        const uint64_t payload_len = moldudp64::get_be64(len_prefix.data());
        auto payload = conn.recv_exact(static_cast<size_t>(payload_len));
        uint64_t decoded_seq = 0;
        std::vector<snapshot::SnapshotRow> decoded_rows;
        expect(snapshot::decode_snapshot(payload, decoded_seq, decoded_rows),
               "snapshot payload received over real TCP decodes cleanly");
        subscriber_book.load(decoded_seq, decoded_rows);
        loaded_snapshot_seq = decoded_seq;
        expect(decoded_rows.size() == snapshot_rows.size(),
               "subscriber received every snapshot row the server had (real TCP, no loss)");
    }
    snapshot_server_thread.join();
    expect(!snapshot_server_failed.load(), "snapshot TCP server thread completed without throwing");

    // Step B: join the live feed, starting the session where the snapshot
    // left off — NOT at sequence 1. Any live packet at or before
    // loaded_snapshot_seq would be treated as already-known; there are none
    // here, since the sender only ever publishes the second half.
    netfeed::McastReceiver mcast_recv(kGroup, kMcastPort);
    netfeed::UnicastSocket client(kClientReplyPort);
    uint64_t live_delivered = 0;
    int gap_requests_sent = 0;

    moldudp64::Session session(
        session_id,
        [&](uint64_t s, const uint8_t* data, uint16_t len) {
            ++live_delivered;
            auto ev = mdfeed::decode(data, len);
            if (ev) subscriber_book.apply(s, *ev);
        },
        [&](const moldudp64::RequestPacket& req) {
            ++gap_requests_sent;
            client.send_to("127.0.0.1", kRequestServerPort, moldudp64::encode_request(req));
        },
        /*start_seq=*/loaded_snapshot_seq + 1);

    const uint64_t total_live_events = total_events - seq_split;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (live_delivered < total_live_events && !sender_failed.load() && !server_failed.load() &&
           std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mcast_recv.fd(), &rfds);
        FD_SET(client.fd(), &rfds);
        const int maxfd = std::max(mcast_recv.fd(), client.fd());
        timeval tv{0, 50000};
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

    std::printf("live events delivered: %llu / %llu\n", (unsigned long long)live_delivered,
                (unsigned long long)total_live_events);
    std::printf("gap requests sent: %d, answered: %d (deliberately dropped: %zu)\n", gap_requests_sent,
                requests_answered.load(), dropped_seqs.size());

    expect(!sender_failed.load(), "sender thread completed without a real socket error");
    expect(!server_failed.load(), "re-request server thread completed without a real socket error");
    expect(live_delivered == total_live_events,
           "every live-half event eventually delivered, via real gap recovery composing correctly with a "
           "session that started past sequence 1");
    expect(gap_requests_sent >= static_cast<int>(dropped_seqs.size()),
           "at least one real gap-recovery request sent per deliberately dropped live packet");

    // Final check: the subscriber, which NEVER saw the first half of
    // history live or via retransmission, must still match the true
    // engine's actual final state — proof the snapshot really did carry
    // enough information to substitute for that missing history.
    const auto final_rows = subscriber_book.rows();
    std::unordered_map<std::string, TrueState> reconstructed;
    for (const auto& r : final_rows) {
        const std::string sym = mdfeed::symbol_str(r.symbol);
        auto& st = reconstructed[sym];
        if (r.side == Side::Buy) {
            if (!st.has_bid || r.price > st.best_bid) {
                st.has_bid = true;
                st.best_bid = r.price;
            }
        } else {
            if (!st.has_ask || r.price < st.best_ask) {
                st.has_ask = true;
                st.best_ask = r.price;
            }
        }
    }

    int mismatches = 0;
    for (const auto& [sym, truth] : true_state) {
        auto it = reconstructed.find(sym);
        const TrueState got = (it != reconstructed.end()) ? it->second : TrueState{false, false, 0, 0};
        if (got.has_bid != truth.has_bid || got.has_ask != truth.has_ask ||
            (got.has_bid && got.best_bid != truth.best_bid) || (got.has_ask && got.best_ask != truth.best_ask)) {
            ++mismatches;
            if (mismatches <= 5) {
                std::printf("MISMATCH %s: true(bid=%d:%lld ask=%d:%lld) subscriber(bid=%d:%lld ask=%d:%lld)\n",
                            sym.c_str(), truth.has_bid, (long long)truth.best_bid, truth.has_ask,
                            (long long)truth.best_ask, got.has_bid, (long long)got.best_bid, got.has_ask,
                            (long long)got.best_ask);
            }
        }
    }
    std::printf("symbol best-bid/ask mismatches: %d / %zu\n", mismatches, true_state.size());
    expect(mismatches == 0,
           "the late-joining subscriber (snapshot + second-half-only live feed) matches the true engine's "
           "final best bid/ask for every symbol — despite never receiving the first half of history");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
