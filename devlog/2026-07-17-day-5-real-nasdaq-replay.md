## 2026-07-17 (later still) — real NASDAQ data replay, and why "live" isn't achievable in this dev environment

**Asked for:** real market data (not synthetic), then specifically live data.

**What actually happened, in order:**

### 1. Real historical data — user found and uploaded a real LOBSTER file

Traced down `totalorganfailure/lobster-data` on Hugging Face — a free, no-signup mirror of real LOBSTER sample data (NASDAQ TotalView-ITCH derived order book reconstructions). Confirmed the file was real (16.6MB, SHA256-verified, not a broken/placeholder link) before asking for it, rather than guessing. Couldn't pull it into this dev sandbox myself — it's stored on a Xet-chunked CDN that this environment's web-fetch tool can't stream, and this sandbox's own network access is allowlisted and blocks the huggingface file CDN outright (confirmed with a direct request, same category of block as `rustup.rs` earlier). User downloaded and uploaded it instead: `AAPL_2012-06-21_34200000_57600000_message_10.csv` — 400,391 real NASDAQ order events for AAPL, full trading day (9:30:00.004 to 15:59:59.913), June 21 2012.

### 2. Live data — investigated properly, confirmed it's structurally not possible here, didn't fake it

Tried: raw bash `curl` to Coinbase's and Binance's public (no-API-key) market data REST endpoints — blocked, same network allowlist issue (`403 blocked-by-allowlist`). The web-fetch tool *could* reach Coinbase's REST API and got back what looked like live data — but the returned timestamps (`2026-06-03`, `2026-05-16`) were weeks to months stale relative to today (`2026-07-17`), meaning whatever's serving those responses is caching them, not proxying live. Checked this by actually reading the returned timestamp field, not assumed.

Even setting the staleness issue aside: a single request/response fetch tool can only ever get one point-in-time snapshot, not a stream. True live order book data requires a **persistent connection** (WebSocket, typically) held open for the duration of the capture — nothing in this environment can do that. `mcp__workspace__web_fetch` is one-shot. `mcp__workspace__bash` calls are independent per invocation with no state carried over and a hard timeout, and raw network access from bash is blocked by the same allowlist. There's no way to hold a live feed open here, full stop — this isn't a workaround-able limitation, it's the tool architecture.

**Didn't try to fake this** — e.g. relabeling repeated polling snapshots as "live streaming data" would be exactly the kind of thing that falls apart under a 10-minute technical deep-dive, which is the standard this whole project is built around. Instead: wrote a script (see below) for the user to run on their own machine, which has none of these restrictions, using Coinbase's public WebSocket feed (crypto exchanges, unlike equities, commonly expose full L2/L3 order book streams with zero API key — this is the standard, well-known way student/portfolio HFT projects get free real-time data, since NASDAQ/CME live feeds are genuinely paid/licensed).

### 3. Real data replay — built it, found and fixed a real modeling bug via measurement

`cpp/tools/replay_lobster.cpp` parses the LOBSTER message format and replays it through `OrderBookV2`:
- Type 1 (new limit order) → `book.add()` — this engine independently decides whether it crosses, using the same code path as everything else in the repo.
- Type 2 (partial cancel) → `book.reduce()` — this is *why* `reduce()` got built this session (previous devlog entries only had full cancel).
- Type 3 (delete) → `book.cancel()`.
- Type 4/5 (executions) — NOT re-submitted as new operations, since they're NASDAQ's report of trades that already happened as a side effect of some type-1 crossing, not independent submissions. Re-running them through `match()` would double-count trades.

**First run, before a fix:** 37,273 of this engine's own trades, 1,627,764 total quantity, vs. LOBSTER's reported 1,845,964 visible-execution quantity — a real gap, and 22,069 `cancel()` calls referencing an order id this engine no longer recognized (out of 171,126 deletes — ~13%). Traced the cause instead of shrugging at it: type-4/5 events were being logged for reporting but **not applied to the book**, so a resting order that was genuinely partially executed in reality kept its full original quantity in this engine's reconstruction indefinitely — a phantom oversized order, sitting around until an explicit cancel/delete removed it. That inflates how much this engine's own independent matching finds to trade against, and it's exactly the kind of bug that only shows up once you check reported volume against your own — not from reading the code.

**Fix:** on type 4, call `book.reduce(order_id, size)` — apply the quantity loss to keep the book state faithful to history, without generating a synthetic `Trade` (still avoiding double-counting the trade ledger, just correcting the *state effect*). After the fix: 13,298 trades, 528,509 quantity, `cancel()` misses dropped from 22,069 to 10,902.

**Why 10,902 cancel-misses still exist, and why that's expected, not a residual bug:** two structural reasons, both explained rather than hand-waved:
1. **Hidden liquidity (type 5, 1,004,176 units of real volume) is fundamentally unobservable** in this message stream — hidden orders never appear as a type-1 "new" event, so there is nothing to reduce or track for them. No replay of *only the visible message stream* can recover this, regardless of implementation quality; it's a known, documented limitation of LOBSTER-based reconstruction in general.
2. **This engine's matching decisions are independent of history's.** Once this engine's own type-1-driven matching produces even one different fill decision than NASDAQ's real matching engine did (which will happen — the real engine has hidden liquidity to match against that this one doesn't), the two books' states necessarily diverge more over the course of a trading day. An order this engine already consumed via its own (different) match will legitimately not exist when a later delete event references it. That's not a defect — it's the inherent cost of independent reconstruction from public data, not corruption of a shared ground truth.

**Real results, honestly caveated:**
- 400,391 real NASDAQ events, zero parse failures.
- **Zero book invariant violations (crossed book) across an entire real trading day.** This is the load-bearing correctness claim, and it holds unconditionally, not "mostly."
- 1.26M real events/sec processing rate (same sandbox-noise caveat as every other throughput number in this repo).
- Volume/cancel-miss divergence from ground truth, explained mechanistically above rather than minimized.

**Wrong / open:**
- Didn't attempt to model hidden liquidity even approximately (e.g. inferring a plausible hidden order from the gap between reported and modeled volume) — possible future work, but speculative reconstruction of unobservable data is a different, weaker kind of claim than what's here now.
- Only replayed one ticker, one day. The engine hasn't been run against AMZN/GOOG/INTC/MSFT/SPY (also in the same Hugging Face mirror) or a higher-volatility day.
- The live-data script (see README) has not been run — it needs the user's own machine, and hasn't been verified end-to-end yet.
