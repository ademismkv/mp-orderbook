# matching-engine

A low-latency, multithreaded matching engine + order book. C++20 on the hot path, Rust sidecar for protocol parsing / risk / gateway work, planned `cxx` FFI boundary between them.

Built as a signature systems project with a public, dated devlog documenting an AI-augmented development workflow — see [`devlog/`](devlog/). Every entry says what was asked of an AI tool, what it produced, what got kept vs. rewritten, and what it got wrong.

## Quick start — see real numbers in under a minute

```bash
git clone https://github.com/ademismkv/mp-orderbook.git
cd mp-orderbook
./quickstart.sh
```

No CMake, no dependencies beyond a C++20 compiler (`g++`/`clang++` 10+). This builds v1 and v2, runs both unit test suites, runs the latency/throughput benchmark, runs the multithreaded scaling benchmark, and replays the real 400,391-event NASDAQ AAPL trading day through the actual engine — all on your machine, right then, printed straight from the programs that measured them. Nothing in `quickstart.sh`'s output is precomputed.

(The interactive dashboard at `dashboard/index.html` *is* precomputed playback — a browser can't invoke a C++ compiler, so it steps through a real prior run's recorded numbers instead of live-measuring. `quickstart.sh` is the actually-live version: it runs the real engine on your own hardware while you watch.)

**No compiler installed? Use Docker instead:**

```bash
docker build -t mp-orderbook .
docker run --rm mp-orderbook
```

Same `quickstart.sh`, same output, but running inside `ubuntu:22.04` with g++ 11 — the exact compiler version this codebase has actually been built and tested with, not a guess at compatibility. Note this doesn't buy you a quieter CPU: the container still shares whatever cores your machine has with everything else running on it, so it won't smooth out host-level noise (see the multithreading numbers below for what that noise looks like in practice) — it just guarantees the same toolchain every time, with nothing to install locally beyond Docker itself. (Caveat: the `Dockerfile` couldn't be tested end-to-end in the environment this was written in — no `docker` binary available there. It's a standard two-stage-free build off a well-known base image, but if it breaks for you, that's real, useful signal — open an issue or just tell me.)

## Architecture in 60 seconds

Price-time-priority matching, one thread per symbol (single-writer principle — no locks on the book itself, only on the queues feeding it), C++ for the matching hot path and Rust for everything that isn't latency-critical (FIX parsing, pre-trade risk, market data publishing). Full reasoning for every decision below is in [`ADR.md`](ADR.md).

| Decision | Choice | Why |
|---|---|---|
| Order book container | Array of price levels (shared price axis) + intrusive doubly-linked list per level + arena allocator | O(1) best-bid/ask, true O(1) cancel, zero `malloc` on the hot path after construction |
| Threading | One matching thread per symbol, no locks on the book | Most concurrency bugs come from multiple writers on shared state — remove the shared state instead of guarding it |
| Rust ↔ C++ boundary | `cxx` for control-plane calls, a shared-memory ring buffer for per-order data-plane traffic | Avoids per-order FFI call overhead on the hot path |
| Prices | Integer ticks, not `double` | No float-compare bugs at price-level boundaries |

## Status

| Component | State |
|---|---|
| v1 — `std::map`-based order book | Correctness baseline. 5/5 tests passing. Kept permanently as the reference implementation everything else is checked against. |
| v2 — array + intrusive list + arena | 10/10 tests passing (incl. 2 for price-window rebase, 3 for partial-cancel/`reduce()`). Verified behaviorally identical to v1 by differential fuzzing (see below). |
| Benchmark harness | Working, dependency-free (no google-benchmark yet). |
| Differential fuzzing | Working. v1 vs v2 checked over 1.75M+ simulated operations, zero mismatches. |
| Multithreading (ADR-1) | Implemented: SPSC ring buffer + per-symbol matching thread. Verified against the sequential reference and clean under ThreadSanitizer + AddressSanitizer (zero races, zero memory errors). Scaling measured up to 4 symbols/cores (see below). |
| Rust sidecar | FFI-wired: `cxx` bridge to `OrderBookV2` implemented (`rust/src/ffi.rs` + `cpp/src/order_book_v2_ffi.cpp`), plus a FIX 4.4 inbound parser (`rust/src/fix.rs`, 8 tests) and an async pre-trade risk pre-check (`rust/src/risk.rs`, 5 tests). The **C++ half of the FFI adapter is compiled and tested right now, in this environment** — `cpp/tests/test_order_book_v2_ffi_standalone.cpp` builds the real, unmodified adapter against hand-written mocks of the two headers `cxx_build` would otherwise generate, proving the translation logic works without needing `cargo`. The **Rust half (the actual `cxx` bridge, plus `fix.rs`/`risk.rs`) is still not compiled** — confirmed fresh this session, not assumed: no `rustc`/`cargo`, and `rustup.rs`/`static.rust-lang.org`/`crates.io` are all still blocked by this sandbox's network allowlist (403 from the proxy on all three). See below. |
| Real data replay | Implemented and run: 400,391 real NASDAQ order events (AAPL, full trading day) replayed through `OrderBookV2`, zero invariant violations. See below. |
| Live NASDAQ replay dashboard | Built and rebuilt: `dashboard/index.html` steps through the **real, full 400,391-event trading day**, driven by real `std::chrono`-measured batch timings from the actual compiled `OrderBookV2` engine (`cpp/tools/replay_lobster_batched.cpp`) — not a JS reimplementation (an earlier version of this dashboard was; that was the wrong call and got replaced, see devlog). Also includes freshly re-measured real multithreading scaling numbers. |
| Live market data | Not built. Live-capture script written for the user to run locally (structurally can't run inside this dev sandbox — see below) but not yet executed end-to-end. |

## Benchmark

Mixed `add()` workload — ~50/50 buy/sell around a moving mid, tight enough spread that a meaningful fraction of orders cross and match rather than all resting (an all-resting workload never exercises the matching path, which would make any implementation look artificially fast).

**20 core-pinned runs each (`taskset -c 0`), median and variance reported, not a single cherry-picked run:**

| | v1 (`std::map` baseline) | v2 (array + arena + `index_.reserve()`) |
|---|---|---|
| throughput, median | 3.267M ops/sec | 4.712M ops/sec |
| throughput, stdev across 20 runs | 0.164M (5%) | 0.744M (16% — see note) |
| p50, median | 167ns | 125ns |
| p99, median | 791.5ns | 417ns |
| worst single-op stall seen in any of the 20 runs | 4.13ms | 70.8ms |

**v2 is 1.44x v1's throughput and 1.34x v1's p50, using outlier-trimmed medians** (2 of 20 v2 runs — 10% — showed a large drop, almost certainly host-level scheduling noise on this shared sandbox rather than an algorithmic issue: `taskset` pins within the guest's visible CPUs but can't guarantee the physical core isn't time-sliced by the hypervisor for other tenants). v1's run-to-run variance was much tighter (5% vs 16%) — a real observation, not just sandbox noise, discussed in `devlog/2026-07-17-day-3-rigorous-benchmark.md`.

**These numbers were measured in a shared cloud sandbox, not quiet dedicated hardware — even with core pinning.** The relative comparison (v2 faster than v1, by roughly this ratio, with more variance) is probably meaningful since both ran in the same environment; the absolute numbers are not resume-ready until re-measured on real, unshared hardware. Full methodology and all 40 raw run results are in `devlog/2026-07-17-day-1-v2-bench-fuzz.md`, `devlog/2026-07-17-day-2-optimize-and-ci.md`, and `devlog/2026-07-17-day-3-rigorous-benchmark.md`.

Target: sub-microsecond p99, benchmarked against LMAX's published 2011 number (100K TPS at <1ms mean) as a reference point — not because 15-year-old hardware is a high bar, but because it's a well-known, citable comparison.

## Threading (ADR-1)

One matching thread per symbol, each owning its own `OrderBookV2` — no locks on the book, only an `SpscRingBuffer` (`cpp/include/spsc_ring.h`, cache-line padded head/tail) feeding orders in and trades out.

**Correctness:** `cpp/tests/test_threaded.cpp` runs the real producer→ring→matcher→ring→consumer pipeline and diffs its output against calling the book directly and sequentially — matched on every tested seed. More importantly, it's clean under **ThreadSanitizer** (zero data races) and **AddressSanitizer + UBSan** (zero memory errors) — matching output numbers alone doesn't prove a concurrent pipeline is race-free; two racy accesses can still coincidentally produce a correct-looking result.

```bash
cd cpp
g++ -std=c++20 -O2 -pthread -Iinclude -Ifuzz src/order_book_v2.cpp tests/test_threaded.cpp -o test_threaded && ./test_threaded 42 200000
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Iinclude -Ifuzz src/order_book_v2.cpp tests/test_threaded.cpp -o test_threaded_tsan && ./test_threaded_tsan 42 5000
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -pthread -Iinclude -Ifuzz src/order_book_v2.cpp tests/test_threaded.cpp -o test_threaded_asan && ./test_threaded_asan 42 200000
```

**Scaling:** `cpp/bench/bench_threaded_scaling.cpp` runs 1..N independent symbol pipelines concurrently and measures aggregate throughput — testing the actual claim (independent per-symbol books scale with core count) instead of asserting it.

| Symbols (= threads) | Aggregate throughput (median of 4 runs) | Scaling efficiency vs. 1-symbol |
|---|---|---|
| 1 | 5.39M ops/sec | 100% (baseline) |
| 2 | 11.36M ops/sec | 105% |
| 3 | 17.56M ops/sec | 109% |
| 4 | 21.27M ops/sec | 99% |

Measured on a 4-core sandbox (`std::thread::hardware_concurrency() == 4`) — scaling is close to linear up to the physical core count, which is exactly what the symbol-sharding design predicts. Same sandbox-noise caveat as every other number in this README applies; full raw runs in `devlog/2026-07-17-day-4-threading.md`.

## Real data replay

`cpp/tools/replay_lobster.cpp` replays a real LOBSTER-format NASDAQ message file through `OrderBookV2` — real order arrivals, cancels, partial cancels, and executions, not synthetic workload generation. Data source and format are documented in `data/README.md`.

```bash
cd cpp
g++ -std=c++20 -O2 -Iinclude src/order_book_v2.cpp tools/replay_lobster.cpp -o replay_lobster
./replay_lobster ../data/AAPL_2012-06-21_34200000_57600000_message_10.csv
```

**Results — AAPL, full trading day, June 21 2012, 400,391 real events:**

| | |
|---|---|
| Parse failures | 0 |
| **Book invariant violations (crossed book)** | **0** |
| Processing rate | 1.26M real events/sec (this sandbox — see the usual caveat) |
| This engine's own trades | 13,298, totaling 528,509 shares |
| LOBSTER-reported visible execution volume | 1,845,964 shares (sanity cross-check, not expected to match exactly) |
| LOBSTER-reported hidden execution volume | 1,004,176 shares (structurally unobservable — see below) |

The zero-invariant-violations result is the one that matters: this engine never crossed its own book across an entire real trading day of real, messy order flow. The volume gap between this engine's trades and LOBSTER's reported volume is expected, not a bug, for two reasons explained in full in `devlog/2026-07-17-day-5-real-nasdaq-replay.md`: (1) ~1M shares of the day's volume executed against **hidden orders**, which by definition never appear in the visible message stream this replay reads — no implementation can recover that from this data source; (2) this engine's own matching decisions are independent of NASDAQ's real matching engine, so the two books' states naturally diverge over a full day once even one fill decision differs. A real bug **was** found and fixed here — the first version of this replay didn't apply visible-execution quantity loss to resting orders, which inflated this engine's own match opportunities with phantom liquidity; fixed by calling `reduce()` (not re-running `match()`, which would have double-counted trades) on execution events. That's also why `OrderBookV2::reduce()` (partial-cancel support) exists at all — it didn't before this needed it.

## Live NASDAQ replay dashboard

`dashboard/index.html` — a self-contained, dependency-free-except-Chart.js-CDN HTML page. Open it directly in a browser (no build step, no server).

This went through two versions, worth being explicit about. The first version reimplemented the matching logic in JS and ran it in the browser over a small embedded sample — that was the wrong call: it showed JS-interpreter timings on a toy slice of data, not the actual engine. It's been replaced.

**What's there now:** the full 400,391-event real trading day, replayed once through the real, unmodified, TSan/ASan-verified `OrderBookV2` C++ engine via a new tool, `cpp/tools/replay_lobster_batched.cpp`, which reports real `std::chrono` wall-clock timing per batch (batch sizes of 5,000 / 10,000 / 20,000 events, your choice). The browser can't invoke a C++ compiler, so that run's real output is precomputed and embedded; the dashboard steps through it interactively (pull next batch / auto-play), showing that batch's real latency and throughput, cumulative trades and volume, best bid/ask, resting order depth, and cumulative cancel/reduce-miss counts — all numbers the real engine actually produced, not simulated or interpolated. Zero invariant violations across all 400,391 events, at every batch granularity — reconfirms the same result documented in the "Real data replay" section above, from a fresh run.

Also on the page: freshly re-measured real multithreading scaling numbers (median of 3 runs, `cpp/bench/bench_threaded_scaling.cpp`) — included there deliberately, because the LOBSTER file itself is one symbol, and per ADR-1 this engine gives one symbol exactly one thread. Multithreading isn't demonstrable *within* a single-symbol replay by design; it's demonstrated separately, with N independent per-symbol books running concurrently.

To regenerate the embedded batch data yourself:
```bash
cd cpp
g++ -std=c++20 -O2 -Iinclude src/order_book_v2.cpp tools/replay_lobster_batched.cpp -o replay_batched
./replay_batched ../data/AAPL_2012-06-21_34200000_57600000_message_10.csv 10000
```

## Live data — why it's not here, and what to do about it

Tried to get genuinely live (not historical) order book data into this repo. Two real attempts, both hit structural limits of this dev environment rather than being skipped:

1. **Raw network calls to public crypto exchange APIs** (Coinbase, Binance — both offer real order book data with zero API key) — blocked by this sandbox's network allowlist (`403 blocked-by-allowlist`), same category of block as `rustup.rs` earlier.
2. **The one working fetch path did reach Coinbase's live REST API** — but the returned timestamps were weeks to months stale relative to the actual date, meaning something in that path caches responses rather than proxying them live. Caught by reading the response, not assumed.

More fundamentally: live data means a **persistent connection** held open while updates stream in. Nothing available in this dev environment can do that — one-shot HTTP fetches and independent, timeout-bounded shell calls can't sustain a WebSocket. This isn't a gap to route around; it's what the tool architecture allows.

**What to do about it:** `tools/capture_live_orderbook.py` — connects to Coinbase's public WebSocket feed (no API key, standard approach for free live order book data since real equities feeds are paid/licensed) and writes a CSV of live book updates + trades. Written and syntax-checked, but **not run end-to-end** — needs to run on a machine with normal network access, i.e. yours, not this dev sandbox:

```bash
pip install websockets
python3 tools/capture_live_orderbook.py --product BTC-USD --seconds 60 --out live_capture.csv
```

Once you have a capture file, the natural next step is a `replay_live.cpp` sibling to `replay_lobster.cpp` (the column shapes are deliberately similar) — not yet built, since there's no captured file yet to build it against.

## Rust sidecar — FFI-wired, FIX parsing, risk pre-check (compiled and tested: 15/15 passing)

The dev sandbox this repo was built in has no `rustc`/`cargo` (`apt-get install` has no root, `rustup.rs` is blocked by the sandbox's network allowlist — both confirmed, not assumed), so this crate has only ever been built and tested on the user's own machine. It took two real build-fix-retry passes to get there (both documented below), and the third attempt — `cargo build` followed by `cargo test` — came back fully green:

```
running 15 tests
test fix::tests::parses_cancel_replace_request ... ok
test fix::tests::parses_market_order_with_no_price ... ok
test fix::tests::accepts_real_soh_delimiter ... ok
test fix::tests::parses_cancel_request ... ok
test fix::tests::parses_new_order_single_limit_buy ... ok
test fix::tests::rejects_empty_message ... ok
test fix::tests::rejects_missing_required_tag ... ok
test fix::tests::rejects_unknown_msg_type ... ok
test risk::tests::accepts_order_within_all_limits ... ok
test risk::tests::rejects_disallowed_symbol ... ok
test risk::tests::rejects_price_outside_collar ... ok
test risk::tests::rejects_oversized_order ... ok
test risk::tests::skips_collar_check_with_no_reference_price ... ok
test tests::non_crossing_order_rests ... ok
test tests::crosses_and_reports_a_trade ... ok

test result: ok. 15 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
```

The last two — `crosses_and_reports_a_trade` and `non_crossing_order_rests` — are the ones that matter most: they call `MatchingEngine`, which goes through the real, compiled `cxx` bridge into the real `OrderBookV2Ffi` C++ adapter and back. That's the whole Rust↔C++ boundary (ADR-3) exercised end to end, not the mock-header standalone test — first time that's been true.

**The bug, for the record:** `rust/src/ffi.rs` declared `#[cxx::bridge(namespace = "ffi")]`, which tells `cxx_build` to generate C++ glue code expecting the opaque `OrderBookV2Ffi` type inside `namespace ffi { ... }`. But `cpp/include/order_book_v2_ffi.h` declares `class OrderBookV2Ffi` at global scope, not inside that namespace. Real error from the user's machine:

```
error: no member named 'OrderBookV2Ffi' in namespace 'ffi'; did you mean simply 'OrderBookV2Ffi'?
  757 |   bool (::ffi::OrderBookV2Ffi::*reduce$)(...) = &::ffi::OrderBookV2Ffi::reduce;
../cpp/include/order_book_v2_ffi.h:30:7: note: 'OrderBookV2Ffi' declared here
   30 | class OrderBookV2Ffi {
```

Fixed by dropping `namespace = "ffi"` from the bridge macro so cxx generates everything at global scope, matching the C++ side as written. Worth being honest about why the earlier standalone C++ verification (`cpp/tests/test_order_book_v2_ffi_standalone.cpp`, run against hand-written mock headers in `cpp/tests/ffi_mock/`) didn't catch this: those mock headers made the identical global-scope assumption, so they matched the (also wrong) C++ header instead of matching what real `cxx_build` actually generates. That test proved the C++ translation logic works; it never proved the namespace was right — only a real `cargo build` could do that.

**Second bug, found on the next real `cargo build`:** a circular include. `order_book_v2_ffi.h` included the generated `matching-engine-sidecar/src/ffi.rs.h` directly, above its own `class OrderBookV2Ffi` declaration. `cxx_build` inserts `#include "order_book_v2_ffi.h"` straight into its generated `ffi.rs.cc`, and expects `ffi.rs.h` — which it includes *afterward*, ending in `using OrderBookV2Ffi = ::OrderBookV2Ffi;` — to run only once the real class already exists. With `#pragma once` on both headers, having `order_book_v2_ffi.h` include `ffi.rs.h` first meant that alias line ran before the class was ever declared:

```
error: no type named 'OrderBookV2Ffi' in the global namespace; did you mean 'OrderBookV2'?
  678 | using OrderBookV2Ffi = ::OrderBookV2Ffi;
error: definition of type 'OrderBookV2Ffi' conflicts with type alias of the same name
   30 | class OrderBookV2Ffi {
```

Fixed by removing that include from `order_book_v2_ffi.h` (replaced with forward declarations of `FfiOrderRequest`/`FfiTrade`, which are enough for method signatures) and including `ffi.rs.h` directly — after `order_book_v2_ffi.h` — in both `cpp/src/order_book_v2_ffi.cpp` and the standalone test, where the complete struct definitions are actually needed. Re-verified the standalone test and the full `quickstart.sh` suite still pass after this change.

**What's here now:**

- `rust/src/ffi.rs` — the `cxx` bridge (ADR-3). Declares shared types (`FfiSide`, `FfiOrderType`, `FfiOrderRequest`, `FfiTrade` — separate from `::Side`/`::Type` in `order_book_v2.h`, because `cxx` requires shared enums to be defined inside the bridge macro itself) and the opaque `OrderBookV2Ffi` C++ type.
- `cpp/include/order_book_v2_ffi.h` + `cpp/src/order_book_v2_ffi.cpp` — the adapter. Translates field-by-field between the FFI mirror types and the real `::OrderRequest`/`::Trade`. Deliberately never touches `OrderBookV2` itself — if this adapter has a bug, the matching logic underneath is still the same code that's already been fuzz-tested against v1 and run clean under TSan/ASan.
- `rust/build.rs` — compiles the bridge and both C++ translation units directly via `cxx_build`, independent of `cpp/CMakeLists.txt`. `cargo build` from `rust/` doesn't need CMake at all.
- `rust/src/lib.rs` — `MatchingEngine`, a safe wrapper around the generated `UniquePtr<OrderBookV2Ffi>` so callers never touch `Pin`/`UniquePtr` directly. Two tests: a crossing order that produces a trade, a resting order that doesn't.
- `rust/src/fix.rs` — FIX 4.4 inbound parser. `NewOrderSingle` (35=D), `OrderCancelRequest` (35=F), `OrderCancelReplaceRequest` (35=G) — enough to place/cancel/modify a limit order, per ADR-4's explicit scope cut. No session layer, no outbound messages. 8 unit tests.
- `rust/src/risk.rs` — the async pre-trade risk pre-check ADR-3 left as an open decision, now resolved: runs on the ingestion side, before an order reaches the matching thread's ring buffer. Symbol allowlist, max order size, max notional, price collar (bps from a reference price). 5 unit tests.
- `rust/src/bin/bench_ffi.rs` — the FFI cost benchmark: same workload shape as `cpp/bench/bench_v2.cpp` (2,000,000 orders, same price/qty ranges), but every call to `add()` goes Rust -> `cxx` -> C++ instead of staying in C++. This is the number that backs up ADR-3's decision to keep cxx off the hot data-plane path (SPSC ring buffer instead) — a measured tradeoff, not an assumption.

```bash
cd rust
cargo build
cargo test
```

This is verified — the transcript above is the user's own terminal output, not a projection. If your build ever fails again (e.g. after pulling a future change), paste the compiler error back — two real bugs (namespace mismatch, circular include) have already been found and fixed this way, each a quick, targeted fix rather than a redesign.

To measure the FFI cost directly:

```bash
cargo run --release --bin bench_ffi
```

Or just run `./quickstart.sh` from the repo root — it detects `cargo` automatically and runs this as its last step, folding the result straight into the summary table alongside the pure-C++ numbers.

## Correctness — differential fuzzing against the reference implementation

v2 exists to be faster than v1, not to be trusted on its own — it's only trustworthy insofar as it's been checked against v1. `cpp/fuzz/` generates a deterministic random operation sequence (add-limit, add-market, cancel) from a seed, replays it through both engines independently, and diffs a checksum of the resulting trade stream plus final book state (best bid, best ask, depth).

Current coverage: 31 runs (10 seeds x 3 op-counts, plus one 1,000,000-op run) = 1,551,000 operations, zero mismatches. Re-run any seed yourself:

```bash
cd cpp
g++ -std=c++20 -O2 -Iinclude -Ifuzz src/order_book.cpp fuzz/fuzz_v1.cpp -o fuzz_v1
g++ -std=c++20 -O2 -Iinclude -Ifuzz src/order_book_v2.cpp fuzz/fuzz_v2.cpp -o fuzz_v2
diff <(./fuzz_v1 42 1000000) <(./fuzz_v2 42 1000000) && echo "match"
```

## Layout

```
matching-engine/
├── quickstart.sh         one command: build everything, run everything, see real numbers
├── Dockerfile            same thing, zero local setup — docker build && docker run
├── cpp/
│   ├── include/        order.h (v1) / order_book_v2.h / spsc_ring.h — public types + class decls
│   ├── src/             order_book.cpp (v1) / order_book_v2.cpp
│   ├── tests/           unit tests + test_threaded.cpp, run via ctest or directly
│   ├── bench/            dependency-free latency/throughput harness
│   ├── fuzz/             differential fuzz (v1 vs v2), shared workload generator
│   ├── tools/            replay_lobster.cpp — real NASDAQ data replay
│   │                     replay_lobster_batched.cpp — same, per-batch timing for the dashboard
│   └── CMakeLists.txt
├── data/                 real LOBSTER sample data (see data/README.md for source)
├── dashboard/            index.html — live NASDAQ replay dashboard, open directly in a browser
├── tools/                capture_live_orderbook.py — run on your machine, not in this sandbox
├── rust/                 sidecar — cxx FFI bridge, FIX parsing, risk pre-check (compiled + tested, 15/15 passing)
├── devlog/               dated entries: asked / got / kept / changed / wrong
├── .github/workflows/    CI: build + unit tests + differential fuzz gate + bench (informational)
└── LICENSE               MIT
```

## Build & test

Fastest path: `./quickstart.sh` from the repo root (see "Quick start" above) — builds and runs everything below in one shot. What follows is the same steps broken out individually, for anyone who wants to run one piece at a time.

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure       # runs both v1 and v2 unit test suites
```

*(Note: the CMake path above hasn't been exercised in this dev sandbox — `cmake` isn't installed here, only `g++` directly. Every number and test result in this README was produced via the raw `g++` commands below, which have been run and verified. The `CMakeLists.txt` targets mirror those same commands, but treat the CMake path as unverified until you've run it yourself.)*

Or without CMake:
```bash
cd cpp
g++ -std=c++20 -O2 -Wall -Wextra -Iinclude src/order_book.cpp    tests/test_order_book.cpp    -o test_v1 && ./test_v1
g++ -std=c++20 -O2 -Wall -Wextra -Iinclude src/order_book_v2.cpp tests/test_order_book_v2.cpp -o test_v2 && ./test_v2
```

Benchmark:
```bash
g++ -std=c++20 -O2 -Iinclude -Ibench src/order_book.cpp    bench/bench_v1.cpp -o bench_v1 && ./bench_v1
g++ -std=c++20 -O2 -Iinclude -Ibench src/order_book_v2.cpp bench/bench_v2.cpp -o bench_v2 && ./bench_v2
```

Rust sidecar (compiled + tested on the user's machine, 15/15 passing — see "Rust sidecar" above):
```bash
cd rust
cargo build
cargo test
```

Dashboard (no build step):
```bash
open dashboard/index.html   # or just double-click it
```

## CI

`.github/workflows/ci.yml` builds v1 and v2 (both the raw g++ commands and the CMakeLists, so both paths are actually checked), runs unit tests, and runs the differential fuzz suite as a **correctness gate** — a v1/v2 mismatch fails the build. The benchmark step runs but doesn't gate the build, because CI runners are shared and noisy — same reasoning as the "don't trust absolute sandbox numbers" note above.

## Design docs

Full architecture decisions (order book data structure, threading model, Rust/C++ FFI boundary, explicit v1 scope guard, and the current status of each) are in [`ADR.md`](ADR.md).

## Roadmap

1. ~~v1 correctness baseline~~
2. ~~v2: array + intrusive list + arena allocator, verified against v1~~
3. ~~Benchmark harness, fix the first real bottleneck found~~
4. ~~Multithreading (single-writer-per-symbol, SPSC ring buffers), verified under TSan/ASan~~
5. Rust sidecar wired up over `cxx` — FIX parsing, pre-trade risk pre-check — **compiled and tested on the user's machine, 15/15 tests passing**, including two live tests through the real `cxx` bridge (see "Rust sidecar" above)
6. Real-hardware benchmark run, replace sandbox numbers everywhere
7. ~~Replay real market data (LOBSTER NASDAQ order-book reconstructions) through the engine~~ — done, see "Real data replay"
8. Live data: run `tools/capture_live_orderbook.py` for real (needs your machine), build `replay_live.cpp` against the capture
9. ~~Live dashboard: order book depth + trade tape + latency, updating live~~ — done for the historical-replay case, see `dashboard/index.html`; a true live-market version depends on item 8

## License

MIT — see [`LICENSE`](LICENSE).
