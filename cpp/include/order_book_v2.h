#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// v2 — array-of-price-levels (shared price axis) + intrusive doubly-linked
// list per level + arena allocator for order nodes. Replaces v1's
// std::map<double, std::deque<Order>>. See ADR-2 in
// 04-PROJECTS/2026-07-17-matching-engine-architecture-decisions.md.
//
// Prices are integer ticks (not double) — avoids float-compare bugs and
// matches how real order books represent price.

enum class Side { Buy, Sell };
enum class Type { Limit, Market, Cancel };

using OrderId  = uint64_t;
using Price    = int64_t;
using Quantity = uint64_t;

struct OrderRequest {
    OrderId  id;
    Side     side;
    Type     type;
    Price    price;   // ignored for Market
    Quantity qty;
};

struct Trade {
    OrderId  maker_id;
    OrderId  taker_id;
    Price    price;   // always the MAKER's price
    Quantity qty;
};

class OrderBookV2 {
public:
    // arena_capacity: max resting orders alive at once before growth throws.
    // initial_window: how many price ticks the level array starts with,
    // centered on the first order's price. Both are sized generously for a
    // benchmark/demo; a production version would tune these per instrument.
    explicit OrderBookV2(uint32_t arena_capacity = 1u << 20, Price initial_window = 20000);

    std::vector<Trade> add(const OrderRequest& req);
    bool cancel(OrderId id);

    // Partial cancellation: reduce a resting order's quantity by `delta`
    // without removing it from its place in the FIFO queue (price-time
    // priority is unaffected — this is NOT the same as cancel+re-add, which
    // would lose queue position). If delta >= the order's current quantity,
    // it's removed entirely (same effect as cancel()). Added specifically
    // to replay real exchange data faithfully — real venues report partial
    // cancels as their own event type (e.g. LOBSTER type 2), distinct from
    // a full delete (type 3), and a resting order that's been trimmed keeps
    // its original time priority. Returns false if id isn't resting.
    bool reduce(OrderId id, Quantity delta);

    Price  best_bid() const { return best_bid_idx_ >= 0 ? base_ + best_bid_idx_ : 0; }
    Price  best_ask() const { return best_ask_idx_ >= 0 ? base_ + best_ask_idx_ : 0; }
    bool   has_bid() const { return best_bid_idx_ >= 0; }
    bool   has_ask() const { return best_ask_idx_ >= 0; }
    size_t depth() const { return index_.size(); }

private:
    static constexpr uint32_t kNil = UINT32_MAX;

    struct Node {                    // intrusive list node, lives in the arena
        OrderId  id = 0;
        Quantity qty = 0;
        uint32_t prev = kNil;
        uint32_t next = kNil;
    };
    struct PriceLevel {
        uint32_t head = kNil;
        uint32_t tail = kNil;
        Quantity total_qty = 0;
    };
    struct IndexEntry {
        Side     side;
        int64_t  level_idx;   // index into levels_
        uint32_t node_idx;    // index into arena_
    };

    // arena
    std::unique_ptr<Node[]> arena_;
    uint32_t arena_capacity_;
    uint32_t arena_bump_ = 0;
    std::vector<uint32_t> free_list_;
    uint32_t alloc_node();
    void     free_node(uint32_t idx);

    // shared price axis: levels_[i] corresponds to price (base_ + i)
    std::vector<PriceLevel> levels_;
    Price base_ = 0;
    bool  have_base_ = false;
    Price initial_window_;

    int64_t best_bid_idx_ = -1;   // -1 = no resting bids
    int64_t best_ask_idx_ = -1;   // -1 = no resting asks

    std::unordered_map<OrderId, IndexEntry> index_;

    int64_t ensure_index_for_price(Price p);   // may rebase/grow levels_
    void    push_back_order(int64_t level_idx, Side side, const OrderRequest& req);
    std::vector<Trade> match(OrderRequest& taker);
};
