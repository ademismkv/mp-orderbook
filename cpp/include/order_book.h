#pragma once
#include <map>
#include <deque>
#include <vector>
#include <unordered_map>
#include "order.h"

// v1 baseline — correctness over speed. See
// 04-PROJECTS/2026-07-17-matching-engine-architecture-decisions.md (ADR-2)
// in the notes vault for the planned v2 container swap
// (array of price levels + intrusive list + arena allocator).
class OrderBook {
public:
    // Add an order, return list of trades it generated
    std::vector<Trade> add(const Order& o);

    // Cancel a resting order by id; returns true if found
    bool cancel(uint64_t id);

    // Inspect the book
    double best_bid() const;
    double best_ask() const;
    size_t depth() const;  // total resting orders

private:
    // Bids sorted DESC (best = highest). Asks sorted ASC (best = lowest).
    std::map<double, std::deque<Order>, std::greater<double>> bids_;
    std::map<double, std::deque<Order>>                       asks_;

    // id -> (side, price) so cancel can find the right level in O(log n)
    std::unordered_map<uint64_t, std::pair<Side, double>>     index_;

    std::vector<Trade> match(Order& taker);  // tries to fill; mutates taker.qty in place (by ref, see devlog 2026-07-17)
};
