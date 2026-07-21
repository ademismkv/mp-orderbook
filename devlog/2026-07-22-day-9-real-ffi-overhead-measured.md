## 2026-07-22 — real FFI overhead measured

Built a Rust-side benchmark that calls the C++ engine through the real compiled `cxx` bridge, same workload shape as the pure-C++ benchmark. Result, measured on real hardware: 5.21M ops/sec through the FFI vs. 5.52M ops/sec pure C++ — about 5.7% slower, p50 latency within 1ns of each other. The control-plane/data-plane FFI split is cheap on this workload — a real number behind the architecture decision instead of an assumption.
