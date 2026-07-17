#include "order_book_v2.h"
#include "workload.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 42;
    const int count = argc > 2 ? std::atoi(argv[2]) : 20000;
    auto ops = generate_ops(seed, count);

    OrderBookV2 book;
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
    return 0;
}
