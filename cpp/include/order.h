#pragma once
#include <cstdint>

enum class Side { Buy, Sell };
// IOC (Immediate-Or-Cancel): match what's immediately available at arrival,
// discard any unfilled remainder — never rests. FOK (Fill-Or-Kill):
// all-or-nothing — the full requested quantity must be fillable or nothing
// trades at all, verified with a non-mutating pre-check before committing
// any trades. PostOnly: must never take liquidity — rejected outright if it
// would cross on arrival, otherwise rests directly (never calls match()).
enum class Type { Limit, Market, Cancel, IOC, FOK, PostOnly };

struct Order {
    uint64_t id;
    Side     side;
    Type     type;
    double   price;     // ignored for Market
    uint64_t qty;
    uint64_t timestamp_ns;
};

struct Trade {
    uint64_t maker_id;
    uint64_t taker_id;
    double   price;     // always the MAKER's price
    uint64_t qty;
};
