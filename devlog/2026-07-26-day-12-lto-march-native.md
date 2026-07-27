## 2026-07-26 — LTO + -march=native

`order_book_v2` builds as a separate static lib from the benchmarks, so `add()`/`match()`/`push_back_order()`/`ensure_index_for_price()` were never inlined into the benchmark's hot loop — no LTO in the CMake `Release` config. Turned on `CMAKE_INTERPROCEDURAL_OPTIMIZATION` and added `-march=native`.

Measured ~5% higher throughput on interleaved before/after `bench_v2` runs (sandbox hardware — not the Mac these numbers are normally taken on). p50/p99 latency unchanged; the win shows up in aggregate wall-clock, not per-op timing.

Verified no behavior change: 10/10 v2 unit tests, 8/8 differential fuzz seeds byte-identical against a non-LTO/non-native build, threaded pipeline still matches the sequential reference (plain build and under TSan).

`-march=native` means the built binary targets the machine it was built on — fine here, not fine if this were ever distributed as a portable binary.

Next: get a real number on the actual Mac via `quickstart.sh`.
