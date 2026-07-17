#include "order_book.h"
#include <algorithm>

std::vector<Trade> OrderBook::add(const Order& o) {
    if (o.type == Type::Cancel) {
        cancel(o.id);
        return {};
    }

    Order taker = o;
    auto trades = match(taker);  // taker passed by ref, taker.qty now reflects fills

    // If limit order still has qty left, rest on the book
    if (taker.type == Type::Limit && taker.qty > 0) {
        if (taker.side == Side::Buy) {
            bids_[taker.price].push_back(taker);
        } else {
            asks_[taker.price].push_back(taker);
        }
        index_[taker.id] = { taker.side, taker.price };
    }
    return trades;
}

namespace {
// bids_ and asks_ are different std::map instantiations (bids_ uses
// std::greater<double> so begin() = best bid). A single templated helper
// avoids duplicating the match loop for each map type.
template <typename Map>
std::vector<Trade> match_against(
    Map& opp, Order& taker, bool taker_is_buy,
    std::unordered_map<uint64_t, std::pair<Side, double>>& index) {
    std::vector<Trade> trades;
    while (taker.qty > 0 && !opp.empty()) {
        auto it = opp.begin();                 // best opposite level
        const double best_opp = it->first;

        // Price check — does the taker cross?
        if (taker.type == Type::Limit) {
            if (taker_is_buy && taker.price < best_opp) break;  // bid below ask -> no cross
            if (!taker_is_buy && taker.price > best_opp) break; // ask above bid -> no cross
        }

        auto& queue = it->second;
        while (taker.qty > 0 && !queue.empty()) {
            Order& maker = queue.front();
            uint64_t fill = std::min(taker.qty, maker.qty);

            trades.push_back({ maker.id, taker.id, best_opp, fill });

            taker.qty -= fill;
            maker.qty -= fill;
            if (maker.qty == 0) {
                index.erase(maker.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) opp.erase(it);
    }
    return trades;
}
}  // namespace

std::vector<Trade> OrderBook::match(Order& taker) {
    const bool taker_is_buy = (taker.side == Side::Buy);
    if (taker_is_buy) {
        return match_against(asks_, taker, taker_is_buy, index_);
    }
    return match_against(bids_, taker, taker_is_buy, index_);
}

bool OrderBook::cancel(uint64_t id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    auto [side, price] = it->second;

    auto do_cancel = [&](auto& book) {
        auto lvl = book.find(price);
        if (lvl == book.end()) return false;
        auto& q = lvl->second;
        q.erase(std::remove_if(q.begin(), q.end(),
                [id](const Order& o) { return o.id == id; }), q.end());
        if (q.empty()) book.erase(lvl);
        return true;
    };

    bool found = (side == Side::Buy) ? do_cancel(bids_) : do_cancel(asks_);
    if (!found) return false;
    index_.erase(it);
    return true;
}

double OrderBook::best_bid() const {
    return bids_.empty() ? 0.0 : bids_.begin()->first;
}
double OrderBook::best_ask() const {
    return asks_.empty() ? 0.0 : asks_.begin()->first;
}
size_t OrderBook::depth() const {
    size_t n = 0;
    for (auto& [p, q] : bids_) n += q.size();
    for (auto& [p, q] : asks_) n += q.size();
    return n;
}
