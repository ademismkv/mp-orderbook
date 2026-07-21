## 2026-07-17 — real bottleneck found by measuring, CI, license

No profiler available (`perf` blocked by sandbox policy), so relied on before/after macro-benchmarking instead of guessing at what to optimize. Found a real bottleneck: the `index_` hash map wasn't pre-sized, so it hit repeated rehashes while scaling to hundreds of thousands of open orders. One-line fix (`.reserve()` in the constructor) cut p99 by ~35-40% and worst-case tail latency by roughly 10-100x — the signature of a rehash storm.

Also tried cache-line padding on the hot `Node` struct, measured no benefit, and didn't ship it: padding exists to prevent false sharing between threads, and there was no concurrency yet to false-share against.

Added an MIT license and a CI workflow that gates on differential-fuzz correctness (a v1/v2 mismatch fails the build).
