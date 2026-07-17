#include "order_book_v2.h"
#include <algorithm>
#include <stdexcept>

OrderBookV2::OrderBookV2(uint32_t arena_capacity, Price initial_window)
    : arena_(std::make_unique<Node[]>(arena_capacity)),
      arena_capacity_(arena_capacity),
      initial_window_(initial_window) {
    // Reserve index_ up front so it doesn't rehash repeatedly while growing
    // to hundreds of thousands of entries during a benchmark/production run
    // — a rehash storm on the id->location map is a real, measurable cost
    // on the add() hot path, unlike blind cache-line padding (see devlog).
    index_.reserve(arena_capacity);
}

uint32_t OrderBookV2::alloc_node() {
    if (!free_list_.empty()) {
        uint32_t idx = free_list_.back();
        free_list_.pop_back();
        return idx;
    }
    if (arena_bump_ >= arena_capacity_) {
        throw std::runtime_error("OrderBookV2 arena exhausted - increase arena_capacity");
    }
    return arena_bump_++;
}

void OrderBookV2::free_node(uint32_t idx) {
    free_list_.push_back(idx);
}

int64_t OrderBookV2::ensure_index_for_price(Price p) {
    if (!have_base_) {
        base_ = p - initial_window_ / 2;
        levels_.assign(static_cast<size_t>(initial_window_), PriceLevel{});
        have_base_ = true;
    }

    int64_t idx = p - base_;
    if (idx < 0) {
        // Rebase: grow at the front, shift everything (including open
        // orders' cached level_idx) by `shift`. Rare in practice — only
        // triggered when price moves outside the current window — but O(n)
        // in open orders when it does. See ADR-2's noted tradeoff.
        int64_t shift = -idx + (initial_window_ / 4) + 1;
        std::vector<PriceLevel> grown(levels_.size() + static_cast<size_t>(shift));
        for (size_t i = 0; i < levels_.size(); ++i) {
            grown[i + static_cast<size_t>(shift)] = levels_[i];
        }
        levels_ = std::move(grown);
        base_ -= shift;
        if (best_bid_idx_ >= 0) best_bid_idx_ += shift;
        if (best_ask_idx_ >= 0) best_ask_idx_ += shift;
        for (auto& kv : index_) kv.second.level_idx += shift;
        idx = p - base_;
    } else if (idx >= static_cast<int64_t>(levels_.size())) {
        levels_.resize(static_cast<size_t>(idx) + static_cast<size_t>(initial_window_ / 4) + 1);
    }
    return idx;
}

void OrderBookV2::push_back_order(int64_t level_idx, Side side, const OrderRequest& req) {
    uint32_t node_idx = alloc_node();
    Node& n = arena_[node_idx];
    n.id = req.id;
    n.qty = req.qty;
    n.prev = kNil;
    n.next = kNil;

    PriceLevel& lvl = levels_[static_cast<size_t>(level_idx)];
    if (lvl.tail == kNil) {
        lvl.head = lvl.tail = node_idx;
    } else {
        arena_[lvl.tail].next = node_idx;
        n.prev = lvl.tail;
        lvl.tail = node_idx;
    }
    lvl.total_qty += req.qty;
    index_[req.id] = IndexEntry{side, level_idx, node_idx};
}

std::vector<Trade> OrderBookV2::match(OrderRequest& taker) {
    std::vector<Trade> trades;
    const bool is_buy = (taker.side == Side::Buy);

    while (taker.qty > 0) {
        int64_t& best_idx = is_buy ? best_ask_idx_ : best_bid_idx_;
        if (best_idx < 0) break;  // nothing to match against on this side

        const Price level_price = base_ + best_idx;
        if (taker.type == Type::Limit) {
            if (is_buy && taker.price < level_price) break;   // bid below ask -> no cross
            if (!is_buy && taker.price > level_price) break;  // ask above bid -> no cross
        }

        PriceLevel& lvl = levels_[static_cast<size_t>(best_idx)];
        while (taker.qty > 0 && lvl.head != kNil) {
            Node& maker = arena_[lvl.head];
            Quantity fill = std::min(taker.qty, maker.qty);

            trades.push_back({maker.id, taker.id, level_price, fill});

            taker.qty -= fill;
            maker.qty -= fill;
            lvl.total_qty -= fill;

            if (maker.qty == 0) {
                uint32_t done = lvl.head;
                lvl.head = maker.next;
                if (lvl.head != kNil) arena_[lvl.head].prev = kNil;
                else lvl.tail = kNil;
                index_.erase(maker.id);
                free_node(done);
            }
        }

        if (lvl.head == kNil) {
            // level just emptied — scan for the next-best level in the
            // correct direction. Amortized O(1) for a dense book; worst
            // case O(window) for a sparse one (documented tradeoff, ADR-2).
            if (is_buy) {
                int64_t i = best_ask_idx_ + 1;
                while (i < static_cast<int64_t>(levels_.size()) && levels_[static_cast<size_t>(i)].head == kNil) ++i;
                best_ask_idx_ = (i < static_cast<int64_t>(levels_.size())) ? i : -1;
            } else {
                int64_t i = best_bid_idx_ - 1;
                while (i >= 0 && levels_[static_cast<size_t>(i)].head == kNil) --i;
                best_bid_idx_ = (i >= 0) ? i : -1;
            }
        }
    }
    return trades;
}

std::vector<Trade> OrderBookV2::add(const OrderRequest& req) {
    if (req.type == Type::Cancel) {
        cancel(req.id);
        return {};
    }

    OrderRequest taker = req;
    auto trades = match(taker);

    if (taker.type == Type::Limit && taker.qty > 0) {
        int64_t idx = ensure_index_for_price(taker.price);
        push_back_order(idx, taker.side, taker);
        if (taker.side == Side::Buy) {
            if (best_bid_idx_ < 0 || idx > best_bid_idx_) best_bid_idx_ = idx;
        } else {
            if (best_ask_idx_ < 0 || idx < best_ask_idx_) best_ask_idx_ = idx;
        }
    }
    return trades;
}

bool OrderBookV2::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Side side = it->second.side;
    const int64_t level_idx = it->second.level_idx;
    const uint32_t node_idx = it->second.node_idx;

    PriceLevel& lvl = levels_[static_cast<size_t>(level_idx)];
    Node& n = arena_[node_idx];

    if (n.prev != kNil) arena_[n.prev].next = n.next; else lvl.head = n.next;
    if (n.next != kNil) arena_[n.next].prev = n.prev; else lvl.tail = n.prev;
    lvl.total_qty -= n.qty;

    free_node(node_idx);
    index_.erase(it);

    if (lvl.head == kNil) {
        if (side == Side::Buy && level_idx == best_bid_idx_) {
            int64_t i = best_bid_idx_ - 1;
            while (i >= 0 && levels_[static_cast<size_t>(i)].head == kNil) --i;
            best_bid_idx_ = (i >= 0) ? i : -1;
        } else if (side == Side::Sell && level_idx == best_ask_idx_) {
            int64_t i = best_ask_idx_ + 1;
            while (i < static_cast<int64_t>(levels_.size()) && levels_[static_cast<size_t>(i)].head == kNil) ++i;
            best_ask_idx_ = (i < static_cast<int64_t>(levels_.size())) ? i : -1;
        }
    }
    return true;
}

bool OrderBookV2::reduce(OrderId id, Quantity delta) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const uint32_t node_idx = it->second.node_idx;
    Node& n = arena_[node_idx];

    if (delta >= n.qty) {
        // Reduces to zero or past it — same as a full cancel. Real venues
        // do send a delete in this situation rather than a reduce-to-zero,
        // but handling it here too makes this robust to replaying data
        // that doesn't perfectly follow that convention.
        return cancel(id);
    }

    const int64_t level_idx = it->second.level_idx;
    PriceLevel& lvl = levels_[static_cast<size_t>(level_idx)];

    n.qty -= delta;
    lvl.total_qty -= delta;
    // Deliberately NOT touching prev/next or head/tail — the order keeps
    // its exact place in the FIFO queue. Losing time priority on a partial
    // cancel would be a real correctness bug if this book were used for
    // actual trading, not just a replay demo.
    return true;
}
