## 2026-07-18 — first real cargo build, namespace bug

First real `cargo build`, run on real hardware for the first time: failed with a namespace mismatch between the `cxx` bridge macro (declared `namespace = "ffi"`) and the C++ header (declared its class at global scope). Fixed by dropping the namespace from the bridge declaration so both sides agree.

The earlier standalone C++ verification didn't catch this because its mock headers made the identical global-scope assumption as the real header — proving the adapter's translation logic, but never testing whether the namespace was actually right. Only a real build could do that.
