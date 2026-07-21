# Day 10 — real per-phase breakdown, real levels_ growth count

**Asked:** a real optimization punch list — replace `unordered_map`, eliminate allocations, measure hardware counters, pin threads, check whether `levels_` can resize, break down where one order's time actually goes, reduce cache misses, eliminate branches, inline aggressively, try `-O3`/`-march=native`. Prioritized into what's verifiable in this sandbox now vs. what needs the user's real machine.

**`levels_` growth — turned a comment into a number.** Added `OrderBookV2::level_array_growths()`, a counter incremented on both branches of `ensure_index_for_price()` (front-rebase and back-resize). Measured:
- 2,000,000-op synthetic benchmark (`bench_v2.cpp`, prices confined to ±50 ticks): **0 growth events**.
- Full real 400,391-event AAPL trading day (`replay_lobster.cpp`, window pre-sized to 150,000 ticks to cover the day's real $577.35–$588.32 range): **1 growth event**.

ADR-2 already called this "rare in practice" — now it's measured, not asserted. Documented in ADR.md with the real numbers.

**Per-phase breakdown of `add()` — a real answer, not a guess.** Added opt-in, zero-cost-when-undefined instrumentation to `OrderBookV2` (`#ifdef OBV2_PROFILE_BREAKDOWN`), timing five phases with `std::chrono`: `match()`, `ensure_index_for_price()`, arena allocation, FIFO linking, and the `index_` (`unordered_map`) insert. New `cpp/bench/bench_breakdown.cpp` runs the same 2M-op workload as `bench_v2.cpp` and reports the split. Two runs, both stable:

```
match()          ~27% of add() wall time   (every call — the matching loop itself)
index_insert     ~13-14%                    (the unordered_map insert — SECOND biggest phase)
fifo_link        ~7%
arena_alloc      ~6%
ensure_index()   ~6%
```

**This is real, independent evidence for the top-priority item on the list:** the `unordered_map` insert costs roughly double any of arena allocation, FIFO linking, or price-level lookup — all three of which are arena-backed, pointer/index-based, and cheap. `index_` is the one heap-allocated, hash-based structure in the hot path, and the numbers say so directly, not just architecturally. This justifies spending real effort on replacing it rather than treating it as a hunch.

**Honest caveat on both new tools:** `bench_breakdown.cpp`'s own instrumentation (five `std::chrono::steady_clock::now()` calls per `add()` instead of the usual two) measurably slows the instrumented binary down relative to `bench_v2.cpp` — only ~59% of wall time is accounted for by the five phases, the rest is loop/RNG overhead plus the chrono calls' own cost. That's expected, not a bug — the phases' *relative* split is the useful signal here, not this binary's absolute throughput (see `bench_v2.cpp`/`quickstart.sh` for that). Zero-cost claim on the default build was verified directly: recompiled and re-ran the full unit test suite and `quickstart.sh` without `-DOBV2_PROFILE_BREAKDOWN` defined — identical behavior, nothing about the numbers documented elsewhere in this repo changed.

**Compiler flags, quick real result:** `-O3` gave a real, repeatable ~10-15% throughput edge over `-O2` across multiple runs in this sandbox; `-march=native` added nothing further here. Switched the default build flag in `quickstart.sh`, `README.md`, and `.github/workflows/ci.yml` from `-O2` to `-O3` — a safe, portable, zero-correctness-risk change (pure codegen, already fuzz-verified against v1). Left `-march=native` out of any default build since it bakes in this specific machine's CPU features, which would break portability for anyone else cloning the repo. Tail latency (p99.9, max) was too noisy in this shared/virtualized sandbox to draw a conclusion from (max latency swung 154μs-1.28ms across otherwise-identical runs) — not chasing that further here; it needs real, unshared hardware, same as the multithreading numbers already flagged elsewhere.

**What's next, and where it belongs:** replacing `index_` with a flat/open-addressing map is real, correctness-sensitive work — doing it here, verified against the existing unit tests and the v1/v2 differential fuzz harness, not left for the user to discover bugs in blind. Hardware counters (`perf stat`: cycles, IPC, L1/LLC misses, branch misses) and real thread-pinning gains are a different kind of task — `perf` doesn't exist on the user's Mac, and even Docker Desktop's Linux VM doesn't reliably expose real PMU counters through virtualization, so that tooling has to be built with the user's actual platform in mind and handed off, not run here.
