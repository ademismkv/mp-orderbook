## 2026-08-12 — ADR-1's threading model, proven against the real ITCH pipeline

Last V3 gap from the "how done are you" review: ADR-1's single-writer-per-symbol
threading claim was only ever demonstrated by `bench_threaded_scaling.cpp` — a
synthetic benchmark generating its own ops. The real Sequencer/Risk/Order-Books
pipeline has run single-threaded, sequentially, this whole time. ADR-1's own
Status line said so explicitly: "not yet tested... a persistent multi-symbol
engine process (current test is a synthetic benchmark, not the real entry
point)."

The design that closes it, without touching or risking the existing,
already-verified `Sequencer`/`RiskEngine` classes at all: a single router
thread parses the real 691,421-message ITCH file exactly as today's
sequential path does, but instead of calling `Sequencer::on_message()`
directly, it works out which symbol each message belongs to and pushes the
raw `itch::Message` into one of 4 `SpscRingBuffer<itch::Message>`s (the
same ring buffer type ADR-1 already built, reused unmodified) keyed by
`hash(symbol) % 4`. Four worker threads each drain their own ring and call
the real, completely unmodified `Sequencer::on_message()` — no matching or
risk logic was reimplemented or shortcut for this test.

The one real design question was whether this is actually safe, and the
answer came from reading the code, not assuming it. `Sequencer` needs its
own `order_ref_number -> symbol` table because post-Add ITCH messages
don't repeat the symbol — so the router needs the same lookup, just a
lighter version of it (symbol only, no side/mpid) purely to decide which
shard's queue to push into. Whether `RiskEngine` could safely run as 4
independent instances instead of 1 shared one came down to actually
reading `risk.h`: every piece of its internal state is keyed by symbol
(`last_trade_price_[symbol]`, and a composite key starting with `symbol`
for self-trade tracking) — there is no cross-symbol interaction anywhere
in that class. That's not a coincidence; it's what "risk is a per-symbol
concern in this design" (ADR-1's own framing) actually looks like when you
go check it against the real class instead of trusting the architecture
diagram.

Verification (`test_concurrent_sharded_pipeline.cpp`) runs the real file
twice: once sequentially (the reference — same behavior
`test_sequencer_wiring.cpp` already established), once through the 4-shard
concurrent path, then requires exact agreement on every field of
`Sequencer::Stats` (add_orders, cancels, deletes, executes, replaces,
risk_rejected, etc.), the total market data event count, and — the real
proof — 0 best-bid/ask mismatches across all 828 real symbols. First run:
clean pass, no debugging needed, which is itself informative — the
"symbol-scoped state has no cross-shard interaction" reasoning above held
up in practice, not just on paper. Ran 4 times consecutively, 0 failures
each time. Both passes together take under 2 seconds.

Ran it under ThreadSanitizer too, same discipline as everywhere else
concurrent in this repo — and hit a real, honest limit of this specific
sandbox: TSan's 5-10x memory overhead, stacked on top of a test that
already runs the real pipeline twice across 5 threads, OOM-killed the
process past roughly 260,000 real messages in this repo's own 3.8GB dev
environment. Rather than skip TSan entirely or silently claim full
coverage, added an optional message-count cap (2nd CLI arg) and found the
actual largest value that runs clean here: 250,000 messages, 651 real
symbols, 4,281 real add orders, 0 races, 0 mismatches. Documented both the
cap and the reasoning in README's Limitations and in `ci.yml`'s comment —
GitHub Actions runners have more headroom than this sandbox, but rather
than guess a bigger number would work there too, the CI job uses the same
250,000 figure actually verified locally.

Updated ADR-1's Status section in place with a dated "Update, day 22c"
paragraph (not rewriting the original text) documenting all of the above.
Updated ROADMAP.md's Concurrency model section, README's Threading
design-decision row, Verification section, Real numbers section, and added
a Limitations bullet about the fixed 4-shard count and the TSan message
cap. Wired into CMakeLists.txt (`concurrent_sharded_pipeline` ctest, full
real file) and `ci.yml`'s TSan job (capped at 250,000 messages, matching
what was actually verified). Added as quickstart.sh's step 11/11 — runs in
under 2 seconds, so it doesn't meaningfully change the script's total
runtime — and softened the script's own timing claim ("well under a
minute" -> "under a minute for steps 1-9; can reach ~1 minute total with
steps 10-11's real socket I/O") after actually timing a full run instead
of leaving an older, now-optimistic claim in place.

This closes every V3 gap raised by the earlier honest assessment: order
types beyond Limit (day 21), the session layer (heartbeats day 21,
session-establishment scope clarified day 22, snapshot recovery day 22b),
and now concurrent execution on the real pipeline (day 22c).
