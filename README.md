# matching-engine

A low-latency, multithreaded price-time-priority matching engine and order book. C++20 on the hot path, a Rust sidecar (FIX parsing, pre-trade risk) bridged over `cxx`. Real NASDAQ data replay, differential fuzz testing against a reference implementation, ThreadSanitizer/AddressSanitizer-verified concurrency.

Built with a public, dated devlog documenting the development process — what was tried, what broke, what got measured and fixed. See [`devlog/`](devlog/) and [`ADR.md`](ADR.md) for the full reasoning behind every decision below.

## Quick start

```bash
git clone https://github.com/ademismkv/mp-orderbook.git
cd mp-orderbook
./quickstart.sh
```

No CMake, no dependencies beyond a C++20 compiler. Builds v1 and v2, runs both test suites, runs the latency/throughput and multithreading benchmarks, and replays a real 400,391-event NASDAQ trading day through the engine — all live on your machine. No compiler? `docker build -t mp-orderbook . && docker run --rm mp-orderbook` runs the same thing inside `ubuntu:22.04`.

## What it's built from

| Piece | Choice | Why |
|---|---|---|
| Order book | Array of price levels (shared price axis) + intrusive doubly-linked list per level + arena allocator + a hand-written open-addressing hash map (`FlatHashMap`, backward-shift deletion) for the order-id index | O(1) best-bid/ask, true O(1) cancel, zero `malloc` on the hot path, no pointer-chasing on lookup |
| Threading | One matching thread per symbol, no locks on the book — only a lock-free SPSC ring buffer (cache-line padded) feeding orders in | Removes shared state instead of guarding it; most concurrency bugs come from multiple writers |
| Rust ↔ C++ boundary | `cxx` for control-plane calls (FIX parsing, risk checks); a shared-memory ring buffer for per-order data-plane traffic | Avoids per-order FFI overhead on the hot path — measured, not assumed (see below) |
| Prices | Integer ticks, not `double` | No float-compare bugs at price-level boundaries |
| Correctness | A second, simpler reference implementation (`std::map`-based v1) that every change is differentially fuzz-tested against | v2 is only trustworthy insofar as it's checked against something simpler and obviously correct |

## Real numbers

**Single-thread benchmark** (mixed buy/sell workload, meaningful cross rate), measured on the developer's machine:

| | v1 (`std::map` baseline) | v2 (current) |
|---|---|---|
| Throughput | ~3.3M ops/sec | 6.88M ops/sec |
| p50 | 167ns | 42ns |
| p99 | ~790ns | 333ns |
| p99.9 | — | 1292ns |

**Multithreaded scaling** (independent per-symbol books, one thread each):

| Symbols | Aggregate throughput |
|---|---|
| 1 | 6.5M ops/sec |
| 2 | 11.9M ops/sec |
| 3 | 17.6M ops/sec |
| 4 | 19.8M ops/sec |

**Real NASDAQ replay** — AAPL, full trading day, June 21 2012, 400,391 real LOBSTER-format order events:

| | |
|---|---|
| Book invariant violations (crossed book) | **0** |
| Trades matched | 13,298, totaling 528,509 shares |
| Parse failures | 0 |

**Rust → `cxx` → C++ FFI overhead**, same workload through the sidecar instead of straight C++: 6.08M ops/sec vs. 6.88M pure C++ (~12% slower, p50 83ns vs. 42ns) — real, measured cost of the language boundary, not a guess.

All numbers were measured directly by the programs in this repo (`bench_v2`, `bench_threaded_scaling`, `replay_lobster`, `bench_ffi`) — run `./quickstart.sh` to reproduce them yourself.

## Verification

- **10/10 unit tests** (v2), 5/5 (v1 reference).
- **Differential fuzzing**: v1 vs. v2 checked over 1.75M+ operations across two workloads (a fixed-price-band generator, and a price-random-walk generator added specifically to force the price-window rebase path), zero mismatches.
- **ThreadSanitizer + AddressSanitizer**: the full producer→ring-buffer→matcher→ring-buffer→consumer pipeline runs clean under both — zero data races, zero memory errors.
- **Real data replay**: 400,391 real NASDAQ events, zero invariant violations.
- **Rust sidecar**: 15/15 tests passing, including two that exercise the real compiled `cxx` bridge end-to-end (not a mock).

Three real bugs were found and fixed along the way via actual compilation and measurement, not code review: a `std::unordered_map` rehash storm (10-100x tail latency spikes, fixed with `.reserve()` then later replaced entirely with `FlatHashMap`), and two C++/Rust FFI bridge bugs (a namespace mismatch and a circular include) caught by real `cargo build` failures. Full postmortems in `devlog/`.

## Layout

```
matching-engine/
├── quickstart.sh         one command: build everything, run everything, see real numbers
├── Dockerfile             same thing, zero local setup
├── cpp/
│   ├── include/          order.h (v1) / order_book_v2.h / spsc_ring.h / flat_hash_map.h
│   ├── src/               order_book.cpp (v1) / order_book_v2.cpp
│   ├── tests/             unit tests, test_threaded.cpp (TSan/ASan)
│   ├── bench/              latency/throughput, per-phase breakdown, perf/thread-pinning tools
│   ├── fuzz/               differential fuzz (v1 vs v2), two workload generators
│   ├── tools/              replay_lobster.cpp — real NASDAQ data replay
│   └── CMakeLists.txt
├── data/                   real LOBSTER sample data
├── dashboard/              index.html — replay dashboard, open directly in a browser
├── rust/                   sidecar — cxx FFI bridge, FIX 4.4 parser, risk pre-check
├── devlog/                 dated entries: what was tried, measured, fixed
├── .github/workflows/      CI: build, test, differential fuzz gate
└── LICENSE                 MIT
```

## Build & test

Fastest path is `./quickstart.sh`. Individual pieces:

```bash
cd cpp
g++ -std=c++20 -O3 -Iinclude src/order_book_v2.cpp tests/test_order_book_v2.cpp -o test_v2 && ./test_v2
g++ -std=c++20 -O3 -Iinclude -Ibench src/order_book_v2.cpp bench/bench_v2.cpp -o bench_v2 && ./bench_v2
g++ -std=c++20 -O3 -Iinclude src/order_book_v2.cpp tools/replay_lobster.cpp -o replay && ./replay ../data/AAPL_2012-06-21_34200000_57600000_message_10.csv
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Iinclude -Ifuzz src/order_book_v2.cpp tests/test_threaded.cpp -o t_tsan && ./t_tsan 42 5000
```

Rust sidecar:
```bash
cd rust
cargo build && cargo test
cargo run --release --bin bench_ffi
```

Or with CMake (`cd cpp && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j && ctest`).

## CI

`.github/workflows/ci.yml` builds both v1 and v2, runs unit tests, and runs differential fuzzing as a correctness gate — a v1/v2 mismatch fails the build.

## License

MIT — see [`LICENSE`](LICENSE).
