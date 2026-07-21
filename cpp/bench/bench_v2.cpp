#include "order_book_v2.h"
#include "histogram.h"
#include <chrono>
#include <random>
#include <vector>

// Same workload shape as bench_v1.cpp — mixed add() around a moving mid,
// meaningful cross rate — so the two numbers are actually comparable.
int main() {
    constexpr int N = 2000000;  // v2 should be fast enough to justify 4x the volume
    OrderBookV2 book;
    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(N);

    uint64_t next_id = 1;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        OrderRequest o;
        o.id = next_id++;
        o.side = side_dist(rng) ? Side::Buy : Side::Sell;
        o.type = Type::Limit;
        o.price = 10000 + price_offset(rng);
        o.qty = static_cast<Quantity>(qty_dist(rng));

        const auto s = std::chrono::steady_clock::now();
        book.add(o);
        const auto e = std::chrono::steady_clock::now();
        latencies_ns.push_back(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(e - s).count()));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    print_percentiles("v2(add)", latencies_ns, N, wall);
    std::printf("final depth=%zu\n", book.depth());
    std::printf("levels_ growth/rebase events: %llu\n", (unsigned long long)book.level_array_growths());
    return 0;
}
