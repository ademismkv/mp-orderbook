# mp-orderbook

A low-latency, multithreaded price-time-priority matching engine and order book, plus the exchange pipeline around it: a real Nasdaq TotalView-ITCH 5.0 Feed Handler, a Sequencer, pre-trade Risk, Execution Reports, and a Market Data Feed publishing over real UDP multicast. C++20 on the hot path, a Rust sidecar (FIX parsing, pre-trade risk) bridged over `cxx`.

Built with a public, dated devlog documenting the actual development process — what was tried, what broke, what got measured and fixed, including several real bugs found only by wiring real data through the real code, not by code review. See [`devlog/`](devlog/), [`ADR.md`](ADR.md), and [`ROADMAP.md`](ROADMAP.md) for the full reasoning behind everything below.

## Quick start

```bash
git clone https://github.com/ademismkv/mp-orderbook.git
cd mp-orderbook
./quickstart.sh
```

No CMake, no dependencies beyond a C++20 compiler. In under a minute: builds the core engine (v1 reference + v2), runs both unit test suites, the zero-allocation hot-path check, the latency/throughput and multithreading benchmarks, replays a real 400,391-event NASDAQ trading day, parses 691,421 real Nasdaq ITCH 5.0 messages through the Feed Handler, runs the full Sequencer → Risk → Order Books → Execution Reports pipeline against that same real data, and publishes the resulting Market Data Feed over a real UDP multicast socket to a subscriber that reconstructs the book from the wire feed alone — every number printed is measured live on your machine, not canned. No compiler? `docker build -t mp-orderbook . && docker run --rm mp-orderbook` runs the core-engine portion inside `ubuntu:22.04`.

## Architecture

What a real exchange actually does: receive a live market data feed, sequence it, apply risk, match, report back to the participant, and publish its own outbound feed. This repo builds that whole pipeline, not just the matching core in the middle:

```
Nasdaq ITCH 5.0 (real historical data, real wire format)
        |
   Feed Handler        parse ITCH 5.0 (23 message types) + MoldUDP64 session framing
        |               (gap detection, request/retransmission over real UDP multicast)
        v
   Sequencer            tracks order_ref_number -> (symbol, side, mpid) since post-Add
        |               ITCH messages don't carry it; routes to the right symbol shard
        v
   Risk                 pre-trade: fat-finger, price bounds, price bands, self-trade
        |               (runs BEFORE the order ever reaches a book)
        v
   Order Books           OrderBookV2 — array of price levels + intrusive list + arena,
        |                one per symbol, one writer thread per book (ADR-1)
        v
   Execution Reports     Ack / Reject / Fill, back to the submitting participant
        |
        v
   Market Data Feed      public book-affecting events, published over a REAL UDP
                          multicast socket wrapped in real MoldUDP64 framing, to a
                          subscriber that reconstructs the book from the wire feed alone
```

Every stage above is real code exercised against real data, not a stub — see [Verification](#verification) for exactly what was checked and how. The one honest simplification: this replays a historical ITCH file rather than subscribing to a live exchange feed (there isn't one to subscribe to), so the Feed Handler's UDP multicast receive path, the Market Data Feed's publish path, and MoldUDP64's gap-recovery mechanism all run over real sockets on loopback instead of a real NIC — every `socket()`/`bind()`/`sendto()`/`recvfrom()`/`IP_ADD_MEMBERSHIP` call is genuine, the network topology is the only thing simulated.

## Design decisions & tradeoffs

| Piece | Choice | Why | Cost |
|---|---|---|---|
| Order book | Array of price levels (shared price axis) + intrusive doubly-linked list per level + arena allocator + a hand-written open-addressing hash map (`FlatHashMap`, backward-shift deletion) for the order-id index | O(1) best-bid/ask, true O(1) cancel, zero `malloc` on the hot path (verified, see below), no pointer-chasing on lookup | The price array can rebase (grow/shift) when a price falls outside the current window — O(n) in open orders when it fires. Measured, not assumed: 0 growth events over a 2M-op synthetic benchmark, 1 over a full real trading day with a pre-sized window (ADR-2) |
| Threading | One matching thread per symbol, no locks on the book — only a lock-free SPSC ring buffer feeding orders in | Removes shared state instead of guarding it (Martin Thompson's single-writer principle); most concurrency bugs come from multiple writers, not from having no lock at all | Cross-symbol operations (portfolio-level risk) don't get atomicity for free — pushed to the risk layer, same as real exchanges (ADR-1). Proven against a synthetic benchmark from day one; proven against the real ITCH pipeline itself as of devlog day 22c (`test_concurrent_sharded_pipeline`, 4 shards, exact match vs. a sequential reference, clean under TSan) |
| Rust ↔ C++ boundary | `cxx` for control-plane calls (FIX parsing, risk checks); a shared-memory ring buffer intended for per-order data-plane traffic | Avoids per-order FFI overhead on the hot path — measured (~5-12% throughput cost across two separate measurement runs), not assumed (ADR-3) | Two real bugs surfaced only by an actual `cargo build` on real hardware — a `cxx` namespace mismatch and a circular-include ordering bug neither the C++-only standalone test nor code review caught (see devlog day 8/8b) |
| Prices | Integer ticks (`int64_t`), not `double` | No float-compare bugs at price-level boundaries; matches how LOBSTER and ITCH's own `Price(4)` convention represent price | None real — this is strictly safer than floats for this use case |
| Feed transport | Nasdaq's real MoldUDP64 session protocol (sequence numbers, heartbeats, request/retransmission) over real UDP multicast sockets, not an invented framing | This is literally the mechanism a real Nasdaq ITCH feed uses for gap recovery — building something ad hoc would be answering a different, easier question | Real UDP loss is real: found and fixed a genuine cross-platform bug (macOS's stricter multicast datagram ceiling rejected packets Linux accepted) by keeping every packet under one Ethernet MTU — see devlog day 19 addendum |
| Snapshot recovery transport | A real TCP connection (`cpp/feed/tcp_socket.h`), separate from MoldUDP64/UDP, GLIMPSE-inspired | A full-book snapshot needs reliable, ordered, arbitrarily-sized delivery — exactly what UDP multicast doesn't give for free and TCP does; real Nasdaq systems make the same split (SoupBinTCP/GLIMPSE for snapshots, MoldUDP64 for the live feed) | Not literal SoupBinTCP framing (no login sequence, no message-type byte prefix) — a length-prefixed request/response exchange only, disclosed as a simplification the same way the outbound MD feed's own wire format is |
| Message → operation mapping | Add Order lets this engine's own `match()` decide what crosses; Order Executed/Executed-With-Price are applied as `reduce()` against the resting order, never re-matched | The real exchange already decided those fills happened — re-matching would double-count. Established against real data (day 17), reused unmodified by the Sequencer | Requires tracking order_ref_number → (symbol, side, mpid) across message types, since ITCH only carries it on Add |
| Self-trade prevention | Only runs for MPID-attributed orders | Real ITCH's anonymous Add Order — No MPID Attribution is the overwhelming majority of real flow (183,724 of 183,767 in this repo's sample) and carries no participant identity to check at all | Self-trade prevention is honestly partial, not silently faked for the anonymous majority — see [Limitations](#limitations) |
| Correctness | A second, simpler reference implementation (`std::map`-based v1) that every change to v2 is differentially fuzz-tested against | v2 is only trustworthy insofar as it's checked against something simpler and obviously correct | Fuzzing only covers what the generators exercise — see Limitations |

Full reasoning, including the two things that were tried and reverted after being measured (a scrambling hash finalizer, tombstone-based hash map deletion — both made things slower, see devlog day 10), lives in [`ADR.md`](ADR.md).

## Real numbers

An actual `./quickstart.sh` run, unedited — top half is the core engine (v1 baseline vs. v2, plus the Rust FFI boundary), bottom half is the full V3 pipeline (Feed Handler through Market Data Feed, snapshot recovery, and the concurrent sharded pipeline vs. its sequential reference):

```
  ┌───────────────────────────────────────────┬────────────────────────────────────┐
  │ Metric                                    │ Value (measured just now)          │
  ├───────────────────────────────────────────┼────────────────────────────────────┤
  │ Unit tests (v1 / v2)                      │ 10/10 / 16/16                      │
  │ Zero-alloc hot path (200K ops)            │ 0 allocations [PASS]               │
  │ Single-thread throughput                  │ 7.883M ops/sec                     │
  │   p50 / p99 / p99.9 latency               │ 42ns / 250ns / 1292ns              │
  │ Multithread aggregate, 1 symbol           │ 6.553M ops/sec                     │
  │ Multithread aggregate, 2 symbols          │ 13.757M ops/sec                    │
  │ Multithread aggregate, 3 symbols          │ 18.631M ops/sec                    │
  │ Multithread aggregate, 4 symbols          │ 22.427M ops/sec                    │
  │ Real NASDAQ replay throughput             │ 1.270M events/sec                  │
  │   Real events processed                   │ 400,391                            │
  │   Trades matched / volume                 │ 13,298 / 528,509                   │
  │   Book invariant violations               │ 0                                    │
  │   Cancel / reduce misses                  │ 10,902 / 33                        │
  │ Rust -> cxx -> C++ FFI throughput         │ 7.254M ops/sec                     │
  │   p50 / p99 / p99.9 latency               │ 83ns / 333ns / 1333ns              │
  ├───────────────────────────────────────────┼────────────────────────────────────┤
  │ ITCH 5.0 Feed Handler (real msgs)         │ 691,421 msgs, 0 errors [PASS]      │
  │ Sequencer + Risk (real data)              │ 828 symbols, 181,229 orders        │
  │   Risk-rejected orders                    │ 2,538 [PASS]                       │
  │ Market Data Feed (real UDP mcast)         │ 445,131 events delivered [PASS]    │
  │   Subscriber book vs true engine          │ 0 / 828 symbols mismatched         │
  │ Snapshot recovery (real TCP + mcast)      │ snapshot: 3,910 resting orders     │
  │   Late-joiner live half delivered         │ 222,566 / 222,566 [PASS]           │
  │   Late-joiner book vs true engine         │ 0 / 828 symbols mismatched         │
  │ Concurrent sharded pipeline               │ 4 shards vs. sequential ref [PASS] │
  │   Sharded book vs sequential ref          │ 0 / 828 symbols mismatched         │
  └───────────────────────────────────────────┴────────────────────────────────────┘
```

Run it yourself to get a fresh one — the numbers above are one real run, not a fixed target; see [Measurement methodology](#measurement-methodology) for why they'll swing run to run and why that's disclosed rather than hidden behind a single "the" number.

**Core engine, single-thread** (mixed buy/sell workload, meaningful cross rate):

| | v1 (`std::map` baseline) | v2 (current) |
|---|---|---|
| Throughput | ~3.3M ops/sec | ~7-8M ops/sec |
| p50 | 167ns | 42-83ns |
| p99 | ~790ns | 209-333ns |
| p99.9 | — | 458-1458ns |

Reported as ranges, deliberately: 5 consecutive runs on this repo's own dev sandbox showed throughput swing 6.34-7.79M ops/sec and p99 swing 209-250ns from scheduler/hardware noise alone — see [Methodology](#measurement-methodology) and `cpp/bench/check_regression.py`'s docstring for the actual measured numbers behind that swing, and why a single "the number" would be misleading.

**Zero-allocation hot path**: 0 heap allocations across 200,000 steady-state add/cancel/reduce/cross operations (`cpp/tests/test_zero_alloc.cpp`, overrides global `operator new`/`delete` and counts). This test found a real gap — `free_list_` had no upfront `reserve()`, so the first several cancels after construction were a real heap growth — fixed, and the test verified it actually catches the regression before being trusted (devlog day 20).

**Multithreaded scaling** (independent per-symbol books, one thread each): near-linear to 4 symbols, roughly 10-27M ops/sec aggregate depending on run — see `bench_threaded_scaling` output for the current run's exact numbers; this repo doesn't hardcode a single "the" scaling number for the same noise reasons as above.

**Real NASDAQ replay** — AAPL, full trading day, June 21 2012, 400,391 real LOBSTER-format order events: 0 book invariant violations (crossed book), 13,298 trades matched totaling 528,509 shares, 0 parse failures.

**Rust → `cxx` → C++ FFI overhead**: measured 5.7-12% slower than pure C++ depending on run/workload (most recent run: 7.254M ops/sec vs. 7.883M pure C++, ~8.0% slower), p50 within 1-41ns of pure C++ (most recent: 83ns vs. 42ns) — a real, measured language-boundary cost, not a guess (ADR-3).

**Feed Handler**: 691,421 real Nasdaq ITCH 5.0 messages parsed, 0 unrecognized message types, 0 length mismatches, all 23 real spec message types covered (devlog day 13).

**Sequencer → Risk → Order Books → Execution Reports**: same 691,421-message file, 828 real symbols sharded, 181,229 orders added, 2,538 risk-rejected (all price-band violations on this particular data slice — see [Limitations](#limitations)), 0 book invariant violations across every shard.

**Market Data Feed**: 445,131 public market data events generated from that same run, published over a real UDP multicast socket (with deliberately dropped packets forcing real MoldUDP64 gap recovery), 100% eventually delivered, and — the actual claim of this stage — **0 mismatches** between a subscriber's book reconstructed from nothing but the wire feed and the true engine state, across all 828 symbols (devlog day 19).

**Snapshot recovery**: same real event stream split at its midpoint (222,565 events); a snapshot built from only the first half (3,910 resting orders as of that sequence) delivered over a real TCP connection to a subscriber that then receives ONLY the second half (222,566 events) live over real UDP multicast — never the first half, not even via retransmission. Result: **0 mismatches** against the true engine's final best bid/ask, across all 828 symbols (devlog day 22).

**Concurrent sharded pipeline**: the same real 691,421-message file run twice — once sequentially (the existing reference behavior), once split across 4 symbol-hashed worker threads, each with its own `Sequencer`/`RiskEngine`/`OrderBookV2`s. Exact match on every `Sequencer::Stats` field (181,229 add orders, 65,865 cancels, 176,502 deletes, 413 executes, 21,122 replaces, 2,538 risk-rejected, 445,131 market data events) and **0 best-bid/ask mismatches** across all 828 real symbols — both passes complete in under 2 seconds total (devlog day 22c).

All numbers were measured directly by the programs in this repo — run `./quickstart.sh` to reproduce every one of them yourself, live, right now.

## Measurement methodology

What's rigorous today: every number above comes from this repo's own compiled code running against real workloads (synthetic benchmark, real NASDAQ replay, or real ITCH data), reported as p50/p99/p99.9 percentiles from `histogram.h` rather than a single average, and — as of `cpp/bench/check_regression.py` — gated in CI against a baseline with tolerances wide enough (measured, not guessed) to survive real observed run-to-run noise without being a rubber stamp.

What's explicitly **not** done yet, honestly, rather than silently assumed: no pinned/isolated CPU cores, no disabled turbo boost or C-states, no `rdtscp`/fenced timing (this repo uses `std::chrono::steady_clock`, not a hardware cycle counter), no TSC calibration. This matters — without it, a benchmark can't fully rule out "a neighboring process/core-migration/frequency-scaling event happened to land during this run" as the explanation for a given number, which is exactly the kind of noise `check_regression.py`'s wide tolerances are working around rather than eliminating. Real HFT benchmarking methodology does all of the above; this repo's numbers should be read as "real measurements with real, disclosed limitations," not "publication-grade latency measurements."

## Verification

- **16/16 unit tests** (v2), 10/10 (v1 reference), plus the zero-allocation hot-path check.
- **Differential fuzzing**: v1 vs. v2 checked over 1.5M+ operations across two workloads (fixed-price-band, and a price-random-walk generator added specifically to force the price-window rebase path), including Limit/Market/IOC/FOK/Post-Only (devlog day 21), zero mismatches, run on every CI push.
- **MoldUDP64 heartbeat-during-idle, over real sockets**: a scenario `test_udp_multicast_e2e` doesn't cover — a gap discovered purely from a real heartbeat packet during an idle period (no real message triggers it), recovered via a real unicast retransmission request, all over genuine `socket()`/`sendto()`/`recvfrom()` calls (devlog day 21).
- **Snapshot recovery, over a real TCP connection**: a subscriber that never receives the first half of real event history at all (not live, not via retransmission) still reconstructs the true engine's exact final state for all 828 real symbols, using nothing but a real TCP snapshot fetch plus the second half of the live feed (devlog day 22).
- **Concurrent per-shard execution, on the real pipeline**: ADR-1's threading model was previously only proven against a synthetic benchmark. `test_concurrent_sharded_pipeline` runs the real 691,421-message file through 4 symbol-sharded worker threads (independent `Sequencer`+`RiskEngine`+`OrderBookV2`s each) and checks the result against a sequential reference: exact match on every stat, 0 best-bid/ask mismatches across all 828 real symbols, clean under ThreadSanitizer (devlog day 22c).
- **ThreadSanitizer + AddressSanitizer + UBSan**, now in CI as their own jobs (not just a manual command): the full producer→ring-buffer→matcher→ring-buffer→consumer pipeline runs clean under TSan (zero races); `test_order_book_v2`, 5 differential fuzz seeds, and the real NASDAQ replay all run clean under ASan+UBSan (zero memory errors, zero undefined behavior).
- **Real data replay**: 400,391 real NASDAQ events (core engine) and 691,421 real Nasdaq ITCH 5.0 messages (Feed Handler → Sequencer → Risk → Market Data Feed), zero invariant violations in either.
- **Rust sidecar**: 15/15 tests passing, including two that exercise the real compiled `cxx` bridge end-to-end (not a mock).
- **Market Data Feed self-consistency**: a subscriber that only ever sees public wire bytes over real UDP multicast reconstructs the exact same best-bid/best-ask as the true engine, for all 828 real symbols — proof the outbound feed actually carries enough information to be useful, not just that it doesn't crash.

Real bugs were found and fixed along the way via actual compilation, measurement, and real data — not code review:
- A `std::unordered_map` rehash storm (10-100x tail latency spikes), fixed with `.reserve()` then replaced entirely with a hand-written `FlatHashMap`.
- Two C++/Rust FFI bridge bugs (a namespace mismatch, a circular include) caught only by a real `cargo build`, not by the C++-only standalone test.
- An unbounded-allocation bug (`OrderBookV2::ensure_index_for_price()` could try to allocate ~13GB from a single sentinel-valued real ITCH price) found only by wiring real production-shaped data through the full pipeline — no unit test or fuzzer had ever triggered it (devlog day 17).
- A missing `free_list_.reserve()` causing real heap allocations on the hot path — found by `test_zero_alloc.cpp`, the first tool in this repo that actually counted allocations instead of assuming the arena design made the problem moot (devlog day 20).
- A cross-platform `EMSGSIZE` crash: packets sized fine for this repo's Linux dev sandbox were rejected outright by a real Mac's stricter multicast datagram limit — fixed by respecting a real Ethernet MTU instead of relying on IP fragmentation (devlog day 19 addendum).

Full postmortems for all of the above, including the reasoning and the exact tool output at the time, in `devlog/`.

## Limitations

Being direct about what this repo does not claim:

- **Order types**: the core engine (both v1 and v2) now implements Limit, Market, IOC, FOK, and Post-Only, differentially fuzzed against each other (devlog day 21) — but the ITCH pipeline above it only ever constructs `Type::Limit` requests, because real ITCH Add Order messages are all plain limit orders. The new types aren't wired into the Rust FFI layer either (no rustc/cargo in this environment to verify a Rust-side change) — the FFI adapter's `to_type()` still only maps Limit/Market/Cancel, a disclosed, deliberate scope cut, not an oversight.
- **Self-trade prevention** only runs for MPID-attributed orders (the minority of real ITCH flow) — the anonymous majority genuinely can't be checked with the identity information ITCH provides, and this repo doesn't pretend otherwise.
- **No position-based risk limits** — would require tracking net position per participant, which needs the same participant-identity information self-trade prevention is missing for the anonymous majority.
- **Market Data Feed's wire format is not literal ITCH bytes** — a deliberately simpler normalized event record, reusing ITCH's real transport (MoldUDP64 + UDP multicast) but not re-deriving its full 23-message-type outbound encoding.
- **Snapshot recovery is implemented** (devlog day 22) — a subscriber that joins mid-stream bootstraps full per-order book state over a real TCP connection (`cpp/feed/snapshot.h`, GLIMPSE-inspired, not literal SoupBinTCP framing), then continues via the live multicast feed starting past sequence 1. What's NOT built: periodic/automatic snapshot publishing on a schedule — this repo's snapshot builder is driven directly off a captured event stream in the verification test, not run continuously as its own always-on service the way a real GLIMPSE deployment would. **Session establishment is not a gap at all** — MoldUDP64 is connectionless and publish-only by design, with no login/handshake in the real protocol (that belongs to SoupBinTCP, Nasdaq's separate order-entry protocol, out of scope for a market-data pipeline); see ADR-5.
- **NEON SIMD parsing result is genuinely inconclusive**, not a clean win — measured 0.58x-1.51x versus a fair scalar baseline after correcting an earlier confound (the first measurement compared against `std::variant` construction overhead, not real decode cost). Reported honestly rather than presented as a success (devlog day 16). AVX2 was never built at all — this repo's actual hardware is ARM64, and shipping x86 SIMD code that was never compiled or measured would violate the same discipline used everywhere else here.
- **Everything network-related runs over loopback**, not a real NIC or a real exchange feed — there is no live feed to subscribe to. The socket code itself (bind, multicast group membership, sendto/recvfrom) is real; the topology is not.
- **Measurement methodology gaps** — see [above](#measurement-methodology): no core pinning, no disabled turbo/C-states, no hardware cycle counter, no TSC calibration.
- **No multi-host/distributed sharding** — single host, multi-core, symbol-sharded (ADR-1) is the whole story; ADR-4 states this explicitly as out of scope, not an oversight.
- **Concurrent sharded pipeline demo uses a fixed 4 shards**, not a dynamically configurable count, and its ThreadSanitizer coverage was only verified up to 250,000 of the file's real 691,421 messages — TSan's 5-10x memory overhead OOM-killed a full-file run in this repo's own 3.8GB dev sandbox. The plain (non-instrumented) run does cover the full file and passes 0-mismatch every time; a real structural data race would very likely reproduce at the smaller TSan-verified scale too, since the same code paths execute, but this is disclosed rather than silently assumed equivalent.
- **Repeated/compounding price-window rebases are not fuzzed** — the mechanism itself has unit test coverage, but a symbol with a genuinely wide, unpredictable intraday range firing the rebase path repeatedly in one run is an untested scenario (ADR-2).
- **This is a portfolio/learning project measured honestly, not a production system** — no auctions, no dark pools, no pro-rata matching, no formal verification of core invariants (TLA+ is listed as an optional stretch goal in `ROADMAP.md`, not done).

See `ROADMAP.md` for the full list of what's built versus what's still open, staged by version.

## Layout

```
mp-orderbook/
├── quickstart.sh           one command: build everything, run everything, see real numbers
├── Dockerfile               same thing (core engine portion), zero local setup
├── cpp/
│   ├── include/            order.h (v1) / order_book_v2.h / spsc_ring.h / flat_hash_map.h
│   ├── src/                 order_book.cpp (v1) / order_book_v2.cpp
│   ├── tests/                unit tests, test_threaded.cpp (TSan/ASan), test_zero_alloc.cpp
│   ├── bench/                 latency/throughput, per-phase breakdown, regression gate
│   │                            (baseline.json, check_regression.py), perf/thread-pinning tools
│   ├── fuzz/                   differential fuzz (v1 vs v2), two workload generators
│   ├── feed/                   Feed Handler: itch_messages.h/itch_parser.cpp (real ITCH 5.0),
│   │                            moldudp64.h/moldudp64_session.h, udp_multicast.h, itch_simd_neon,
│   │                            sequencer.h/.cpp, risk.h/.cpp, execution_report.h, market_data.h,
│   │                            snapshot.h/tcp_socket.h (real TCP snapshot recovery, day 22)
│   ├── tools/                 replay_lobster.cpp — real NASDAQ data replay
│   └── CMakeLists.txt
├── data/                    real LOBSTER + ITCH 5.0 sample data
├── rust/                    sidecar — cxx FFI bridge, FIX 4.4 parser, risk pre-check
├── devlog/                  dated entries: what was tried, measured, fixed
├── ADR.md                   architecture decisions, with status and measured results
├── ROADMAP.md               staged plan: what's built, what's next, what's explicitly out of scope
├── .github/workflows/       CI: build+test, differential fuzz gate, benchmark regression gate,
│                              zero-alloc check, ASan+UBSan job, TSan job, full pipeline ctest
└── LICENSE                  MIT
```

## Build & test

Fastest path is `./quickstart.sh`. Individual pieces:

```bash
cd cpp
g++ -std=c++20 -O3 -Iinclude src/order_book_v2.cpp tests/test_order_book_v2.cpp -o test_v2 && ./test_v2
g++ -std=c++20 -O3 -Iinclude src/order_book_v2.cpp tests/test_zero_alloc.cpp -o test_zero_alloc && ./test_zero_alloc
g++ -std=c++20 -O3 -Iinclude -Ibench src/order_book_v2.cpp bench/bench_v2.cpp -o bench_v2 && ./bench_v2
g++ -std=c++20 -O3 -Iinclude src/order_book_v2.cpp tools/replay_lobster.cpp -o replay && ./replay ../data/AAPL_2012-06-21_34200000_57600000_message_10.csv
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Iinclude -Ifuzz src/order_book_v2.cpp tests/test_threaded.cpp -o t_tsan && ./t_tsan 42 5000
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Iinclude src/order_book_v2.cpp tests/test_order_book_v2.cpp -o t_asan && ./t_asan

# Feed Handler / Sequencer / Risk / Market Data Feed — real ITCH 5.0 data:
g++ -std=c++20 -O3 -Iinclude -Ifeed feed/itch_parser.cpp feed/tests/test_itch_parser.cpp -o test_itch_parser && ./test_itch_parser ../data/itch50_sample_20191230.bin
g++ -std=c++20 -O3 -pthread -Iinclude -Ifeed feed/itch_parser.cpp feed/sequencer.cpp feed/risk.cpp src/order_book_v2.cpp feed/tests/test_market_data_feed.cpp -o test_md_feed && ./test_md_feed ../data/itch50_sample_20191230.bin
g++ -std=c++20 -O3 -pthread -Iinclude -Ifeed feed/itch_parser.cpp feed/sequencer.cpp feed/risk.cpp src/order_book_v2.cpp feed/tests/test_snapshot_recovery.cpp -o test_snapshot && ./test_snapshot ../data/itch50_sample_20191230.bin
g++ -std=c++20 -O3 -pthread -Iinclude -Ifeed feed/itch_parser.cpp feed/sequencer.cpp feed/risk.cpp src/order_book_v2.cpp feed/tests/test_concurrent_sharded_pipeline.cpp -o test_concurrent_sharded && ./test_concurrent_sharded ../data/itch50_sample_20191230.bin
```

Rust sidecar:
```bash
cd rust
cargo build && cargo test
cargo run --release --bin bench_ffi
```

Or with CMake — this is also what wires up every test above, including the full Feed/Sequencer/Risk/Market Data Feed suite, in one shot: `cd cpp && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j && ctest --output-on-failure`.

## CI

`.github/workflows/ci.yml` runs on every push and pull request, as three jobs:

- **`cpp`**: builds v1+v2, runs unit tests, the zero-allocation hot-path check, differential fuzzing (v1/v2 mismatch fails the build), a real p99/throughput regression gate against a committed baseline (tolerances measured against this repo's own observed noise, not guessed — see `cpp/bench/check_regression.py`), uploads that run's raw benchmark output as an artifact, then does a full CMake build and runs every test via `ctest` — including the entire Feed Handler → Sequencer → Risk → Order Books → Execution Reports → Market Data Feed pipeline against real Nasdaq data.
- **`sanitizers-asan-ubsan`**: the core engine's unit tests, 5 differential fuzz seeds, and the real NASDAQ replay, all under AddressSanitizer + UndefinedBehaviorSanitizer.
- **`sanitizers-tsan`**: the threaded producer/matcher/consumer pipeline under ThreadSanitizer.

## License

MIT — see [`LICENSE`](LICENSE).
