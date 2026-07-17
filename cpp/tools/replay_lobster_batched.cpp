// Same replay logic as replay_lobster.cpp (see that file for the full
// event-type reasoning), but reports per-batch wall-clock timing instead of
// one aggregate number. Built specifically so the dashboard could be driven
// by real C++-measured numbers over the FULL real trading day, instead of a
// small JS reimplementation over a small embedded sample.
//
// Usage: replay_lobster_batched <csv path> <batch size> > out.csv
// Output columns: batch_idx,batch_events,batch_wall_ns,cum_events,
//                  cum_trades,cum_trade_qty,cum_invariant_violations,
//                  cum_cancel_misses,cum_reduce_misses,best_bid,best_ask,depth
//
// Every number in the output is measured by actually running this engine's
// real add()/reduce()/cancel() over real NASDAQ order events — nothing here
// is synthesized or interpolated.

#include "order_book_v2.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

struct LobsterEvent {
    double time;
    int type;
    uint64_t order_id;
    uint64_t size;
    int64_t price;
    int direction;
};

static bool parse_line(const std::string& line, LobsterEvent& ev) {
    std::stringstream ss(line);
    std::string field;
    if (!std::getline(ss, field, ',')) return false;
    ev.time = std::strtod(field.c_str(), nullptr);
    if (!std::getline(ss, field, ',')) return false;
    ev.type = std::atoi(field.c_str());
    if (!std::getline(ss, field, ',')) return false;
    ev.order_id = std::strtoull(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.size = std::strtoull(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.price = std::strtoll(field.c_str(), nullptr, 10);
    if (!std::getline(ss, field, ',')) return false;
    ev.direction = std::atoi(field.c_str());
    return true;
}

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/AAPL_2012-06-21_34200000_57600000_message_10.csv";
    const uint64_t batch_size = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10000;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::fprintf(stderr, "could not open %s\n", path.c_str());
        return 1;
    }

    OrderBookV2 book(1u << 21, /*initial_window=*/150000);

    uint64_t cum_events = 0, batch_events = 0;
    uint64_t cum_trades = 0, cum_trade_qty = 0;
    uint64_t cum_invariant_violations = 0;
    uint64_t cum_reduce_misses = 0, cum_cancel_misses = 0;
    uint64_t batch_idx = 0;

    std::printf("batch_idx,batch_events,batch_wall_ns,cum_events,cum_trades,cum_trade_qty,cum_invariant_violations,cum_cancel_misses,cum_reduce_misses,best_bid,best_ask,depth\n");

    std::string line;
    auto batch_t0 = std::chrono::steady_clock::now();

    auto flush_batch = [&]() {
        auto batch_t1 = std::chrono::steady_clock::now();
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(batch_t1 - batch_t0).count();
        std::printf("%llu,%llu,%lld,%llu,%llu,%llu,%llu,%llu,%llu,%lld,%lld,%zu\n",
            (unsigned long long)batch_idx, (unsigned long long)batch_events, ns,
            (unsigned long long)cum_events, (unsigned long long)cum_trades, (unsigned long long)cum_trade_qty,
            (unsigned long long)cum_invariant_violations, (unsigned long long)cum_cancel_misses, (unsigned long long)cum_reduce_misses,
            (long long)book.best_bid(), (long long)book.best_ask(), book.depth());
        batch_idx++;
        batch_events = 0;
        batch_t0 = std::chrono::steady_clock::now();
    };

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        LobsterEvent ev;
        if (!parse_line(line, ev)) continue;

        cum_events++;
        batch_events++;
        const Side side = (ev.direction == 1) ? Side::Buy : Side::Sell;

        switch (ev.type) {
            case 1: {
                OrderRequest req{ev.order_id, side, Type::Limit, ev.price, ev.size};
                for (auto& t : book.add(req)) {
                    cum_trades++;
                    cum_trade_qty += t.qty;
                }
                break;
            }
            case 2:
                if (!book.reduce(ev.order_id, ev.size)) cum_reduce_misses++;
                break;
            case 3:
                if (!book.cancel(ev.order_id)) cum_cancel_misses++;
                break;
            case 4:
                book.reduce(ev.order_id, ev.size);
                break;
            case 5:
            case 7:
            default:
                break;
        }

        if (book.has_bid() && book.has_ask() && book.best_bid() >= book.best_ask()) {
            cum_invariant_violations++;
        }

        if (batch_events >= batch_size) {
            flush_batch();
        }
    }
    if (batch_events > 0) {
        flush_batch();
    }

    return 0;
}
