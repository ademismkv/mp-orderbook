# Architecture Decisions

Concrete decisions, not options. Each one states what to build, why, and what it costs.

## ADR-1: Threading model — single-writer-per-symbol, not shared-state multithreading

**Decision:** One matching thread owns one symbol's order book exclusively for its entire lifetime. No lock ever guards the book itself. Parallelism happens *across* symbols (N symbols → up to N threads, pinned to cores), not *within* one symbol's book.

**Why:** This is Martin Thompson's single-writer principle applied directly. Most "multithreaded matching engine" projects try to make one order book safe for concurrent writers (locks or CAS loops on a shared structure) and pay for it in latency and bug surface. Real exchanges shard by symbol instead — AAPL's book and TSLA's book never touch the same lock because they're never touched by more than one thread each. LMAX runs its entire business logic single-threaded for the same reason.

**What this means concretely:**
- Ingestion threads (parsing FIX/ITCH-style input) push orders onto a per-symbol SPSC ring buffer.
- Exactly one matching thread per symbol drains its ring buffer and mutates that symbol's book — no atomics, no locks, on the book data structure itself.
- Output (trades, market data) goes out on a second SPSC ring buffer per symbol.
- This means "lock-free order book" (an earlier framing) is the wrong target: you don't need a lock-free *book*, you need lock-free *queues feeding into and out of* an ordinary single-threaded book — a meaningfully smaller and more tractable problem.

**Cost:** Cross-symbol operations (portfolio-level risk checks) don't get atomicity for free — handled at the gateway/risk layer, not inside matching, same as real exchanges.

**Status:** Implemented (`cpp/include/spsc_ring.h`, `cpp/tests/test_threaded.cpp`, `cpp/bench/bench_threaded_scaling.cpp`). Verified against the sequential reference on 6 seeds, and clean under ThreadSanitizer (zero races, 5 seeds) and AddressSanitizer+UBSan (zero memory errors). Scaling measured near-linear (99-109% efficiency) up to 4 symbols on a 4-core sandbox — see README "Threading" section and `devlog/2026-07-17-day-4-threading.md`. Not yet tested: real network ingestion, symbol count exceeding core count (oversubscription), or a persistent multi-symbol engine process (current test is a synthetic benchmark, not the real entry point).

## ADR-2: Order book data structure — price-level array + intrusive list, arena-allocated

**Decision:** A flat array of price levels indexed by tick offset from a reference price (not a tree), where each level holds an intrusive doubly-linked list of orders. Orders are allocated from a pre-sized arena, not `new`/`malloc`, and freed back to a free-list on cancel/fill.

**Why:**
- A price-indexed array gives O(1) best-bid/best-ask lookup and sequential, cache-friendlier memory layout than a tree's pointer-chasing.
- An intrusive list mutates pointers only on insert/erase — no container copies, and cancel is true O(1) (unlink) instead of a linear scan.
- Arena allocation removes `malloc` from the hot path entirely.

**Status:** Implemented as `OrderBookV2` (`cpp/include/order_book_v2.h`, `cpp/src/order_book_v2.cpp`). Verified behaviorally identical to the v1 reference implementation by differential fuzzing (1.5M+ operations, zero mismatches — see `cpp/fuzz/` and the devlog). Benchmarked faster than v1: see README benchmark table. One real bug (a hash-map rehash storm on the `index_` map) was found and fixed by measuring, not by guessing — see `devlog/2026-07-17-day-2-optimize-and-ci.md`.

**Known cost, not yet stress-tested at scale:** the price array rebases (grows/shifts) when an order's price falls outside the current window. Two unit tests cover this (front-growth, back-growth), but only single rebases each — repeated rebases in one run haven't been fuzzed yet.

## ADR-3: Rust ↔ C++ boundary — `cxx` for control-plane, SPSC ring buffer for data-plane

**Decision:** Two different mechanisms for two different kinds of traffic:
- **Control plane** (start/stop, config, risk-limit updates, symbol subscribe/unsubscribe) — `cxx` crate, synchronous function calls. Low-frequency; correctness and ergonomics matter more than nanoseconds here.
- **Data plane** (every order, every trade, every market-data tick) — a hand-rolled SPSC ring buffer in shared memory, cache-line padded head/tail indices. `cxx` is zero-copy for simple types but is still a function-call boundary; per-order call overhead across FFI, millions of times a second, reintroduces exactly the cost the single-writer design elsewhere is meant to avoid.

**Open decision — now resolved:** is pre-trade risk actually synchronous-blocking on the matching thread, or an async pre-check that rejects orders before they reach the ring buffer? Resolved as the latter, and built: `rust/src/risk.rs` runs on the ingestion side (symbol allowlist, max order size, max notional, price collar in bps against the book's reference price), never on the matching thread's own critical path. 5 unit tests. Same not-yet-compiled caveat as the rest of this ADR.

**Status:** FFI boundary implemented, not yet built/verified. `rust/src/ffi.rs` declares the `cxx` bridge (shared `FfiSide`/`FfiOrderType`/`FfiOrderRequest`/`FfiTrade` types, opaque `OrderBookV2Ffi` C++ type). `cpp/include/order_book_v2_ffi.h` + `cpp/src/order_book_v2_ffi.cpp` are the adapter layer — field-by-field translation into the real `::OrderRequest`/`::Trade`, deliberately never touching `OrderBookV2` itself. `rust/build.rs` compiles the bridge plus `order_book_v2.cpp` and the adapter directly via `cxx_build`, independent of the CMake build. `rust/src/lib.rs`'s `MatchingEngine` is a safe wrapper with two tests (`crosses_and_reports_a_trade`, `non_crossing_order_rests`).

Honest caveat, updated: the **C++ half is now actually compiled and tested in this environment** — `cpp/tests/test_order_book_v2_ffi_standalone.cpp` builds the real, unmodified `cpp/src/order_book_v2_ffi.cpp` against hand-written mocks of the two headers `cxx_build` would otherwise generate (`cpp/tests/ffi_mock/rust/cxx.h`, `cpp/tests/ffi_mock/matching-engine-sidecar/src/ffi.rs.h` — field-for-field identical to what `rust/src/ffi.rs` declares), and passes real assertions on crossing trades, resting orders, reduce/cancel, and depth. This proves the adapter's translation logic is correct. It does **not** prove the real `cxx` bridge compiles — that's a separate claim, and it's still unverified: no `rustc`/`cargo` in this sandbox, freshly reconfirmed (not assumed) — `rustup.rs`, `static.rust-lang.org`, and `crates.io` all return 403 from the sandbox's network proxy. Expect to run `cd rust && cargo build` locally, paste back the first compiler error if there is one, and iterate — that loop is the plan, not a fallback, and it's now a smaller remaining gap than before this session's C++-side verification.

## ADR-4: What's explicitly out of scope for v1

- No pro-rata matching, no auctions, no dark pools — pure price-time priority only.
- No true lock-free *book* internals (ADR-1 removes the need for this).
- No multi-host/distributed sharding — single host, multi-core, symbol-sharded (ADR-1) is the whole v1 story.
- FIX support, when built: inbound only, 4.4, enough fields to place/cancel/modify a limit order — not a full spec implementation. **Status:** built (`rust/src/fix.rs`) — parses NewOrderSingle/OrderCancelRequest/OrderCancelReplaceRequest, 8 unit tests, no session layer or outbound messages. Same build caveat as ADR-3: not yet compiled, since this crate can't be built in this sandbox.

## Summary table

| Decision | v1 (baseline) | v2 (current) | v3 (planned) |
|---|---|---|---|
| Book container | `std::map` + `std::deque` | array of price levels + intrusive list + arena | same — Node cache-padding tried, measured no single-threaded benefit, not shipped (devlog day 2) |
| Threading | single-threaded, implicit | single-threaded, implicit | ~~explicit single-writer-per-symbol + SPSC ring buffers~~ **implemented**, verified under TSan/ASan, near-linear scaling to 4 cores (ADR-1) |
| Rust boundary | none | none | `cxx` bridge **implemented** (control-plane calls), FIX 4.4 inbound parser + async risk pre-check **implemented** — none of it compiled yet, no `cargo` in this sandbox (ADR-3) |
