// Per-phase breakdown of add(): where does one order's processing time
// actually go? Answers this with real std::chrono samples taken inside
// OrderBookV2 itself (guarded by OBV2_PROFILE_BREAKDOWN — see
// order_book_v2.h), not a guess.
//
// Must be compiled with -DOBV2_PROFILE_BREAKDOWN, e.g. (run from cpp/):
//   g++ -std=c++20 -O3 -DOBV2_PROFILE_BREAKDOWN -Iinclude -Ibench src/order_book_v2.cpp bench/bench_breakdown.cpp -o bench_breakdown
//   ./bench_breakdown
//
// Honest caveat, read this before trusting the absolute numbers: every
// phase boundary below is itself a std::chrono::steady_clock::now() call,
// and that call has its own real, nonzero cost — typically tens of
// nanoseconds, machine-dependent. This build times ~5 chrono calls per
// add() instead of the usual 2 (bench_v2.cpp's before/after only), so this
// binary's own throughput and total add() latency will be measurably
// SLOWER than the uninstrumented numbers in bench_v2.cpp — that's expected
// overhead from the act of measuring, not a regression. What this *is*
// good for: the relative split between phases (what % of add() is
// matching vs. arena alloc vs. the hash-map insert vs. everything else),
// since the same per-call chrono overhead is baked into every phase
// roughly equally and mostly cancels out in a ratio.
#include "order_book_v2.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#ifndef OBV2_PROFILE_BREAKDOWN
#error "bench_breakdown.cpp must be compiled with -DOBV2_PROFILE_BREAKDOWN (see comment above)"
#endif

static void report_phase(const char* name, uint64_t total_ns, uint64_t calls, double add_wall_ns) {
    if (calls == 0) {
        std::printf("  %-16s   (never entered this run)\n", name);
        return;
    }
    const double avg_ns = static_cast<double>(total_ns) / static_cast<double>(calls);
    const double pct_of_add = 100.0 * static_cast<double>(total_ns) / add_wall_ns;
    std::printf("  %-16s calls=%-8llu total=%9.3fms  avg=%7.1fns/call  ~%5.1f%% of add() wall time\n",
        name, (unsigned long long)calls, total_ns / 1e6, avg_ns, pct_of_add);
}

int main() {
    constexpr int N = 2000000;  // same workload shape as bench_v2.cpp, for comparability
    OrderBookV2 book;
    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    uint64_t next_id = 1;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        OrderRequest o;
        o.id = next_id++;
        o.side = side_dist(rng) ? Side::Buy : Side::Sell;
        o.type = Type::Limit;
        o.price = 10000 + price_offset(rng);
        o.qty = static_cast<Quantity>(qty_dist(rng));
        book.add(o);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double add_wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    const auto& b = book.breakdown();

    std::printf("=== add() phase breakdown, %d ops (instrumented build — see header caveat) ===\n", N);
    std::printf("total wall time (this instrumented run): %.3fms\n\n", add_wall_ns / 1e6);
    report_phase("match()",        b.match_ns,         b.match_calls,         add_wall_ns);
    report_phase("ensure_index()", b.ensure_index_ns,  b.ensure_index_calls,  add_wall_ns);
    report_phase("arena_alloc",    b.arena_alloc_ns,   b.arena_alloc_calls,   add_wall_ns);
    report_phase("fifo_link",      b.fifo_link_ns,     b.fifo_link_calls,     add_wall_ns);
    report_phase("index_insert",   b.index_insert_ns,  b.index_insert_calls, add_wall_ns);

    const uint64_t accounted_ns = b.match_ns + b.ensure_index_ns + b.arena_alloc_ns + b.fifo_link_ns + b.index_insert_ns;
    std::printf("\naccounted for: %.1f%% of wall time (the rest is loop overhead, the chrono calls"
                " themselves, and this benchmark's own random-number generation — not part of add())\n",
        100.0 * static_cast<double>(accounted_ns) / add_wall_ns);
    std::printf("final depth=%zu  levels_ growth/rebase events=%llu\n",
        book.depth(), (unsigned long long)book.level_array_growths());
    return 0;
}
