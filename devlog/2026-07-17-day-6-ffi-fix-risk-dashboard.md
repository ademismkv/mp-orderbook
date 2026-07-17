## 2026-07-17 (later still, again) — cxx FFI wiring, FIX parsing, risk pre-check, and a real interactive dashboard

**Asked for, verbatim:** the Rust sidecar skeleton "not wired" to C++, done. FFI to C++ (cxx, ADR-3), "not started", done. FIX parsing + risk pre-check, done. A real dashboard that pulls real NASDAQ data in batches and shows live latency/throughput stats.

Four things happened this session, in the order they were built.

### 1. The dashboard — the one thing fully verifiable in this sandbox

`dashboard/index.html`. A 735-event stratified sample of the real AAPL LOBSTER file (every execution event kept — 168 of them — the rest evenly sampled to keep the embedded payload small enough to work with reliably) drives a client-side JS reimplementation of `OrderBookV2`'s price-time-priority logic. "Pull next batch" processes N real events and reports genuinely measured `performance.now()` latency and throughput — not fabricated numbers, but also explicitly **not** the C++ engine's numbers; the page says so directly, because conflating a JS interpreter's speed with the TSan/ASan-verified C++ benchmark elsewhere in this README would be exactly the kind of thing that falls apart under scrutiny.

Verified before calling it done: extracted `<script>` content from the file and ran `node --check` on it — valid JS syntax, not just "looks right." Also spot-checked brace/paren balance wasn't broken by the large embedded data literal.

### 2. The `cxx` FFI bridge (ADR-3)

`rust/src/ffi.rs` declares the bridge — shared `FfiSide`/`FfiOrderType`/`FfiOrderRequest`/`FfiTrade` types (not reused from `::Side`/`::Type`, because `cxx` requires shared enums to be defined inside the bridge macro itself) and an opaque `OrderBookV2Ffi` C++ type. `cpp/include/order_book_v2_ffi.h` + `cpp/src/order_book_v2_ffi.cpp` are the adapter — field-by-field translation into the real `OrderRequest`/`Trade`/`Side`/`Type`, deliberately never touching `OrderBookV2` itself, so a bug here can't silently corrupt the already fuzz-tested and TSan/ASan-verified matching logic underneath. `rust/build.rs` compiles the bridge plus both C++ translation units directly via `cxx_build`, independent of the CMake build — `cargo build` from `rust/` doesn't need CMake.

**What this is not:** verified. There's no `rustc`/`cargo` in this dev sandbox — same blocker as day 4. The pre-FFI skeleton built clean on the first try on the user's machine; this is a materially bigger surface (cross-language codegen, a C++ compile step launched from `build.rs`, a generated-header include path that depends on the crate name matching exactly). Said plainly in the README and ADR.md rather than implied to be as safe a bet as the skeleton was: expect a build-fix-retry loop, not a guaranteed clean build.

### 3. FIX parsing + risk pre-check

`rust/src/fix.rs` — inbound FIX 4.4, three message types (`NewOrderSingle`, `OrderCancelRequest`, `OrderCancelReplaceRequest`), enough fields to place/cancel/modify a limit order, per ADR-4's explicit scope cut. No session layer, no outbound messages, no full spec. 8 unit tests, including a real-SOH-delimiter test (not just the more readable `|`-delimited convenience format used in the other tests).

`rust/src/risk.rs` — resolves ADR-3's previously-open "sync or async risk check" question as async/ingestion-side, per the lean already recorded there. Four checks (symbol allowlist, max order size, max notional, price collar in bps from a reference price), each independent and separately tested — 5 unit tests.

Both modules are pure logic with no I/O and no `unsafe`, which was a deliberate choice given they can't be compiled here: the smaller and more self-contained the untested surface, the less there is to go wrong on the first real build.

### 4. Docs updated to match reality, not aspiration

`ADR.md`'s ADR-3 status, ADR-4's FIX line, and the summary table all rewritten to say what's actually implemented (not "planned") while being explicit about "not yet compiled." README's status table, a new "Live NASDAQ replay dashboard" section, a rewritten "Rust sidecar" section, the layout tree, build/test instructions, and the roadmap (items 5 and 9) all updated the same way. Two claims that would have been dishonest if left as they were before this session: "Rust sidecar — skeleton" (no longer true — it's now a materially larger, unverified codebase, which is a different risk profile than a compiled skeleton) and "no live dashboard" (partially true — the historical-replay dashboard exists now, a true live-market one still doesn't, per day 5's explanation).

**Honest summary of what's provably true right now vs. what's asked-for-but-unverified:** the dashboard is real and checked (JS syntax-valid, uses real data, computes real timings). The FFI/FIX/risk code is real, reasonably careful, internally consistent with the rest of the codebase's conventions (same `Price` tick representation, same "never touch the verified core" discipline as the v1→v2 relationship) — and completely unbuilt. That's not a hedge to undercut the work; it's the same standard applied every other time in this project that something couldn't be verified in this sandbox.
