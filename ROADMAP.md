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

## V3 — done (day 22c)

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

Order types beyond plain Limit: Market, IOC, FOK, Post-Only — done (day 21):
implemented and differentially fuzzed in both v1 and v2; not yet wired into
the ITCH pipeline above the core engine (real Add Order messages are all
plain Limit) or the Rust FFI layer (no rustc/cargo available to verify a
Rust-side change) — see README's Limitations section.
Session layer: heartbeats — done, including a real live-socket test proving
gap detection from a heartbeat alone during an idle period (day 21,
`test_udp_multicast_heartbeat`); session establishment — not a gap at all,
see ADR-5 (MoldUDP64 is connectionless/publish-only by design, no
login/handshake exists in the real protocol; that belongs to SoupBinTCP, a
different Nasdaq protocol for order entry, out of scope for a market-data
pipeline); snapshot recovery — done (day 22): a real TCP-based exchange
(`cpp/feed/snapshot.h`, `cpp/feed/tcp_socket.h`, GLIMPSE-inspired, not
literal SoupBinTCP framing) lets a subscriber that joins mid-stream
bootstrap full per-order book state as of a given sequence number, then
continue via `moldudp64::Session`'s new `start_seq` parameter — verified
end to end (`test_snapshot_recovery`) with a subscriber that never receives
the first half of real event history at all, still matching the true
engine's final state for all 828 real symbols.

### Feed Handler build order — all done

1. Real ITCH 5.0 message parser (full spec, ~20+ message types) — verified
   against real Nasdaq-published sample data, not synthetic vectors. **Done.**
2. MoldUDP64 session framing — this is how real ITCH multicast actually
   handles gaps: sequence numbers + heartbeats + a rewind/retransmission
   request-response session. Building an ad hoc gap scheme instead of this
   would be the wrong answer to "how do you detect a dropped packet."
   **Done**, including real-socket heartbeat-during-idle gap detection
   (day 21) and real snapshot recovery for a subscriber joining mid-stream
   (day 22b).
3. Real UDP multicast sender (simulated exchange feed) + receiver (the
   feed handler itself), exercised over loopback since there's no live
   exchange feed to actually subscribe to. **Done.**
4. SIMD parser for the hot decode path, differentially verified byte-for-
   byte against the scalar parser — same v1-vs-v2 discipline already used
   for the matching core. **Done as NEON, not AVX2** — this repo's actual
   hardware is ARM64; shipping unverified x86 SIMD code would have violated
   the same discipline this substitution follows (devlog day 16). Result is
   honestly inconclusive (0.58x-1.51x vs. a fair scalar baseline), not
   presented as a clean win.
5. Wire the parsed, sequenced stream into the existing OrderBookV2
   pipeline, closing the loop end to end. **Done**, including proving the
   symbol-sharded concurrency model (ADR-1) against this real pipeline, not
   just a synthetic benchmark (day 22c).

### Concurrency model for V3

Not "N threads working on shared state" — symbol-sharded, same principle
ADR-1 already established for the matching core:

```
symbol → hash → shard → single writer (one book, one thread, no mutex)
```

One global sequencer assigns order; per-symbol shards each own their book
exclusively. No shared mutable state between shards.

**Done (day 22c)**: proven against the real ITCH pipeline itself, not just
`bench_threaded_scaling.cpp`'s synthetic workload. A single router thread
reads the real file and routes each message to one of 4 shards by
`hash(symbol)`; each shard owns an independent `Sequencer` + `RiskEngine` +
set of `OrderBookV2`s and runs the real, unmodified pipeline code. Checked
against a sequential reference run over the same real file: exact match on
every `Sequencer::Stats` field and 0 best-bid/ask mismatches across all 828
real symbols (`test_concurrent_sharded_pipeline`), clean under ThreadSanitizer
too (verified up to 250,000 real messages — the largest run that fits this
repo's own dev sandbox's memory alongside TSan's overhead; a structural race
would reproduce at that scale, not just at the full file's).

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

**Production features** — self-trade prevention, fat-finger limits
(`max_order_size`), price bands: **done** (day 15-16, `RiskEngine`).
Heartbeats: **done** (day 20-21). Snapshot recovery: **done** (day 22).
Still missing: max-position limits — would need tracking net position per
participant, which needs the same participant-identity information
self-trade prevention is already honestly missing for the anonymous
majority of real ITCH flow (see Limitations).

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

This section described the original state of the project, before V3 work
started: "perfect input, already-parsed messages, no dropped packets, no
malformed packets, no network, no risk, no sessions, no backpressure."
That's no longer accurate and is kept here only as a record of where this
started — real ITCH parsing, real UDP multicast with real gap detection
and recovery, a real risk stage, heartbeats, and real snapshot recovery
are all built and verified (see the pipeline diagram and Verification
section above, and README.md).

What's honestly still open as of day 22c, updated rather than left stale:
- Position-based risk limits (would need participant-identity information
  most real ITCH flow doesn't carry — see README's Limitations).
- A true p50/p99/p99.9 latency *distribution* histogram in the dashboard
  (currently a per-batch line chart, not a distribution view).
- Multi-host/distributed sharding — explicitly out of scope (ADR-4), not
  a gap.
- The new order types (IOC/FOK/Post-Only) aren't wired into the ITCH
  pipeline above the core engine (real Add Order messages are all plain
  Limit) or the Rust FFI layer (no rustc/cargo in this environment).
- The concurrent sharded pipeline demo uses a fixed 4 shards and was only
  verified under ThreadSanitizer up to 250,000 of the file's 691,421 real
  messages (a sandbox memory constraint, not a correctness gap — see
  README's Limitations).
- Real network ingestion runs over loopback, not a real NIC or a live
  exchange feed — there is no live feed to subscribe to.
- Measurement methodology gaps documented in README (no core pinning, no
  disabled turbo/C-states, no hardware cycle counter, no TSC calibration).

Closing V3 is what turned "a fast order book" into something closer to an
honest exchange simulation — that was the actual gap this roadmap existed
to close, and the remaining items above are what's left, not a hidden
larger gap.
