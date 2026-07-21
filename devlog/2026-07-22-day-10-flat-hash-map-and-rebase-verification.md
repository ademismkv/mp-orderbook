## 2026-07-22 — FlatHashMap replacement, verified end-to-end

Replaced `std::unordered_map` with a hand-written open-addressing hash map (backward-shift deletion, identity hashing tuned for sequential order ids) for the order-id index. Two earlier design attempts were tried and measured before landing here: a scrambling hash finalizer that hurt throughput by destroying cache locality on real sequential ids, and a tombstone-based deletion scheme that caused real 40-60ms compaction stalls — both replaced once measured.

Found a real gap while verifying the final version: the existing fuzz workload's fixed price band never actually triggers the rebase path, so the riskiest interaction in this change (rebase combined with the new deletion scheme) had never been tested despite passing every prior run. Added a price-random-walk workload that forces real rebases — 48 total differential runs against the reference implementation, zero mismatches, real rebase counts up to 66 per run. Also clean under ThreadSanitizer/AddressSanitizer and the real NASDAQ replay (identical trade counts and zero invariant violations to every prior run).

Measured result: p50 125ns → 84ns, p99 ~440ns → 375ns, throughput ~5M → ~5.9M ops/sec.
