## 2026-07-22 — perf and thread-pinning tools for real hardware

Compared `-O2` / `-O3` / `-O3 -march=native`: `-O3` gave a real, repeatable throughput gain with zero correctness risk, so it's now the default build flag everywhere. `-march=native` added nothing further and was left out of defaults since it breaks portability across machines.

Built a `perf stat` wrapper (real hardware counters — cycles, IPC, cache misses, branch misses) and a thread-pinning benchmark variant, both meant to run on real, unshared hardware rather than this sandbox. `perf` doesn't exist on macOS at all — documented Docker and Xcode Instruments as the real alternatives, with the honest caveat that thread pinning is only advisory on macOS (Apple's own docs say so), not a hard guarantee the way it is on Linux.
