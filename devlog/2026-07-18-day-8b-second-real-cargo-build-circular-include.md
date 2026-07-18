# Day 8, continued — second real `cargo build`, second real bug

**Asked:** re-run `cargo build` after yesterday's namespace fix.

**Got:** further, both on the user's machine, with a different error this time:

```
error: no type named 'OrderBookV2Ffi' in the global namespace; did you mean 'OrderBookV2'?
  678 | using OrderBookV2Ffi = ::OrderBookV2Ffi;
error: definition of type 'OrderBookV2Ffi' conflicts with type alias of the same name
   30 | class OrderBookV2Ffi {
error: cannot initialize a variable of type '::rust::Vec<::FfiTrade> (OrderBookV2Ffi::*)(::FfiOrderRequest)' with an rvalue of type '...(OrderBookV2::*)(const OrderRequest &)': type mismatch
```

**Root cause:** `cpp/include/order_book_v2_ffi.h` had `#include "matching-engine-sidecar/src/ffi.rs.h"` above its own `class OrderBookV2Ffi { ... };` declaration. Reading the error's include stack closely (`ffi.rs.cc:1` includes `order_book_v2_ffi.h`; `order_book_v2_ffi.h:24` includes `ffi.rs.h`; error is inside `ffi.rs.h:678`) shows the actual generated flow: `cxx_build` puts `#include "order_book_v2_ffi.h"` directly into its generated `ffi.rs.cc` (from the `include!(...)` line in `rust/src/ffi.rs`), and *separately* includes the generated `ffi.rs.h` — which ends with `using OrderBookV2Ffi = ::OrderBookV2Ffi;`, expecting the real class to already exist by then. My header including `ffi.rs.h` itself, before its own class declaration, meant that when `ffi.rs.h` was first pulled in (via my header, line 24), `#pragma once` had already started guarding `order_book_v2_ffi.h` — so when `ffi.rs.h` in turn (per `cxx_build`'s own internal structure) would normally rely on the consumer header having already been processed, the order was backwards. The alias ran with no real class in scope, the compiler recovered by treating the name as a bogus alias, and everything downstream cascaded from there (including the third error, a fuzzy match against `OrderBookV2` instead of `OrderBookV2Ffi`).

In plain terms: my header should never have included the generated header. cxx's model is that *your* header declares the real class, and the generated header is included separately, afterward, wherever the complete type is actually needed (implementation files) — not from within your own header.

**Fix:**
- `cpp/include/order_book_v2_ffi.h`: removed the `ffi.rs.h` include; added forward declarations `struct FfiOrderRequest;` / `struct FfiTrade;` (sufficient for by-value parameters and return types in a declaration — C++ doesn't require complete types there).
- `cpp/src/order_book_v2_ffi.cpp`: added `#include "matching-engine-sidecar/src/ffi.rs.h"` directly, right after `#include "order_book_v2_ffi.h"` — same order `cxx_build` uses internally, so `OrderBookV2Ffi` is already real by the time the alias line runs.
- `cpp/tests/test_order_book_v2_ffi_standalone.cpp`: same addition, since it constructs `FfiOrderRequest{...}` and `FfiTrade` values directly and needs the complete struct definitions, not just forward declarations.

**Verified:** rebuilt and ran the standalone C++ test (`g++ ... order_book_v2.cpp order_book_v2_ffi.cpp test_order_book_v2_ffi_standalone.cpp` against the mock headers) — still green. Re-ran the full `quickstart.sh` suite end to end — still green, same numbers as before (this change touches only include structure, not logic).

**Kept:** the same honest framing as yesterday — this is a second data point in the same build-fix-retry loop, not a setback. Two independent, real bugs found by two independent real compiler runs, each fixed in minutes once the actual error text was available. Neither would have been caught by review alone; both needed a real toolchain to surface.

**Still outstanding:** third real `cargo build` attempt, with both fixes applied, still owed — this sandbox has no `cargo`.
