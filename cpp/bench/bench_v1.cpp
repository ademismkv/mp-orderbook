#include "order_book.h"
#include "histogram.h"
#include <chrono>
#include <random>
#include <vector>

// Workload: mixed add() calls around a moving mid price, ~50/50 buy/sell,
// tight enough spread that a meaningful fraction of orders cross and match
// rather than all resting. This is the realistic case, not the easy case
// (an all-resting workload makes any order book look fast since it never
// exercises match()).
int main() {
    constexpr int N = 500000;  // v1 (std::map) is the slow baseline, keep runtime reasonable
    OrderBook book;
    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(N);

    uint64_t next_id = 1;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        Order o;
        o.id = next_id++;
        o.side = side_dist(rng) ? Side::Buy : Side::Sell;
        o.type = Type::Limit;
        o.price = 10000.0 + price_offset(rng);
        o.qty = static_cast<uint64_t>(qty_dist(rng));
        o.timestamp_ns = 0;

        const auto s = std::chrono::steady_clock::now();
        book.add(o);
        const auto e = std::chrono::steady_clock::now();
        latencies_ns.push_back(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(e - s).count()));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    print_percentiles("v1(add)", latencies_ns, N, wall);
    std::printf("final depth=%zu\n", book.depth());
    return 0;
}
