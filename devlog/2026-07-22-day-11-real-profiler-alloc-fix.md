## 2026-07-22 — real CPU profiler data, match() allocation removed

Got real hardware sampling data via Xcode Instruments' CPU Profiler on real hardware (the `.trace` bundle itself is a proprietary Apple format with no parser outside Instruments.app, but its call-tree view can be copied out as text). Finding: `operator new` inside `match()` was 35% of `match()`'s own time — traced to `match()`/`add()` returning `std::vector<Trade>` by value, heap-allocating fresh on every call that produced a fill.

Fixed by adding a reusable-buffer overload — `add(req, out_trades)` clears and reuses a caller-owned vector instead of allocating one per call — and switching every hot-path caller (both benchmarks, the FFI adapter) to it. Kept the old by-value `add(req)` for tests/fuzzers/replay tools that don't care about the allocation.

Verified: 15/15 C++ unit tests, 48/48 differential fuzz runs (fixed-band + rebase-walk) against the v1 reference, clean under ThreadSanitizer and AddressSanitizer, identical trade count/volume on the real NASDAQ replay. Real hardware provided the finding; this sandbox verified correctness wasn't broken making the fix.
