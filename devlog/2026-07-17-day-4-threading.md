## 2026-07-17 — threading: SPSC ring buffer, TSan/ASan verified

Built a lock-free single-producer/single-consumer ring buffer (cache-line padded head/tail — the one place padding actually matters, since two real threads touch it) and a full producer → ring → matching thread → ring → consumer pipeline, one thread per symbol, no locks on the book itself.

Verified against a sequential reference across 6 seeds, then run clean under ThreadSanitizer (zero races) and AddressSanitizer + UBSan (zero memory errors) — matching output alone doesn't prove concurrent code is race-free, since two racy accesses can still coincidentally produce a correct-looking result.

Scaling benchmark (independent per-symbol books, zero shared state): near-linear up to the sandbox's 4 physical cores, 5.39M → 21.27M ops/sec from 1 to 4 symbols.
