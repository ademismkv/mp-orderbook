## 2026-08-12 — IOC, FOK, Post-Only order types

Closed the first V3 gap from the "how done are you" review: plain Limit and
Market were the only order types either engine understood. Added `IOC`,
`FOK`, `PostOnly` to `Type` (order.h and order_book_v2.h — same enum shape
in both, so differential fuzzing keeps covering v1 vs. v2). Confirmed first
that no exhaustive `switch` on `Type` exists anywhere that new enum values
would break — both engines use plain `if` checks in `match()`/`add()`, and
the FFI adapter's `to_type()` switch already has a `default:` case, so it
stays on Limit/Market/Cancel only by choice, not by accident.

**IOC** needed almost no new code: `match()`'s price-check condition
changed from `taker.type == Type::Limit` to `taker.type != Type::Market` (so
Limit, IOC, and FOK all get price-checked, Market alone sweeps
unconditionally), and the existing "only `Type::Limit` rests" condition
already meant IOC's unfilled remainder is discarded automatically, same as
Market's always was.

**FOK** needed a real non-mutating pre-check: `can_fully_fill()` walks price
levels in the crossing direction summing quantity until it either clears the
requested amount (fillable) or the price band runs out (not fillable) —
v2's version uses the already-tracked `PriceLevel::total_qty` aggregate
(same as `match()`'s scan pattern, just non-mutating); v1 has no such
aggregate (a `std::map<price, std::deque<Order>>`, correctness-over-speed by
design) so it walks individual resting orders instead. If the check fails,
`add()` returns before calling `match()` at all — no partial fill, nothing
rests.

**Post-Only** never calls `match()`: a pre-check against `best_bid()`/
`best_ask()` (v2) or `bids_.begin()`/`asks_.begin()` (v1) decides whether
the order would cross on arrival. If it would, it's rejected outright — no
trade, nothing rests. If not, it rests directly; there's nothing to match
against by definition once you know it doesn't cross, so skipping `match()`
isn't an optimization, it's what "must never take liquidity" actually means.

Extended `cpp/fuzz/workload.h`'s `OpKind` and both generators'
(`generate_ops`, `generate_ops_price_walk`) action distribution to include
`AddIOC`/`AddFOK`/`AddPostOnly` (65% limit / 10% market / 7% IOC / 7% FOK /
6% post-only / 5% cancel — previously 85/10/5), and updated all four fuzz
driver binaries (`fuzz_v1`, `fuzz_v2`, and the rebase-forcing variants) to
map the new `OpKind` values to the matching `Type`. Added 6 new unit tests
to each of `test_order_book_v2.cpp` (16 total, up from 10) and
`test_order_book.cpp` (10 total, up from 5), covering: IOC discarding a
partial remainder and doing nothing on a no-cross arrival, FOK rejecting
when only partial liquidity is available and fully filling across two price
levels when it is, Post-Only resting on a non-cross and rejecting on a
would-cross.

Verified before syncing: both unit test suites pass (16/16 v2, 10/10 v1);
8/8 differential fuzz seeds (1, 2, 3, 7, 42, 100, 999, 123456) byte-identical
between v1 and v2 on the standard fixed-price-band workload, now exercising
the new types; 4/4 seeds byte-identical on the rebase-forcing price-walk
workload too (7-10 real rebases per run, confirming the new types don't
break `ensure_index_for_price`'s rebase path); `test_zero_alloc` still 0
allocations (confirms nothing about this change touched the arena hot
path); FFI adapter (`test_order_book_v2_ffi_standalone`) still compiles and
passes under `-Wall -Wextra` unmodified — the new `Type` values aren't wired
into the Rust FFI layer, a deliberate, disclosed scope cut (no rustc/cargo
in this environment to verify a Rust-side change; real ITCH Add Order
messages are all plain Limit orders anyway, so the sequencer never needed
to construct an IOC/FOK/PostOnly request from wire data — see
`feed/sequencer.cpp`, still only ever sets `req.type = Type::Limit`).

Not attempted here: CMake wasn't available in this sandbox to run a full
`ctest` pass (no root to install it) — verification above used direct g++
builds of every affected target instead, same discipline as every other
change in this project. CI (GitHub Actions, where cmake exists) will run
the real `ctest` suite on push.
