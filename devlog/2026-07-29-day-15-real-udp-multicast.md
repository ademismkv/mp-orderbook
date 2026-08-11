## 2026-07-29 — real UDP multicast, not simulated

The MoldUDP64 test from yesterday proved the framing and gap-recovery logic against real ITCH bytes, but everything happened in-process — no socket involved. Today: actual `socket()`/`bind()`/`sendto()`/`recvfrom()`/`setsockopt(IP_ADD_MEMBERSHIP)` calls, over loopback since there's no live exchange feed to subscribe to.

First run failed immediately: `IP_ADD_MEMBERSHIP failed: No such device`. This sandbox has no default route — only `lo`, no gateway — so `INADDR_ANY` gives the kernel nothing to resolve a multicast interface against. Fixed by binding both the sender's `IP_MULTICAST_IF` and the receiver's group-membership interface explicitly to `127.0.0.1` instead of `INADDR_ANY`, forcing loopback rather than asking the kernel to route to it. A real deployment would bind to the actual NIC the same way — this isn't a workaround specific to fake data, it's the same kind of explicit-interface-binding real feed handlers do.

Three real threads: a sender publishing real ITCH-wrapped MoldUDP64 packets over actual multicast, a re-request server answering retransmission requests over a real unicast socket, and the main thread as the actual Feed Handler — joins the group for real, receives real datagrams via `select()` over two live sockets, decodes MoldUDP64 framing, drives the same `Session` state machine from yesterday, and runs every delivered message through the real ITCH parser. One packet withheld from the multicast send to force a real gap over a real network path, recovered through a real retransmission round-trip.

5/5 consecutive runs pass clean — no flakiness once the interface binding was fixed. This closes the loop from "receive UDP multicast" and "handle packet gaps" being separate framing-logic claims to being one real, exercised code path: NIC-equivalent socket → MoldUDP64 → ITCH parser.

Next: the AVX2 SIMD parser, differentially verified against the scalar one the same way v2 was checked against v1.
