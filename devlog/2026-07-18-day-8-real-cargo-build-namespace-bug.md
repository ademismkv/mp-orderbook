# Day 8 — first real `cargo build`, one real bug, one real fix

**Asked:** get the Rust/`cxx` FFI sidecar (ADR-3) actually built, not just written and mock-tested.

**Got:** the user ran `cargo build` from `rust/` on their own machine — the first time this crate has ever gone through a real Rust toolchain — and it failed on the first try, with a genuine compiler error:

```
error: no member named 'OrderBookV2Ffi' in namespace 'ffi'; did you mean simply 'OrderBookV2Ffi'?
  757 |   bool (::ffi::OrderBookV2Ffi::*reduce$)(::std::uint64_t, ::std::uint64_t) = &::ffi::OrderBookV2Ffi::reduce;
../cpp/include/order_book_v2_ffi.h:30:7: note: 'OrderBookV2Ffi' declared here
   30 | class OrderBookV2Ffi {
fatal error: too many errors emitted, stopping now [-ferror-limit=]
20 errors generated.
```

**Root cause:** `rust/src/ffi.rs` declared `#[cxx::bridge(namespace = "ffi")]`. That attribute tells `cxx_build` to generate C++ glue code that expects every bridged type — the opaque `OrderBookV2Ffi` type, the free function `make_order_book`, all of it — to live inside `namespace ffi { ... }` on the C++ side. But `cpp/include/order_book_v2_ffi.h` and `cpp/src/order_book_v2_ffi.cpp` declared `class OrderBookV2Ffi` and `make_order_book(...)` at global scope, no namespace wrapper. Twenty near-identical errors, one root cause: every use site of the type in the generated glue hit the same missing-namespace lookup failure.

**Why the earlier "verification" didn't catch it:** `cpp/tests/test_order_book_v2_ffi_standalone.cpp` (built day 7) compiles the real, unmodified `order_book_v2_ffi.cpp` against hand-written mock headers in `cpp/tests/ffi_mock/`, so it can be tested without `cargo`. Those mocks declared `FfiSide`/`FfiOrderType`/`FfiOrderRequest`/`FfiTrade` at global scope too — matching the (also-wrong) real header, not matching what `cxx_build` would actually generate given `namespace = "ffi"`. The standalone test proved the adapter's field-by-field translation logic is correct. It never proved the namespace was right, because both halves of that test shared the same wrong assumption. Only a real `cargo build`, run by someone with an actual Rust toolchain, could surface this — which is exactly what happened.

**Fix:** dropped `namespace = "ffi"` from the bridge macro in `rust/src/ffi.rs`, so `cxx_build` now generates everything at global scope — matching the C++ header as already written. Smaller, lower-risk change than the alternative (wrapping the C++ class and free function in `namespace ffi {}` across two files). Re-ran the standalone C++ test after the change to confirm it's still green — it is, and now for the right reason, since the mock headers' global-scope assumption is actually correct.

**Kept:** the honest framing throughout — this was flagged in the README before it happened ("this will probably need at least one build-fix-retry pass, and that's fine, not a failure"), and the loop worked exactly as described: real error pasted back, root cause identified from the error text and known `cxx` namespace semantics, one-line fix, re-verify what can be re-verified locally (the standalone C++ test), hand back for another real attempt.

**Changed:** README's "Rust sidecar" section and status lines, ADR-3's status and honest-caveat paragraph, ADR-4's FIX line, the summary table's "Rust boundary" row — all updated to say what actually happened (real build attempted, real bug found and fixed) instead of "not yet compiled."

**Wrong, worth naming plainly:** the mock-header standalone test was described in earlier docs as proof the FFI adapter "works." That was too strong — it proved the C++ translation logic works, but the mocks encoded the same namespace mistake as the real header, so the test was blind to exactly the class of bug that showed up. A more accurate framing, adopted now: standalone C++ mock tests catch logic bugs in the adapter; only a real `cxx_build` run catches bridge-configuration bugs like this one. Both are needed; neither substitutes for the other.

**Still outstanding:** a full `cargo build && cargo test` pass with this fix applied hasn't happened yet — this sandbox still has no `cargo`, so that's the next real data point, pending the user re-running it.
