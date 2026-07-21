## 2026-07-17 — replaced the dashboard, verified the FFI adapter standalone

The JS-based dashboard was a real mistake, not just a framing issue — a C++-focused project should show the C++ engine's real numbers. Replaced it with one driven by the actual compiled engine's real per-batch timings over the full 400K-event trading day, reconfirming zero invariant violations at every batch granularity tested.

Verified the FFI adapter's C++ half by compiling it against hand-written mock headers standing in for what `cargo`/`cxx` would generate (no Rust toolchain available in this environment) — passed real assertions on crossing trades, resting orders, cancel, reduce, and depth tracking. This proves the adapter's translation logic; it doesn't prove the real `cxx` bridge compiles, which needed a real `cargo build` to confirm later.

Re-ran the multithreading benchmark fresh rather than reusing older numbers, and surfaced it directly on the dashboard next to the replay panel.
