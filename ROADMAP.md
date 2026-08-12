# Roadmap: matching core → exchange

What exists today (V1/V2) is a matching core: an order book that takes
already-parsed requests and matches them correctly, fast, and verifiably.
What's missing is everything upstream and downstream of that core — a real
exchange has to receive a live market data feed, sequence it, apply risk,
and publish its own outbound feed. This document tracks that gap and the
plan to close it, staged so each version is a real, checkpointable
improvement rather than one undifferentiated pile of work.

## V1 — done

- Matching engine (array-of-price-levels + arena + intrusive list, v2)
- CSV/LOBSTER replay against real NASDAQ data
- Differential fuzzing (v1 `std::map` reference vs v2)
- Rust sidecar: FIX parsing + pre-trade risk check, bridged via `cxx`

## V2 — done

- Public repo, reproducible benchmarks
- p50/p99/p99.9 latency reporting, not just a median
- Measurement methodology documented (see devlog)
- CI: build + test + differential-fuzz gate on every commit

## V3 — in progress (this roadmap)

The actual exchange pipeline, replacing "CSV in, matched trades out" with
"real wire-format market data in, sequenced and risk-checked, matched
trades out":

```
NASDAQ ITCH 5.0 UDP multicast
        ↓
   Feed Handler        (parse ITCH 5.0, MoldUDP64 session: gap detect + recover)
        ↓
   Sequencer           (single global order, feeds per-symbol shards)
        ↓
   Order Books         (existing OrderBookV2, one per symbol, one writer thread)
        ↓
   Risk                (pre-trade: fat-finger, max position, max order size, price bands, self-trade prevention)
        ↓
   Execution Reports    (acks, fills, rejects)
        ↓
   Market Data Feed     (outbound, republish book state / trades)
```

Order types beyond plain Limit: Market, IOC, FOK, Post-Only.
Session layer: heartbeats, session establishment, snapshot recovery.

### Feed Handler build order (highest-priority missing piece)

1. Real ITCH 5.0 message parser (full spec, ~20+ message types) — verified
   against real Nasdaq-published sample data, not synthetic vectors.
2. MoldUDP64 session framing — this is how real ITCH multicast actually
   handles gaps: sequence numbers + heartbeats + a rewind/retransmission
   request-response session. Building an ad hoc gap scheme instead of this
   would be the wrong answer to "how do you detect a dropped packet."
3. Real UDP multicast sender (simulated exchange feed) + receiver (the
   feed handler itself), exercised over loopback since there's no live
   exchange feed to actually subscribe to.
4. AVX2 SIMD parser for the hot decode path, differentially verified
   byte-for-byte against the scalar parser — same v1-vs-v2 discipline
   already used for the matching core.
5. Wire the parsed, sequenced stream into the existing OrderBookV2
   pipeline, closing the loop end to end.

### Concurrency model for V3

Not "N threads working on shared state" — symbol-sharded, same principle
ADR-1 already established for the matching core:

```
symbol → hash → shard → single writer (one book, one thread, no mutex)
```

One global sequencer assigns order; per-symbol shards each own their book
exclusively. No shared mutable state between shards.

## V4

- `io_uring` receive path (replacing the loopback-socket receive path)
- Busy polling instead of blocking recv
- Journal → replay → replica: deterministic state reconstruction from a
  durable log, not "distributed" in the consensus sense — this is the
  honest version of that feature for a project this size
- Snapshot recovery
- Live latency dashboard

## V5 (optional / stretch)

- Hardware timestamping (`SO_TIMESTAMPING`) for a real tick-to-trade
  number instead of an internally-measured one
- Formal verification of core invariants (TLA+): never-crossed book,
  quantity conservation, price-time priority

## Cross-cutting work (applies across all versions above)

**Latency metrics** — p50/p95/p99/p99.9/max for add, cancel, modify, and
execute individually, not one aggregate number. Tick-to-trade (NIC →
parse → sequence → book → risk → execution → response) as the headline
metric once the pipeline exists end to end, not internal-only latency.

**Scalability benchmarks** — sweep book depth (10/100/1,000/10,000),
message rate (1x/2x/5x/10x), and symbol count (1/10/100/1,000);
measure throughput, latency, queue buildup, dropped packets, and
backpressure behavior at each point, not just the happy-path number.

**Profiling** — `perf stat` (cache misses, branch misses, cycles, IPC) and
`perf c2c` (false sharing) alongside the existing Instruments captures;
flamegraphs generated from real runs.

**Measurement methodology** — document pinned/isolated cores, turbo and
C-states disabled, `rdtscp`/fenced timing, TSC calibration. The point is
proving the benchmark isn't fake, the same way real p50/p99/stdev-across-
runs reporting already does for the current numbers.

**Low-level optimizations** — branchless lookups, `__builtin_prefetch`,
flat-array "near book" vs. hash-map "far book", `[[likely]]`/`[[unlikely]]`,
huge pages, `-march=native` (already landed — see devlog day 12).

**Production features** — self-trade prevention, fat-finger limits, max
position, max order size, price bands, heartbeats, sessions, snapshot
recovery.

**Testing** — property-based invariants (never-crossed book, quantity
conservation, price-time priority preserved) on top of the existing
differential fuzzing; every failed fuzz case gets its seed logged for
deterministic replay.

**CI** — done (day 20): benchmark runs on every commit with results
uploaded as an artifact, a real p99/throughput regression gate against a
measured (not guessed) noise-tolerant baseline, a zero-allocation
assertion on the hot path (`test_zero_alloc.cpp` — found and fixed a real
allocation bug, a missing `free_list_.reserve()`), and ASan+UBSan / TSan
as their own CI jobs, not just a manual command.

**Visualization** — book replay animation exists (`dashboard/index.html`,
real precomputed data, day ~2). Still open: a true p50/p99/p99.9 latency
*distribution* histogram (the dashboard currently has a per-batch latency
line chart, not a distribution view).

**Documentation** — done (day 20): README.md now has dedicated
Architecture, Design decisions & tradeoffs, Measurement methodology, and
Limitations sections, cross-referencing ADR.md/devlog rather than
duplicating them.

## Being honest about what's missing today

The current engine assumes: perfect input, already-parsed messages, no
dropped packets, no malformed packets, no network, no risk, no sessions,
no backpressure. Real exchanges don't get any of that for free. Closing
V3 is what turns "a fast order book" into "an exchange" — that's the
actual gap this roadmap exists to close.
