#pragma once
#include <cstdint>

enum class Side { Buy, Sell };
enum class Type { Limit, Market, Cancel };

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
