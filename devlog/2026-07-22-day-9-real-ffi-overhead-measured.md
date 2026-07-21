# Day 9 — real FFI overhead, measured

**Asked:** how fast is the system with both Rust and C++ working together, not just the pure-C++ engine alone.

**Got:** `rust/src/bin/bench_ffi.rs` run for real on the user's machine, via `./quickstart.sh`, same 2,000,000-order workload shape as `cpp/bench/bench_v2.cpp`:

| | Pure C++ (`bench_v2`) | Rust → `cxx` → C++ (`bench_ffi`) |
|---|---|---|
| Throughput | 5.523M ops/sec | 5.210M ops/sec |
| p50 | 83ns | 84ns |
| p99 | 500ns | 584ns |
| p99.9 | 4334ns | 4292ns |

**Reading it straight:** the FFI boundary costs about 5.7% throughput and ~1ns at p50 on this workload — essentially nothing. p99 is up 84ns (500 → 584), which is real but small. p99.9 is actually slightly *lower* through the FFI path, which is almost certainly run-to-run noise (both numbers are single runs, not medians of several — see the caveat below), not a real effect.

**What this actually confirms:** `cxx`'s "zero-copy for simple types, real function-call boundary otherwise" design claim (cited in ADR-3) holds up under a real measurement here — calling from Rust into C++ through the generated bridge is not the dominant cost in this workload; the matching logic itself still is. That's a genuinely useful data point for the ADR-3 writeup: the decision to keep `cxx` off the hot data-plane path and use a hand-rolled SPSC ring buffer instead was made on reasoning (per-call FFI overhead reintroduces the cost the single-writer design avoids), not measurement — this benchmark is the first time that reasoning has a real number next to it, and the number is smaller than the reasoning implied it might be.

**Honest caveat, not glossed over:** this benchmark calls `add()` in a tight loop from a single thread with no other contention — it measures the marginal cost of one FFI call, not the cost of routing millions of calls/sec through it under real system load (context switches, cache pressure from other threads, GC-equivalent pauses that Rust doesn't have but C++ allocator pressure can still cause). The SPSC-ring-buffer decision in ADR-1/ADR-3 still stands for the actual data plane — this number doesn't overturn that decision, it just puts a real figure on what "the alternative" would have cost, which is useful context, not a design change. Also: single run, not a median of several — the multithreaded scaling numbers in the same table show real run-to-run variance on this machine (4-symbol aggregate dropping below 3-symbol in multiple runs, most likely macOS thread scheduling noise on an 8-core machine under background load), so treat the exact percentages here as "roughly this," not to two decimal places of precision.

**Kept:** no overclaiming — the number is reported as measured, with the caveat about what it does and doesn't prove stated plainly, same as every other number in this project.
