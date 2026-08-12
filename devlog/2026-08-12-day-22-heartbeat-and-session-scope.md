## 2026-08-12 — Heartbeat-during-idle over real sockets, and an honest session-establishment scope call

Two more V3 gaps from the "how done are you" review closed together, since
the second is really a clarification of the first.

**Heartbeat-during-idle, live.** `moldudp64_session.h`'s `Session` already
handled heartbeats correctly (`on_packet` checks `hdr.message_count ==
kHeartbeat` and calls `maybe_request_gap_` even when no message blocks are
present), and `test_moldudp64.cpp` already covered an on-time heartbeat
in-process. What was missing: a heartbeat actually detecting a real gap,
sent over a real socket, during a genuinely idle period — the actual reason
the mechanism exists (moldudp64.h's own header comment: "heartbeats carry
the next-expected sequence number even during idle periods so loss can be
detected without waiting for the next real message"). Wrote
`cpp/feed/tests/test_udp_multicast_heartbeat.cpp`: a sender publishes one
real packet (seq 1-2) over real UDP multicast, then withholds the next
packet (seq 3-4) — the drop — and goes idle, emitting nothing but real
heartbeat packets carrying `next_expected=5` (the exchange's true position,
since it already assigned seq 3-4 even though that packet never went out).
The first such heartbeat, and nothing else, is what triggers gap detection
and a real Request Packet over a real unicast socket; a re-request server
thread answers it; recovery flushes seq 3-4 in order; a later matching
heartbeat and a real end-of-session packet close out clean. Ran 4 times in
a row, 0 failures each time (9 assertions/run) — real sockets and real
timing, not simulated.

**Session establishment: not actually a gap.** ROADMAP.md's V3 section had
listed "heartbeats, session establishment, snapshot recovery" together
under "session layer," which reads like three equally-missing pieces. They
aren't. MoldUDP64 is connectionless and publish-only by design — there is
no login/handshake in the real protocol, full stop; any subscriber that
knows the multicast group and port can join. The protocol that has a real
login/session handshake is SoupBinTCP, Nasdaq's separate order-entry
protocol — not something a market-data pipeline like this one ever needs,
since it never submits orders to a real exchange. Building a fake login
sequence on top of MoldUDP64 to satisfy a literal reading of "session
establishment" would have added code that doesn't correspond to anything a
real MoldUDP64 consumer does. Wrote this up as ADR-5 rather than just
fixing it silently — worth having a documented reason on record for why
this item is "not applicable" rather than "not done." Updated
ROADMAP.md's V3 section and README's Limitations to say the same thing in
fewer words, and to correctly separate it from snapshot recovery, which
*is* a real, still-open gap (a subscriber joining mid-stream or falling too
far behind has no way to bootstrap today — next up).

Verified before syncing: the new heartbeat test builds clean and passes
repeatably; `quickstart.sh`'s hardcoded pass-count labels (`5/5` / `10/10`)
were stale from before yesterday's IOC/FOK/Post-Only unit tests were added
— fixed to `10/10` / `16/16` and reran the full script end to end to
confirm the table renders correctly with real numbers, not hardcoded ones.
