## 2026-07-17 (later still) — ADR-1 implemented: SPSC ring buffer, threaded pipeline, verified under TSan/ASan

**Asked for:** "what should I do for you to do rust skeleton and multithreaded thing" — i.e. what's blocking each one. Answer turned out to be different for each: multithreading needed nothing from the user (standard C++, built it directly), Rust needs the user to do something (see below), so did the C++ threading work immediately rather than waiting.

**Got:**
- `cpp/include/spsc_ring.h` — `SpscRingBuffer<T>`: lock-free single-producer/single-consumer ring, `head_`/`tail_` on separate cache lines (`alignas(64)`). This is the concrete case where cache-line padding actually matters — unlike the single-threaded `Node` experiment in devlog day 2 that showed no benefit, `head_` and `tail_` here are genuinely written by two different threads, so false sharing is a real risk, not a hypothetical one.
- `cpp/tests/test_threaded.cpp` — builds the real pipeline (producer thread → `SpscRingBuffer<OrderRequest>` → matching thread running its own `OrderBookV2` → `SpscRingBuffer<Trade>` → this thread as consumer) and diffs its output against calling `OrderBookV2::add()` directly and sequentially on the identical op sequence. This is the actual test of ADR-1's claim: does routing everything through queues instead of calling the book directly change behavior. It shouldn't, because the SPSC ring preserves FIFO order and there's still exactly one writer per book.
- `cpp/bench/bench_threaded_scaling.cpp` — spawns 1..N independent symbol pipelines (each own thread, own `OrderBookV2`, zero shared state) and measures aggregate throughput at each N, to test the actual thesis of symbol-sharding rather than assert it.

**Verification — this is the part that matters more than the numbers:**
- Matching output (trades, quantities, checksum) between the threaded pipeline and the sequential reference: identical across 6 tested seeds.
- **Ran under ThreadSanitizer** (`-fsanitize=thread`) — 5 seeds, zero race warnings. TSan checks the actual memory model (unsynchronized concurrent access to the same memory), not just whether the final numbers happen to agree — two racy accesses can still coincidentally produce a correct-looking result, which is exactly why "the numbers matched" alone isn't proof for concurrent code the way it is for `fuzz_v2`.
- Also ran clean under AddressSanitizer + UndefinedBehaviorSanitizer.
- Confirmed `perf_event_paranoid` still blocks real profiling in this sandbox, so no flamegraph — but TSan/ASan don't need those permissions and were available, which is why they were reached for here instead.

**Scaling numbers (5 runs, `taskset -c 0-3` — sandbox has 4 cores, `std::thread::hardware_concurrency()` confirms this):**

| Symbols (= threads) | Aggregate throughput (median of 4 runs, discarding 1 cold-start run) | Scaling efficiency vs. 1-symbol baseline |
|---|---|---|
| 1 | 5.39M ops/sec | 100% (baseline) |
| 2 | 11.36M ops/sec | 105% |
| 3 | 17.56M ops/sec | 109% |
| 4 | 21.27M ops/sec | 99% |

Scaling is close to linear up to the sandbox's 4 physical cores — this is real evidence for ADR-1's actual thesis (independent per-symbol books need no synchronization, so throughput should scale with core count), not just an assertion that it should work. Efficiency numbers slightly above 100% at 2-3 symbols are almost certainly measurement noise (small wall-clock times, same sandbox-variance caveat as every other benchmark in this repo), not a real superlinear effect — noted rather than quietly rounded to a cleaner-looking number.

**What this benchmark does NOT yet show:** real order flow arriving over a network, ring-buffer contention under realistic burst patterns, or behavior with more symbols than cores (oversubscription). It shows the core architectural bet — independent single-writer books scale — holds on this hardware, for this workload shape.

**On the Rust question — why threading happened immediately and Rust didn't:** tried two ways to get Rust working in this dev sandbox: `apt-get install rustc cargo` (failed, no root — `dpkg` lock requires privileges this environment doesn't grant) and `rustup.rs` install script (failed, `curl` to `sh.rustup.rs` returned 403 — the sandbox's network allowlist doesn't include it). Both dead ends confirmed rather than assumed. The C++ threading work needed nothing beyond what was already available (g++, pthread, TSan/ASan all present), so it happened first. Rust genuinely needs the user's own machine — see the note in README / the chat response for the exact unblock steps.

**Wrong / open:**
- TSan runs were capped at ~5,000 ops per seed (TSan's instrumentation overhead is large enough that a 500K-op run timed out) — good enough to catch a race if one exists structurally, but a longer-running TSan soak test on real hardware would be more thorough.
- Ring buffer capacity (`1u << 16` = 65,536 slots) was picked without a specific justification — worth revisiting once there's a real workload to size it against (too small risks producer stalls under burst load, too large wastes cache/memory for no benefit, same category of judgment call as the price-window sizing in ADR-2).
- Haven't wired the threaded engine into a persistent multi-symbol demo yet — `bench_threaded_scaling.cpp` proves the scaling claim but is a synthetic benchmark, not the actual engine entry point.
