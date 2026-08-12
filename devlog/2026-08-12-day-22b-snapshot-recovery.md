## 2026-08-12 — Real snapshot recovery over a real TCP connection

The last real V3 gap ADR-5 identified (as distinct from session
establishment, which isn't a gap): a subscriber that joins the Market Data
Feed mid-stream, or falls too far behind for point-to-point retransmission
to economically cover, has no way to bootstrap current book state — the
existing MoldUDP64 gap recovery only ever recovers individual dropped
packets within a session a subscriber is already participating in.

Built it the way real Nasdaq systems actually do: MoldUDP64/UDP multicast
for the live feed and cheap point-to-point retransmission, a SEPARATE
TCP-based exchange for the snapshot itself (real systems use SoupBinTCP
carrying GLIMPSE for this). The reasoning isn't arbitrary — a full-book
snapshot needs reliable, ordered, arbitrarily-sized delivery, which is
exactly what TCP gives for free and UDP multicast doesn't. Wrote
`cpp/feed/tcp_socket.h` (minimal real `socket()`/`bind()`/`listen()`/
`accept()`/`connect()`/`send()`/`recv()` wrappers, `send_all`/`recv_exact`
looping over TCP's short-write/short-read reality) and
`cpp/feed/snapshot.h`: a `Builder` that maintains full per-order resting
state by applying the same `mdfeed::MDEvent` stream the live feed
publishes, each tagged with its MoldUDP64 sequence number, plus a
fixed-record wire format for a full snapshot (`as_of_seq` + row count +
`{symbol, order_id, side, price, qty}` rows). Explicitly not literal
SoupBinTCP framing (no login sequence, no message-type byte prefix) — same
honesty pattern as the outbound MD feed's own simplified wire format.

The one real code change needed elsewhere: `moldudp64::Session` gained a
`start_seq` constructor parameter (default 1, unchanged for every existing
caller). A subscriber that loaded a snapshot as of sequence N constructs
its session with `start_seq = N + 1`, and the exact same gap-detection
machinery that already existed just... works, for whatever's missing
between the snapshot and the first live packet actually received.

Verification (`cpp/feed/tests/test_snapshot_recovery.cpp`) is the part
worth being precise about, because it would be easy to write a test that
looks like it proves snapshot recovery while actually just replaying
everything from sequence 1 in disguise. This one doesn't: it runs the real
691,421-message ITCH file through the full pipeline, splits the resulting
445,131 market data events exactly at the midpoint, builds a snapshot from
ONLY the first half (3,910 resting orders as of sequence 222,565), serves
it over one real TCP connection, and then publishes ONLY the second half
over real UDP multicast — the first half is never sent over multicast in
this test at all, not even available for a retransmission request to
recover. A few packets in the live second half are deliberately dropped
too, to prove ordinary gap recovery still composes correctly with a
session that didn't start at sequence 1. The subscriber ends up with 0
best-bid/ask mismatches against the true engine's actual final state,
across all 828 real symbols — genuine proof the snapshot carried enough
information to substitute for history the subscriber never saw, not just
that the mechanism runs without crashing. Ran the full test 3 times
consecutively with 0 failures each time (10 assertions/run, real sockets
and real timing, not simulated).

Wired into CMakeLists.txt as `snapshot_recovery`, into `ci.yml`'s ctest
comment, and into `quickstart.sh` as step 10/10 (builds clean, runs in
~5s, table renders real numbers — also caught and fixed two stale
hardcoded pass-count labels, `5/5`/`10/10`, left over from before
yesterday's IOC/FOK/Post-Only unit test additions). Updated README
(Real numbers, Verification, Limitations, Design decisions & tradeoffs —
new row for the snapshot transport choice), ROADMAP.md (V3 session-layer
item and the Production features cross-cutting item both now say "done,"
correctly separated from the still-open max-position-limits gap), and
ADR-5 with a dated update section rather than editing the original
reasoning in place.
