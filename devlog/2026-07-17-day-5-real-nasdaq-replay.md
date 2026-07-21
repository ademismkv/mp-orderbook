## 2026-07-17 — real NASDAQ data replay

Replayed a real LOBSTER-format NASDAQ message file (AAPL, 400,391 events, a full trading day) through the engine. First run surfaced a real bug: visible-execution events weren't being applied to resting orders, leaving phantom oversized liquidity sitting in the book indefinitely. Fixed by adding partial-cancel (`reduce()`) support and applying execution quantity loss on replay, without double-counting trades.

After the fix: 13,298 trades, 528,509 shares matched, **zero book invariant violations across the entire day**. A volume gap remains between this engine's own trade ledger and LOBSTER's reported execution volume — expected, not a residual bug: roughly a third of real volume executed against hidden orders, which are by definition never visible in this message stream, and this engine's independent matching naturally diverges from history's once even one fill decision differs.

Also investigated true live (not historical) market data. Concluded it's structurally not achievable in this environment — no way to hold a persistent streaming connection open — and wrote a capture script for the user's own machine instead of relabeling a workaround as "live."
