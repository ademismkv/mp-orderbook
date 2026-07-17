# data/

`AAPL_2012-06-21_34200000_57600000_message_10.csv` — real NASDAQ order book event data for AAPL, June 21 2012, full trading session (9:30:00.004–15:59:59.913 ET). 400,391 real events (new orders, partial cancels, deletes, visible/hidden executions).

**Source:** [LOBSTER](https://lobsterdata.com/) (Limit Order Book System) sample data, mirrored publicly with no signup required at [huggingface.co/datasets/totalorganfailure/lobster-data](https://huggingface.co/datasets/totalorganfailure/lobster-data). LOBSTER reconstructs order book data from NASDAQ TotalView-ITCH messages; this is their standard free sample tier (also covers AMZN, GOOG, INTC, MSFT, SPY at various depth levels, same source).

**Format:** columns are `time, event_type, order_id, size, price, direction` — see `cpp/tools/replay_lobster.cpp` for the full column/event-type reference and how each is applied to `OrderBookV2`.

**Used by:** `cpp/tools/replay_lobster.cpp` — see the "Real data replay" section in the top-level README.
