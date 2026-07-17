## 2026-07-17 (later still) — measure before optimizing, CI, LICENSE, recruiter-facing README

**Asked for:** "take it further and make it ready" — push the engineering forward and get the repo into a genuinely shippable/public state.

**Wanted to try `perf` first, couldn't:** the dev sandbox has `perf` installed but `perf_event_paranoid=4` blocks it (`Access to performance monitoring... limited`, no CAP_PERFMON). No hardware counters, no flamegraph, in this environment. Falling back to macro-benchmarking (wall-clock before/after a specific, reasoned change) instead of guessing at what to optimize.

**Found and fixed a real bottleneck — the `index_` hash map wasn't reserved.** `std::unordered_map<OrderId, IndexEntry> index_` was left to grow organically, which means repeated rehashes as it crosses growth thresholds while scaling to hundreds of thousands of open orders. Added `index_.reserve(arena_capacity)` in the constructor. Measured effect (3 runs each, before/after, same 2M-op benchmark):

```
before: throughput ~3.87-3.95M ops/sec   p99 ~625-708ns   max tail 18.8-20.1ms
after:  throughput ~4.49-4.73M ops/sec   p99 ~417-458ns   max tail 0.1-1.8ms
```

~18-20% throughput gain, ~35-40% p99 improvement, and — the more interesting number — the worst-case tail dropped by roughly 10-100x. That's the signature of a rehash storm: a small number of ops hit a full rehash (allocate + rehash every existing entry) and pay for it disproportionately. One-line fix, verified against both the 7 unit tests and a fresh differential fuzz run (200,000 ops, still byte-identical to v1) before keeping it.

**Tried cache-line padding on `Node`, measured it, and it didn't help — kept it out.** The plan (and the reading list) both call out cache-line padding as a standard optimization, so tried `alignas(64)` on the intrusive-list `Node` struct as an experiment (isolated build, not merged into the real source until the result was known). Result: throughput and p99 were statistically indistinguishable from the `index_.reserve()`-only version (~4.73-4.76M ops/sec vs ~4.49-4.73M — overlapping ranges, no clear signal). This is the expected outcome, not a surprise in hindsight: cache-line padding exists to prevent *false sharing* — two threads on different cores fighting over the same cache line. There's no concurrency yet (ADR-1's threading model isn't implemented), so there's nothing to false-share against. Padding now would just bloat the arena's memory footprint for zero benefit. **Decision: don't pad yet. Revisit when ADR-1 lands and there's actually a second thread touching adjacent memory.** This is worth keeping in the devlog specifically because "I tried the standard optimization, measured no benefit, understood why, and didn't ship it anyway" is a better signal than blindly applying every technique on a checklist.

**Added for "ready":**
- `LICENSE` (MIT) — repo wasn't legally clear to actually be public before this.
- `.github/workflows/ci.yml` — builds v1 and v2 with both the direct g++ commands and the CMakeLists (so both build paths are actually checked, not just documented), runs all unit tests, runs the differential fuzz suite as a **correctness gate** (fails the build on any v1/v2 mismatch), and runs the benchmark as an informational (non-gating) step — CI runners are shared/noisy, so a benchmark regression gate there would be exactly the same measurement-reliability problem flagged in the previous devlog entry.
- README rewritten to be self-contained (see next entry) — it previously pointed at the private notes vault for the architecture explanation and benchmark numbers, which doesn't work once this is a public repo a recruiter is looking at in isolation.

**Wrong / open:**
- Still no real profiler in this environment — the `index_.reserve()` fix was found by reasoning about the data structure, not by measuring where cycles actually go. It happened to be right (confirmed by the before/after numbers), but "reason about what's probably slow, then verify" is a weaker method than "measure what's actually slow, then fix it," and got lucky here. Running with `perf record` + a flamegraph on real hardware is still a real to-do, not a nice-to-have.
- CI workflow is written but has never actually run (no GitHub Actions runner available in this environment) — needs to be verified for real the first time it's pushed.
