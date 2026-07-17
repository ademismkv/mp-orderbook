#!/usr/bin/env python3
"""
Captures genuinely live order book data from Coinbase's public WebSocket
feed (no API key needed) and writes it to a CSV in a format close enough to
LOBSTER's that cpp/tools/replay_lobster.cpp only needs minor adaptation.

WHY THIS RUNS ON YOUR MACHINE AND NOT INSIDE THE AI SESSION:
Live data means a persistent connection held open while updates stream in.
The dev sandbox this repo was built in can only make one-shot HTTP
request/response calls with no persistent connection support, and its
network access is allowlisted (blocked most external hosts, including this
one). Your machine has neither restriction — this is real, not a
workaround-around-a-limitation script, it's the normal way to do this.

WHY COINBASE AND NOT NASDAQ/A REAL EQUITIES VENUE:
Real exchange live feeds (NASDAQ TotalView-ITCH, CME MDP, etc.) are paid,
licensed, and typically require a broker relationship or market data
subscription costing real money — not something to route around. Major
crypto exchanges, in contrast, commonly expose full L2 (and sometimes L3)
order book updates over public WebSocket with zero authentication, which is
why crypto is the standard choice for free real-time order book data in
student/portfolio HFT projects. This isn't a downgrade from "real" data —
it's real live order book data, just for a different asset class than the
NASDAQ replay in data/.

USAGE:
    pip install websockets
    python3 capture_live_orderbook.py --product BTC-USD --seconds 60 --out live_capture.csv

Then adapt cpp/tools/replay_lobster.cpp's parser (or write a sibling
replay_live.cpp) to read live_capture.csv — the column meanings are
documented below and are deliberately close to LOBSTER's so most of the
existing replay logic carries over.
"""
import argparse
import asyncio
import csv
import json
import sys
import time

try:
    import websockets
except ImportError:
    print("Missing dependency. Run: pip install websockets", file=sys.stderr)
    sys.exit(1)

COINBASE_WS = "wss://ws-feed.exchange.coinbase.com"


async def capture(product: str, seconds: float, out_path: str) -> None:
    subscribe_msg = {
        "type": "subscribe",
        "product_ids": [product],
        "channels": ["level2", "matches"],
    }

    rows_written = 0
    t_start = time.time()

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        # Columns chosen to mirror LOBSTER's message file shape:
        # time, event_type, order_id_or_price_level, size, price, direction
        # event_type here: "l2update" (book delta) or "match" (a trade)
        writer.writerow(["time", "event_type", "side", "price", "size", "seq"])

        async with websockets.connect(COINBASE_WS) as ws:
            await ws.send(json.dumps(subscribe_msg))
            print(f"Subscribed to {product} on Coinbase's public feed. Capturing for {seconds:.0f}s...")

            while time.time() - t_start < seconds:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=max(1.0, seconds - (time.time() - t_start)))
                except asyncio.TimeoutError:
                    break

                msg = json.loads(raw)
                msg_type = msg.get("type")
                now = time.time()

                if msg_type == "l2update":
                    # changes: [[side, price, size], ...] — size "0" means the level was removed
                    for side, price, size in msg.get("changes", []):
                        writer.writerow([f"{now:.6f}", "l2update", side, price, size, msg.get("sequence", "")])
                        rows_written += 1
                elif msg_type == "match":
                    writer.writerow([
                        f"{now:.6f}", "match", msg.get("side", ""),
                        msg.get("price", ""), msg.get("size", ""), msg.get("sequence", ""),
                    ])
                    rows_written += 1
                elif msg_type == "snapshot":
                    # Initial full book snapshot — record best few levels only
                    # to keep the file small; full snapshot is huge and the
                    # l2updates from here on are what matters for replay.
                    for side_key, side_label in (("bids", "buy"), ("asks", "sell")):
                        for price, size in msg.get(side_key, [])[:50]:
                            writer.writerow([f"{now:.6f}", "snapshot", side_label, price, size, ""])
                            rows_written += 1
                # ignore heartbeats / subscription acks / errors silently — this is a capture script, not a monitor

    elapsed = time.time() - t_start
    print(f"Done. {rows_written} rows written to {out_path} over {elapsed:.1f}s "
          f"({rows_written / max(elapsed, 0.001):.1f} events/sec).")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product", default="BTC-USD", help="Coinbase product id, e.g. BTC-USD, ETH-USD")
    ap.add_argument("--seconds", type=float, default=60.0, help="how long to capture")
    ap.add_argument("--out", default="live_capture.csv", help="output CSV path")
    args = ap.parse_args()

    asyncio.run(capture(args.product, args.seconds, args.out))


if __name__ == "__main__":
    main()
