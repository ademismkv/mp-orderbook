#include "order_book_v2.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

// Tests ADR-1's actual claim empirically: throughput should scale roughly
// linearly with symbol count, since each symbol is an independent
// single-writer book with its own thread and no shared state with the
// others. Runs N=1..max_symbols concurrently and reports aggregate
// throughput at each N. Sandbox has 4 cores (see devlog) so scaling past 4
// symbols is expected to flatten — that's the point of measuring it rather
// than asserting "it scales."

struct SymbolStats {
    uint64_t ops_done = 0;
};

void run_symbol(int symbol_id, int n_ops, SymbolStats* stats) {
    OrderBookV2 book;  // one book per thread — never touched by any other thread
    std::mt19937_64 rng(1000 + static_cast<uint64_t>(symbol_id));
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    uint64_t next_id = 1;
    for (int i = 0; i < n_ops; ++i) {
        OrderRequest o;
        o.id = next_id++;
        o.side = side_dist(rng) ? Side::Buy : Side::Sell;
        o.type = Type::Limit;
        o.price = 10000 + price_offset(rng);
        o.qty = static_cast<Quantity>(qty_dist(rng));
        book.add(o);
    }
    stats->ops_done = static_cast<uint64_t>(n_ops);
}

int main(int argc, char** argv) {
    const int max_symbols = argc > 1 ? std::atoi(argv[1]) : 4;
    const int ops_per_symbol = argc > 2 ? std::atoi(argv[2]) : 1000000;

    std::printf("hardware_concurrency=%u\n", std::thread::hardware_concurrency());

    for (int n = 1; n <= max_symbols; ++n) {
        std::vector<SymbolStats> stats(static_cast<size_t>(n));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(n));

        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < n; ++s) {
            threads.emplace_back(run_symbol, s, ops_per_symbol, &stats[static_cast<size_t>(s)]);
        }
        for (auto& th : threads) th.join();
        const auto t1 = std::chrono::steady_clock::now();

        const double wall = std::chrono::duration<double>(t1 - t0).count();
        const uint64_t total_ops = static_cast<uint64_t>(n) * static_cast<uint64_t>(ops_per_symbol);
        const double agg_m = (static_cast<double>(total_ops) / wall) / 1e6;
        std::printf("symbols=%d total_ops=%llu wall=%.3fs aggregate=%.3fM ops/sec per_symbol=%.3fM ops/sec\n",
            n, (unsigned long long)total_ops, wall, agg_m, agg_m / n);
    }
    return 0;
}
