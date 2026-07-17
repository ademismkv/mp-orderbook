#include "order_book_v2.h"
#include "spsc_ring.h"
#include "workload.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

// ADR-1's actual claim: producer -> SPSC ring -> single matching thread ->
// SPSC ring -> consumer must be EXACTLY equivalent to calling
// OrderBookV2::add() directly and sequentially (which fuzz_v2.cpp already
// checked against v1). This test builds the real threaded pipeline and
// diffs it against the sequential reference, then is meant to be run under
// ThreadSanitizer (see Makefile-less build commands in README) — matching
// output numbers alone doesn't prove there's no data race; TSan checks the
// memory model, not just the result.

struct WireOp {
    bool is_cancel;
    OrderRequest req;
};

int main(int argc, char** argv) {
    const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 42;
    const int count = argc > 2 ? std::atoi(argv[2]) : 100000;
    auto ops = generate_ops(seed, count);

    // --- reference: sequential, single-threaded, ground truth ---
    OrderBookV2 ref_book;
    uint64_t ref_trades = 0, ref_qty = 0;
    Checksum ref_cs;
    for (auto& op : ops) {
        if (op.kind == OpKind::Cancel) {
            ref_book.cancel(op.id);
            continue;
        }
        OrderRequest o{op.id, op.is_buy ? Side::Buy : Side::Sell,
                       op.kind == OpKind::AddLimit ? Type::Limit : Type::Market,
                       op.price, op.qty};
        for (auto& t : ref_book.add(o)) {
            ref_trades++;
            ref_qty += t.qty;
            ref_cs.mix(t.maker_id);
            ref_cs.mix(t.taker_id);
            ref_cs.mix(static_cast<uint64_t>(t.price));
            ref_cs.mix(t.qty);
        }
    }

    // --- threaded: producer thread -> SPSC ring -> matcher thread -> SPSC ring -> this (consumer) thread ---
    SpscRingBuffer<WireOp> in_ring(1u << 16);
    SpscRingBuffer<Trade> out_ring(1u << 16);
    std::atomic<bool> producer_done{false};
    std::atomic<bool> matcher_done{false};

    std::thread producer([&]() {
        for (auto& op : ops) {
            WireOp w{};
            if (op.kind == OpKind::Cancel) {
                w.is_cancel = true;
                w.req = OrderRequest{op.id, Side::Buy, Type::Cancel, 0, 0};
            } else {
                w.is_cancel = false;
                w.req = OrderRequest{op.id, op.is_buy ? Side::Buy : Side::Sell,
                                      op.kind == OpKind::AddLimit ? Type::Limit : Type::Market,
                                      op.price, op.qty};
            }
            while (!in_ring.push(w)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread matcher([&]() {
        OrderBookV2 book;  // this thread's book — the only thread that ever touches it (single-writer)
        WireOp w;
        while (true) {
            if (in_ring.pop(w)) {
                if (w.is_cancel) {
                    book.cancel(w.req.id);
                } else {
                    for (auto& t : book.add(w.req)) {
                        while (!out_ring.push(t)) {
                            std::this_thread::yield();
                        }
                    }
                }
            } else if (producer_done.load(std::memory_order_acquire) && in_ring.empty()) {
                break;
            }
        }
        matcher_done.store(true, std::memory_order_release);
    });

    uint64_t threaded_trades = 0, threaded_qty = 0;
    Checksum threaded_cs;
    Trade t;
    while (true) {
        if (out_ring.pop(t)) {
            threaded_trades++;
            threaded_qty += t.qty;
            threaded_cs.mix(t.maker_id);
            threaded_cs.mix(t.taker_id);
            threaded_cs.mix(static_cast<uint64_t>(t.price));
            threaded_cs.mix(t.qty);
        } else if (matcher_done.load(std::memory_order_acquire) && out_ring.empty()) {
            break;
        }
    }

    producer.join();
    matcher.join();

    std::printf("reference: trades=%llu qty=%llu checksum=%llu\n",
        (unsigned long long)ref_trades, (unsigned long long)ref_qty, (unsigned long long)ref_cs.h);
    std::printf("threaded:  trades=%llu qty=%llu checksum=%llu\n",
        (unsigned long long)threaded_trades, (unsigned long long)threaded_qty, (unsigned long long)threaded_cs.h);

    const bool ok = (ref_trades == threaded_trades) && (ref_qty == threaded_qty) && (ref_cs.h == threaded_cs.h);
    std::printf(ok ? "MATCH\n" : "MISMATCH\n");
    return ok ? 0 : 1;
}
