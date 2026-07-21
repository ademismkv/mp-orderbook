## 2026-07-17 — 20-run statistical benchmark, core-pinned

Ran 20 core-pinned trials each for v1 and v2 instead of trusting a single measurement. Outlier-trimmed medians: v2 is 1.44x v1's throughput and 1.34x faster at p50 (125ns vs 167ns), p99 417ns vs 791.5ns.

The more interesting finding: v2 has much higher run-to-run variance than v1 (16% vs 5% stdev), including one 70.8ms single-op stall. A contiguous arena, touched sequentially, is more exposed to a single host-level scheduling interruption than a scattered `std::map` — a real structural property of the design, not noise to explain away, and a reason these numbers needed re-measuring on real hardware before going anywhere near a resume.
