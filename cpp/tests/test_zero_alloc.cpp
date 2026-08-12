#include "order_book_v2.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

// Turns "the arena design means no malloc on the hot path" from a claim
// this repo makes in README.md into something actually checked, the same
// way level_array_growths() turned "rebases are rare" from a guess into a
// measured fact (see ADR-2). Overriding the global operator new/delete for
// this whole executable and counting calls is a blunt instrument, but it's
// an honest one: it can't be fooled by an allocation that happens to be
// small or short-lived the way sampling a profiler can.
//
// This test is what found a real gap: free_list_ (order_book_v2.cpp) had
// no upfront reserve(), so the first several cancel()s after construction
// each triggered a real heap growth as the free list ramped up — a genuine
// allocation on the hot path that nothing before this test had ever
// caught, because no prior test or benchmark counted allocations at all.
// Fixed with a `free_list_.reserve(arena_capacity)` alongside the
// already-existing `index_.reserve(arena_capacity)`.
namespace {
uint64_t g_alloc_count = 0;
uint64_t g_alloc_bytes = 0;
bool g_counting = false;
} // namespace

void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_alloc_count;
        g_alloc_bytes += size;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
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

int main() {
    // Generous, fixed-size setup so the measured region never triggers a
    // legitimate, expected allocation (levels_ growth/rebase, arena
    // exhaustion) — this test is about the STEADY-STATE hot path, not a
    // claim that construction or a genuine window-outgrow is free. Every
    // real caller in this repo (bench_v2.cpp, replay_lobster.cpp,
    // Sequencer's shards) already sizes these generously up front for
    // exactly this reason.
    OrderBookV2 book(1u << 16, 20000);
    std::vector<Trade> trades;
    trades.reserve(8);   // matches Sequencer's scratch_trades_ pattern

    // --- warmup (NOT measured): establishes have_base_/levels_, and
    // exercises one real cross so `trades`'s capacity is primed too.
    // Also cycles enough add+cancel pairs that anything with its own
    // internal ramp-up (there shouldn't be any, post-fix) settles before
    // the measured region starts.
    uint64_t next_id = 1;
    for (int i = 0; i < 4096; ++i) {
        OrderRequest sell{next_id++, Side::Sell, Type::Limit, 10100 + (i % 500), 10};
        book.add(sell, trades);
        OrderRequest cxl{sell.id, Side::Sell, Type::Cancel, 0, 0};
        book.add(cxl, trades);
    }
    {
        OrderRequest sell{next_id++, Side::Sell, Type::Limit, 10000, 5};
        book.add(sell, trades);
        OrderRequest buy{next_id++, Side::Buy, Type::Limit, 10000, 5};
        book.add(buy, trades);   // real cross — primes `trades`'s capacity
    }

    // --- measured region: steady-state mixed add/cancel/reduce/cross ---
    constexpr int kOps = 200000;
    std::vector<OrderId> resting;
    resting.reserve(4096);

    g_alloc_count = 0;
    g_alloc_bytes = 0;
    g_counting = true;

    for (int i = 0; i < kOps; ++i) {
        const int op = i % 4;
        if (op == 0 || resting.empty()) {
            // Add a non-crossing resting sell somewhere inside the window
            // established during warmup.
            OrderRequest req{next_id++, Side::Sell, Type::Limit, 10100 + (i % 500), 10};
            book.add(req, trades);
            resting.push_back(req.id);
        } else if (op == 1) {
            // A crossing buy — exercises match()'s trades.push_back path
            // against the already-reserved `trades` buffer.
            OrderRequest req{next_id++, Side::Buy, Type::Limit, 10100 + (i % 500), 5};
            book.add(req, trades);
        } else if (op == 2) {
            OrderId id = resting.back();
            resting.pop_back();
            OrderRequest cxl{id, Side::Sell, Type::Cancel, 0, 0};
            book.add(cxl, trades);
        } else {
            OrderId id = resting.back();
            book.reduce(id, 1);
        }
    }

    g_counting = false;

    std::printf("=== zero-allocation hot-path check ===\n");
    std::printf("ops in measured region: %d\n", kOps);
    std::printf("allocations during measured region: %llu (%llu bytes)\n", (unsigned long long)g_alloc_count,
                (unsigned long long)g_alloc_bytes);
    std::printf("level_array_growths(): %llu (should be 0 — window sized generously in setup)\n",
                (unsigned long long)book.level_array_growths());

    expect(g_alloc_count == 0, "zero heap allocations across 200,000 steady-state add/cancel/reduce/cross ops");
    expect(book.level_array_growths() == 0, "no level-array growth/rebase during the measured region (setup was sized for it)");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
