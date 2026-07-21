## 2026-07-22 — per-phase breakdown, levels_ growth measured

Added a counter for the price-window rebase path: zero triggers in the synthetic benchmark (prices confined to a narrow band), exactly one on the full real NASDAQ trading day. Rare in practice, as the architecture always assumed — now measured, not asserted.

Added opt-in instrumentation timing five phases of `add()`. Result: matching ~27% of wall time, the `unordered_map` insert ~13-14% (the single biggest non-matching cost — roughly double arena allocation, FIFO linking, or price-level lookup, each ~6-7%). Real, independent evidence that the hash map was the right next thing to replace, not just a hunch.
