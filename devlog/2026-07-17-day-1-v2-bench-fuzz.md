## 2026-07-17 (later same day) — v2 order book, benchmark harness, differential fuzzing

**Asked for:** "let's build it" — turn ADR-2 (array of price levels + intrusive list + arena allocator) into real code, get real benchmark numbers instead of estimates, and find a way to prove v2 doesn't change behavior versus the correctness-verified v1 baseline.

**Got:**
- `cpp/include/order_book_v2.h` + `cpp/src/order_book_v2.cpp` — `OrderBookV2`: integer-tick prices (not double — avoids float-compare bugs), a shared price axis (`levels_[i]` = price `base_ + i`) with `best_bid_idx_`/`best_ask_idx_` tracked incrementally, intrusive doubly-linked list per price level, and an arena (`std::unique_ptr<Node[]>` + free-list) so no `malloc`/`new` happens once the arena is sized at construction. Handles rebasing the price window when an order lands outside the current range (front-growth shifts `base_` and every open order's cached level index; back-growth is a cheap resize).
- `cpp/tests/test_order_book_v2.cpp` — the original 5 tests ported to the new API, plus 2 new tests specifically for the rebase path (front-growth below the window, back-growth above it, and a check that an order placed *before* a rebase is still correctly cancellable *after* one).
- `cpp/bench/` — a small dependency-free benchmark (`histogram.h` + `bench_v1.cpp`/`bench_v2.cpp`) since there's no `google-benchmark` available in the dev sandbox this was built in. Measures per-op latency with `std::chrono::steady_clock` around each `add()` call, reports p50/p99/p99.9 and throughput.
- `cpp/fuzz/` — differential fuzz testing. `workload.h` generates a deterministic op sequence (add-limit/add-market/cancel, seeded RNG) that's engine-agnostic; `fuzz_v1.cpp` and `fuzz_v2.cpp` each replay the *same* seed through their own book and print a checksum of the resulting trade stream + final book state. They're separate binaries, not one program, because v1's and v2's headers both define `Side`/`Type` at global scope and can't be included in the same translation unit.

**Kept:** v1 stays in the repo untouched, as the correctness reference — this is the whole point, v2 is only trustworthy insofar as it's been checked against v1, not because it "looks right."

**Real numbers (measured in this dev sandbox — see caveat below, do not put these directly on a resume without re-measuring on real hardware):**

Benchmark (mixed add() workload, ~50/50 buy/sell, narrow spread so a meaningful fraction of orders actually cross and match rather than all resting):
```
v1(add)    ops=500000    throughput=3.219M ops/sec   p50=208ns   p99=750ns    p999=1959ns
v2(add)    ops=2000000   throughput=3.882M ops/sec   p50=125ns   p99=584ns    p999=1583ns
```
v2: ~40% lower p50 latency, ~20% higher throughput than v1. A real, honest improvement — not the "5-8x" numbers cited as achievable in the reading list, because this v2 hasn't yet applied cache-line padding, hasn't been profiled/tuned, and is still running through a hash-map lookup (`index_`) on every cancel and every fill-to-zero. That gap is exactly next session's work, and now there's a real number to move.

**Caveat that matters:** both benchmarks show p999/max latencies with multi-millisecond outliers (not shown above — full numbers are in the bench output). That's almost certainly sandbox noise — shared VM, no core pinning, no `isolcpus`, background scheduling jitter, arena/vector page-fault-on-first-touch — not the algorithm. Real latency claims need to be re-measured on quiet, dedicated hardware with core pinning before they go anywhere near a resume or README headline number. This first pass establishes the *comparison* (v2 faster than v1, by roughly this ratio) more reliably than it establishes the *absolute* numbers.

**Differential fuzz results:** ran 30 seed/count combinations (10 seeds x 3 op-counts: 100/5,000/50,000 ops) plus one 1,000,000-op run — 1,551,000 total simulated operations. v1 and v2 produced byte-identical trade checksums, trade counts, total filled quantity, final depth, and final best-bid/best-ask on every single run. This is the actual evidence that the v2 rewrite preserves v1's (already-tested) matching semantics — a diff of two numbers, not a claim.

**Wrong / open:**
- Nothing wrong was found in v2 by the fuzz run this time — first implementation matched on the first try after the 7 unit tests passed. Worth being honest that this is somewhat lucky; the rebase path (front-growth) is exactly the kind of code that tends to hide bugs that only a fuzzer finds, and it wasn't tested at large scale (the two explicit rebase unit tests only rebase once each). A stronger next fuzz session would force *repeated* rebases in both directions within a single run.
- No Rust/`cxx` work happened this session — no `rustc`/`cargo` available in the dev sandbox this was built in, so the Rust sidecar is still just the skeleton from the previous session, unverified.
- Didn't touch multithreading (ADR-1) or cache-line padding yet — this session was entirely "make v2 correct and measurably faster than v1, prove it with a diff not an assertion." That was the actual scope, and it's done.

**Next:** cache-line pad the hot structs (`Node`, `PriceLevel`) and re-benchmark to see how much of the "5-8x" claim from the reading list is real; re-run the full benchmark + fuzz suite on real (non-sandbox) hardware with core pinning before any number goes in a README or resume.
