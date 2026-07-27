// Same test as bench_threaded_scaling.cpp — N independent symbols, each an
// isolated OrderBookV2 with zero shared state, one thread per symbol — but
// this version pins each thread to a specific core before running, to see
// whether the non-monotonic scaling seen on real runs (4-symbol aggregate
// dropping below 3-symbol, reported repeatedly on an 8-core Mac) is OS
// scheduler noise or something else. Since there is genuinely zero shared
// state between threads in this benchmark, a scaling drop cannot be a
// synchronization/contention bug in the code — pinning is here to test
// whether it's scheduler placement instead (e.g. a thread landing on a
// slower efficiency core, or migrating mid-run).
//
// Honest platform difference, not glossed over:
//   Linux:  pthread_setaffinity_np is a hard binding — the kernel will not
//           schedule that thread anywhere else. If pinning changes the
//           result on Linux, that's a real, causal answer.
//   macOS:  thread_policy_set(THREAD_AFFINITY_POLICY) is documented by
//           Apple as an *advisory* hint for grouping related threads, not a
//           hard binding, and on Apple Silicon's heterogeneous P-core/
//           E-core layout the OS scheduler still makes the real placement
//           decisions — this call does not reliably control which physical
//           core a thread lands on. Run this on your Mac and it will
//           compile and run, but treat a "no difference" result as
//           inconclusive, not as proof pinning doesn't matter — macOS just
//           doesn't expose the control needed to test that claim the way
//           Linux does. If you want a real answer to "does pinning fix the
//           4-symbol dip," that needs a Linux box (a cloud VM, or Docker
//           with a Linux kernel underneath is closer but still not a
//           guarantee — Docker Desktop's VM has its own scheduler).

#include "order_book_v2.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#if defined(__linux__)
    #define PIN_LINUX 1
    #include <pthread.h>
#elif defined(__APPLE__)
    #define PIN_MACOS 1
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
    #include <pthread.h>
#endif

static bool pin_current_thread_to_core(int core_id) {
#if defined(PIN_LINUX)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(PIN_MACOS)
    // Advisory only — see the file-level comment. thread_affinity_policy_data_t's
    // affinity_tag groups threads that share a tag; it does not name a specific
    // physical core the way Linux's cpu_set_t does. Using core_id as the tag is
    // the closest available approximation, not an equivalent guarantee.
    thread_affinity_policy_data_t policy = { core_id };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    (void)core_id;
    return false;  // unknown platform — ran without pinning, not silently claiming success
#endif
}

struct SymbolStats {
    uint64_t ops_done = 0;
    bool pin_ok = false;
};

void run_symbol(int symbol_id, int n_ops, int core_id, SymbolStats* stats) {
    stats->pin_ok = pin_current_thread_to_core(core_id);

    OrderBookV2 book;  // one book per thread — never touched by any other thread
    std::mt19937_64 rng(1000 + static_cast<uint64_t>(symbol_id));
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    uint64_t next_id = 1;
    std::vector<Trade> trades;  // reused across add() calls — see order_book_v2.h
    for (int i = 0; i < n_ops; ++i) {
        OrderRequest o;
        o.id = next_id++;
        o.side = side_dist(rng) ? Side::Buy : Side::Sell;
        o.type = Type::Limit;
        o.price = 10000 + price_offset(rng);
        o.qty = static_cast<Quantity>(qty_dist(rng));
        book.add(o, trades);
    }
    stats->ops_done = static_cast<uint64_t>(n_ops);
}

int main(int argc, char** argv) {
    const int max_symbols = argc > 1 ? std::atoi(argv[1]) : 4;
    const int ops_per_symbol = argc > 2 ? std::atoi(argv[2]) : 1000000;
    const unsigned hw = std::thread::hardware_concurrency();

#if defined(PIN_LINUX)
    std::printf("platform=linux pinning=hard(pthread_setaffinity_np) hardware_concurrency=%u\n", hw);
#elif defined(PIN_MACOS)
    std::printf("platform=macos pinning=advisory(thread_policy_set, NOT a hard guarantee) hardware_concurrency=%u\n", hw);
#else
    std::printf("platform=unknown pinning=unsupported hardware_concurrency=%u\n", hw);
#endif

    for (int n = 1; n <= max_symbols; ++n) {
        std::vector<SymbolStats> stats(static_cast<size_t>(n));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(n));

        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < n; ++s) {
            const int core_id = hw > 0 ? (s % static_cast<int>(hw)) : s;
            threads.emplace_back(run_symbol, s, ops_per_symbol, core_id, &stats[static_cast<size_t>(s)]);
        }
        for (auto& th : threads) th.join();
        const auto t1 = std::chrono::steady_clock::now();

        const double wall = std::chrono::duration<double>(t1 - t0).count();
        const uint64_t total_ops = static_cast<uint64_t>(n) * static_cast<uint64_t>(ops_per_symbol);
        const double agg_m = (static_cast<double>(total_ops) / wall) / 1e6;

        int pinned_count = 0;
        for (auto& st : stats) pinned_count += st.pin_ok ? 1 : 0;

        std::printf("symbols=%d total_ops=%llu wall=%.3fs aggregate=%.3fM ops/sec per_symbol=%.3fM ops/sec pin_calls_succeeded=%d/%d\n",
            n, (unsigned long long)total_ops, wall, agg_m, agg_m / n, pinned_count, n);
    }
    return 0;
}
