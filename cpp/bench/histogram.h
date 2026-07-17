#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

// Minimal dependency-free percentile reporter. No google-benchmark
// available in the dev sandbox this was first built in — this is meant to
// be swapped for google-benchmark/HdrHistogram later (see plan), but gives
// real, honest numbers today with zero external dependencies.
inline void print_percentiles(const char* label, std::vector<uint64_t>& ns,
                               uint64_t total_ops, double wall_seconds) {
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * (ns.size() - 1));
        return ns[idx];
    };
    double ops_per_sec = total_ops / wall_seconds;
    std::printf(
        "%-10s ops=%-8llu wall=%.3fs  throughput=%8.3fM ops/sec  "
        "p50=%6lluns  p99=%7lluns  p999=%8lluns  max=%8lluns\n",
        label, (unsigned long long)total_ops, wall_seconds, ops_per_sec / 1e6,
        (unsigned long long)pct(0.50), (unsigned long long)pct(0.99),
        (unsigned long long)pct(0.999), (unsigned long long)ns.back());
}
