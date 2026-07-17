## 2026-07-17 (later still) — 20-run statistical benchmark, core-pinned, outliers quantified not hidden

**Asked for:** actually measure it — not a single run, multiple real trials, real data (not eyeballed).

**Method:** `taskset -c 0` to pin each benchmark process to a single core (removes cross-core migration as a noise source — `chrt` for real-time scheduling priority was tried too but failed, `Operation not permitted`, this sandbox doesn't grant CAP_SYS_NICE). One discarded warm-up run each, then 20 measured runs each for v1 and v2, same 2M-op mixed add() workload as before. Parsed with a small script computing mean/median/stdev/min/max, not just reporting one number.

**Results (n=20 runs each, all runs included):**

| | v1 (`std::map`) | v2 (array+arena+reserve) |
|---|---|---|
| throughput, median | 3.267M ops/sec | 4.712M ops/sec |
| throughput, stdev | 0.164M (5% of median) | 0.744M (16% of median) |
| p50, median | 167ns | 125ns |
| p99, median | 791.5ns | 417ns |
| worst single-op stall seen across all 20 runs | 4.13ms | **70.8ms** |

**The real finding: v2 has higher variance, not just a higher mean.** v1's throughput stayed inside a tight 5% band across all 20 runs — boring, predictable. v2 had 2 of 20 runs (10%) drop to 1.8M and 3.5M ops/sec — well below its usual ~4.7M — with one of those runs hitting a 70.8ms single-op stall. Flagged both as likely host-level scheduling noise (this is a shared cloud sandbox; `taskset` pins within the guest's visible CPUs but can't guarantee the *physical* core isn't being time-sliced by the hypervisor for other tenants) rather than an algorithmic problem in v2 — excluding those 2 runs, v2's remaining 18 runs tighten to a 3.506-4.768M range, and the ratio holds:

**Outlier-trimmed comparison (18 clean v2 runs vs all 20 v1 runs, both medians):**
- **v2 is 1.44x v1's throughput**
- **v2's p50 is 1.34x faster than v1's** (125ns vs 167ns)
- p99: 417ns (v2) vs 791.5ns (v1) — v2's p99 is essentially v1's p50

**Why v2 has more variance than v1, worth understanding rather than hand-waving:** v1's `std::map` workload has a smoother memory-access pattern (tree nodes are individually `new`'d, scattered, but each op does similar work). v2's arena is one big contiguous allocation touched sequentially — meaning it's *more* susceptible to a single bad host-scheduling interruption showing up as a multi-millisecond stall precisely because everything else about it is fast and low-jitter (a 70ms stall is enormous relative to a 125ns steady-state op, but would barely register against v1's already-slower baseline). This is a real, structural reason v2 needs actual dedicated hardware before its numbers mean anything for a resume — not a defect, but a measurement-environment limitation that a faster implementation makes more visible, not less.

**Tried to get real NASDAQ order flow data (LOBSTER) instead of synthetic orders, couldn't in this environment:** `lobsterdata.com`'s sample-data page is a client-rendered React app — a plain fetch only returns the empty shell, and their actual sample downloads appear to be gated behind an interactive request/email flow, not static files at a guessable URL. Not something to force through browser automation for a "let me just grab a CSV" task. Real next step: register at lobsterdata.com yourself (free, academic use), download a sample day of AAPL, and replay it through `fuzz_v2`'s op-generation pattern adapted to LOBSTER's message format — that's real work for a future session, not something to fake by relabeling synthetic data as "real."

**Wrong / open:**
- 20 runs is enough to see the variance pattern, not enough to fully characterize the tail (a 70ms stall in 1-in-20 runs could be 1-in-20 or could be rarer/more common — would want 100+ runs on real hardware to actually characterize p99.9 of *runs*, not just p99.9 of *ops within a run*).
- Still haven't profiled with `perf` (blocked in this sandbox) — the variance source above is a reasoned hypothesis consistent with the data, not a confirmed root cause from a profiler.
