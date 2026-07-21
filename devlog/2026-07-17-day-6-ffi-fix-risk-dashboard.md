## 2026-07-17 — cxx FFI bridge, FIX parsing, risk pre-check, first dashboard

Wired the Rust sidecar to C++ over `cxx`: shared FFI types plus an adapter that translates field-by-field into the real order book types without touching the matching logic itself, so a bug in the adapter can't silently corrupt already-verified code. Added a FIX 4.4 inbound parser (`NewOrderSingle`/`Cancel`/`CancelReplace`, 8 tests) and an async pre-trade risk pre-check (symbol allowlist, size/notional limits, price collar, 5 tests) — both pure logic, no I/O, no `unsafe`.

Built a first interactive dashboard, driven by a JS reimplementation of the matching logic over a small embedded data sample. Flagged even at the time as showing JS-interpreter timing, not the real C++ engine's numbers — replaced the following session.
