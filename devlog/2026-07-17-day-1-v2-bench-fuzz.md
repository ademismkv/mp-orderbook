## 2026-07-17 — v2 order book, benchmark harness, differential fuzzing

Built `OrderBookV2`: integer-tick prices (not `double` — avoids float-compare bugs), a shared price-axis array of levels, an intrusive doubly-linked list per level, and an arena allocator so no `malloc`/`new` happens once sized at construction. Added a dependency-free latency/throughput benchmark and a differential fuzz harness: a deterministic seeded operation generator replays the identical sequence through v1 and v2 independently, diffing a checksum of the trade stream and final book state.

First run: 1,551,000 simulated operations across 31 seed/count combinations, byte-identical between v1 and v2 on every run — real evidence the rewrite preserves v1's already-tested semantics, not just a claim that it should.

Benchmark: v2 ~40% lower p50, ~20% higher throughput than v1 (measured in a shared, noisy sandbox — the relative comparison is trustworthy, absolute numbers weren't yet resume-ready at this point).
