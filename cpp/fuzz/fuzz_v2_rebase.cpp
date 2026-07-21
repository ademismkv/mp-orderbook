#include "order_book_v2.h"
#include "workload.h"
#include <cstdio>
#include <cstdlib>

// Deliberately-narrow initial_window (see main) so the price random walk in
// generate_ops_price_walk (workload.h) forces real, repeated
// ensure_index_for_price rebases over the course of a run — the standard
// fuzz_v2.cpp never exercises that path at all (fixed +-50 tick band, any
// seed/count). This is the only place in the whole test suite that stresses
// the specific intersection FlatHashMap::for_each_mut() only runs under:
// a rebase happening while the index has a live mix of occupied/deleted
// slots from backward-shift-deletion churn (heavy cancel traffic here, same
// as the standard fuzz workload).
int main(int argc, char** argv) {
    const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 42;
    const int count = argc > 2 ? std::atoi(argv[2]) : 20000;
    auto ops = generate_ops_price_walk(seed, count);

    // Narrow window (200 ticks) against a walk that can drift thousands of
    // ticks over a long run - guarantees many rebases, not just one.
    OrderBookV2 book(1u << 20, /*initial_window=*/200);
    Checksum cs;
    uint64_t total_trades = 0, total_trade_qty = 0;

    for (auto& op : ops) {
        if (op.kind == OpKind::Cancel) {
            book.cancel(op.id);
            continue;
        }
        OrderRequest o;
        o.id = op.id;
        o.side = op.is_buy ? Side::Buy : Side::Sell;
        o.type = (op.kind == OpKind::AddLimit) ? Type::Limit : Type::Market;
        o.price = op.price;
        o.qty = op.qty;

        auto trades = book.add(o);
        for (auto& t : trades) {
            total_trades++;
            total_trade_qty += t.qty;
            cs.mix(t.maker_id);
            cs.mix(t.taker_id);
            cs.mix(static_cast<uint64_t>(t.price));
            cs.mix(t.qty);
        }
    }

    std::printf("trades=%llu trade_qty=%llu checksum=%llu depth=%zu best_bid=%lld best_ask=%lld\n",
        (unsigned long long)total_trades, (unsigned long long)total_trade_qty,
        (unsigned long long)cs.h, book.depth(),
        (long long)book.best_bid(), (long long)book.best_ask());
    std::fprintf(stderr, "levels_ growth/rebase events: %llu\n",
        (unsigned long long)book.level_array_growths());
    return 0;
}
