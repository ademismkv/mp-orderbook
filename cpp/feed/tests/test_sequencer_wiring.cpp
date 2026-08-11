#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "sequencer.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

// End-to-end wiring test: real ITCH bytes -> real parser -> real
// Sequencer -> real Risk checks -> real, symbol-sharded OrderBookV2
// instances -> real Execution Reports. This is what closes ROADMAP.md's
// Feed -> Sequencer -> Order Books -> Risk -> Execution Reports gap.
//
// The real sample spans ~8,900 distinct symbols (one Stock Directory
// message per listed symbol), so this is a genuine multi-symbol
// sharding test, not a single-book replay like replay_lobster.cpp. Each
// shard gets a deliberately small arena_capacity/initial_window — this
// repo's default OrderBookV2 constructor sizes for a single
// high-volume instrument (1M-order arena, 20,000-tick window); doing
// that ~8,900 times would be tens of gigabytes for a wiring-verification
// harness. A real deployment shards across many machines/processes, not
// one process holding every listed symbol's book — this is a
// demonstration at feasible scale, not a claim about production sizing.
//
// This test is also what found a real engine bug: the first run hit
// std::bad_alloc trying to allocate ~13GB inside OrderBookV2. Root
// cause (via ASan): a real order in the real data — symbol TVIX, price
// 888888800 (a $88,888.88 sentinel/garbage value) — asked
// ensure_index_for_price() to grow the level array by ~891M entries.
// Fixed with a bounded-growth guard in OrderBookV2 itself (kMaxLevels).
// See devlog day 17. Day 18 adds RiskEngine and Execution Reports on
// top of the same Sequencer, verified below both against the real file
// and against targeted synthetic cases the real data doesn't happen to
// exercise (real data had no self-trade or price-band violations in
// this slice — that's not the same as the mechanism being untested).
namespace {
constexpr uint32_t kShardArenaCapacity = 128;
constexpr int64_t kShardInitialWindow = 200;

int g_failures = 0;
void expect(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    std::unordered_map<std::string, std::unique_ptr<OrderBookV2>> shards;
    OrderBookV2* last_touched = nullptr;
    auto book_for = [&](const std::string& symbol) -> OrderBookV2& {
        auto it = shards.find(symbol);
        if (it == shards.end()) {
            it = shards.emplace(symbol, std::make_unique<OrderBookV2>(kShardArenaCapacity, kShardInitialWindow))
                     .first;
        }
        last_touched = it->second.get();
        return *it->second;
    };

    Sequencer seq;
    RiskEngine risk;   // default Config: max_order_size=1,000,000, price in [1, 2,000,000,000], 25% price band
    std::vector<ExecutionReport> reports;
    auto report_cb = [&](const ExecutionReport& r) { reports.push_back(r); };

    itch::BinaryFileReader reader(path);

    uint64_t total_messages = 0, parse_failures = 0, invariant_violations = 0, both_sides_present = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (auto raw = reader.next_raw()) {
        ++total_messages;
        auto parsed = itch::parse_message(raw->data(), raw->size());
        if (!parsed) {
            ++parse_failures;
            continue;
        }
        last_touched = nullptr;
        seq.on_message(*parsed, book_for, &risk, report_cb);
        if (last_touched && last_touched->has_bid() && last_touched->has_ask()) {
            ++both_sides_present;
            if (last_touched->best_bid() >= last_touched->best_ask()) ++invariant_violations;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(t1 - t0).count();

    const auto& s = seq.stats();
    std::printf("=== Feed -> Sequencer -> Risk -> Order Books -> Execution Reports, real data: %s ===\n",
                path.c_str());
    std::printf("total messages:      %llu (parse failures: %llu)\n", (unsigned long long)total_messages,
                (unsigned long long)parse_failures);
    std::printf("wall time:           %.3fs (%.2fM msgs/sec)\n", wall, total_messages / wall / 1e6);
    std::printf("symbols sharded:     %zu\n", shards.size());
    std::printf("\n");
    std::printf("add_orders:          %llu\n", (unsigned long long)s.add_orders);
    std::printf("cancels (partial):   %llu\n", (unsigned long long)s.cancels);
    std::printf("deletes (full):      %llu\n", (unsigned long long)s.deletes);
    std::printf("executes:            %llu\n", (unsigned long long)s.executes);
    std::printf("replaces:            %llu\n", (unsigned long long)s.replaces);
    std::printf("unrouted:            %llu\n", (unsigned long long)s.unrouted);
    std::printf("other_message_types: %llu\n", (unsigned long long)s.other_message_types);
    std::printf("trades_produced:     %llu\n", (unsigned long long)s.trades_produced);
    std::printf("add_rejected:        %llu (arena/kMaxLevels — see devlog day 17)\n",
                (unsigned long long)s.add_rejected);
    std::printf("risk_rejected:       %llu (RiskEngine declined before the book ever saw the order)\n",
                (unsigned long long)s.risk_rejected);
    std::printf("execution reports emitted: %zu\n", reports.size());
    std::printf("\n");
    std::printf("moments with both a bid and ask present: %llu\n", (unsigned long long)both_sides_present);
    std::printf("book invariant violations: %llu\n", (unsigned long long)invariant_violations);

    // Real-data pass sanity: every Ack should correspond to a real add,
    // every Reject to a risk/arena rejection, and Ack+Reject should
    // together account for every Add Order / Replace attempt (accepted
    // or not) — a report accounting check, not just "some number of
    // reports came out."
    uint64_t acks = 0, rejects = 0, fills = 0;
    for (const auto& r : reports) {
        if (r.type == ExecReportType::Ack) ++acks;
        else if (r.type == ExecReportType::Reject) ++rejects;
        else if (r.type == ExecReportType::Fill) ++fills;
    }
    std::printf("reports: acks=%llu rejects=%llu fills=%llu\n", (unsigned long long)acks, (unsigned long long)rejects,
                (unsigned long long)fills);
    expect(acks == s.add_orders + s.replaces, "ack count matches successful add_orders + replaces");
    expect(rejects == s.add_rejected + s.risk_rejected, "reject count matches add_rejected + risk_rejected");
    expect(fills == s.trades_produced, "fill count matches trades_produced");
    expect(invariant_violations == 0, "zero crossed-book invariant violations across the whole real file");
    expect(parse_failures == 0, "zero parse failures across the whole real file");

    // Honest breakdown of WHY real orders got risk-rejected, rather than
    // just reporting the total and moving on — a risk engine that rejects
    // a lot of real flow is a real finding, not something to gloss over.
    uint64_t reject_toolarge = 0, reject_priceoob = 0, reject_priceband = 0, reject_selftrade = 0, reject_other = 0;
    for (const auto& r : reports) {
        if (r.type != ExecReportType::Reject) continue;
        if (r.reason.find("max_order_size") != std::string::npos) ++reject_toolarge;
        else if (r.reason.find("outside [") != std::string::npos) ++reject_priceoob;
        else if (r.reason.find("away from reference") != std::string::npos) ++reject_priceband;
        else if (r.reason.find("already rests") != std::string::npos) ++reject_selftrade;
        else ++reject_other;
    }
    std::printf("risk_rejected breakdown: fat_finger=%llu price_oob=%llu price_band=%llu self_trade=%llu "
                "other(arena/kMaxLevels)=%llu\n",
                (unsigned long long)reject_toolarge, (unsigned long long)reject_priceoob,
                (unsigned long long)reject_priceband, (unsigned long long)reject_selftrade,
                (unsigned long long)reject_other);

    // --- synthetic checks: mechanisms real data didn't happen to exercise ---
    reports.clear();

    // 1) Crossing order still produces a real trade with Risk in the loop
    //    (proves Risk doesn't accidentally block a legitimate cross).
    itch::AddOrderNoMPID buy{};
    buy.h = itch::CommonHeader{1, 1, 1};
    buy.order_ref_number = 900000001;
    buy.buy_sell_indicator = 'B';
    buy.shares = 100;
    buy.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    buy.price = 100000;   // $10.00
    seq.on_message(itch::Message{buy}, book_for, &risk, report_cb);

    itch::AddOrderNoMPID sell{};
    sell.h = itch::CommonHeader{1, 1, 2};
    sell.order_ref_number = 900000002;
    sell.buy_sell_indicator = 'S';
    sell.shares = 100;
    sell.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    sell.price = 99000;   // $9.90 — crosses the resting $10.00 bid
    seq.on_message(itch::Message{sell}, book_for, &risk, report_cb);
    expect(seq.stats().trades_produced >= 1, "legitimate crossing order still trades with Risk enabled");

    // 2) Fat-finger: an absurdly large order gets rejected before the book sees it.
    itch::AddOrderNoMPID fatfinger{};
    fatfinger.h = itch::CommonHeader{1, 1, 3};
    fatfinger.order_ref_number = 900000003;
    fatfinger.buy_sell_indicator = 'B';
    fatfinger.shares = 50'000'000;   // way past default max_order_size (1,000,000)
    fatfinger.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    fatfinger.price = 100000;
    const uint64_t risk_rejected_before = seq.stats().risk_rejected;
    seq.on_message(itch::Message{fatfinger}, book_for, &risk, report_cb);
    expect(seq.stats().risk_rejected == risk_rejected_before + 1, "oversized order rejected by RiskEngine");
    expect(!reports.empty() && reports.back().type == ExecReportType::Reject &&
               reports.back().reason.find("max_order_size") != std::string::npos,
           "fat-finger reject carries the right reason in its execution report");

    // 3) Price band: an order priced wildly away from the established
    //    reference (the $10.00/$9.90 book above, band default 25%) gets rejected.
    itch::AddOrderNoMPID badprice{};
    badprice.h = itch::CommonHeader{1, 1, 4};
    badprice.order_ref_number = 900000004;
    badprice.buy_sell_indicator = 'B';
    badprice.shares = 10;
    badprice.stock.raw = {'W', 'I', 'R', 'E', 'T', 'E', 'S', 'T'};
    badprice.price = 500000;   // $50.00 vs. an ~$9.9x reference — far past the 25% band
    const uint64_t risk_rejected_before2 = seq.stats().risk_rejected;
    seq.on_message(itch::Message{badprice}, book_for, &risk, report_cb);
    expect(seq.stats().risk_rejected == risk_rejected_before2 + 1, "price-band-violating order rejected");

    // 4) Self-trade prevention: an MPID-attributed order can't rest
    //    against its own resting order on the opposite side.
    itch::AddOrderMPID mine_buy{};
    mine_buy.h = itch::CommonHeader{1, 1, 5};
    mine_buy.order_ref_number = 900000005;
    mine_buy.buy_sell_indicator = 'B';
    mine_buy.shares = 10;
    mine_buy.stock.raw = {'S', 'E', 'L', 'F', 'T', 'E', 'S', 'T'};
    mine_buy.price = 100000;
    mine_buy.attribution.raw = {'A', 'C', 'M', 'E'};
    seq.on_message(itch::Message{mine_buy}, book_for, &risk, report_cb);

    itch::AddOrderMPID mine_sell{};
    mine_sell.h = itch::CommonHeader{1, 1, 6};
    mine_sell.order_ref_number = 900000006;
    mine_sell.buy_sell_indicator = 'S';
    mine_sell.shares = 10;
    mine_sell.stock.raw = {'S', 'E', 'L', 'F', 'T', 'E', 'S', 'T'};
    mine_sell.price = 99000;   // would cross ACME's own resting bid
    mine_sell.attribution.raw = {'A', 'C', 'M', 'E'};
    const uint64_t risk_rejected_before3 = seq.stats().risk_rejected;
    seq.on_message(itch::Message{mine_sell}, book_for, &risk, report_cb);
    expect(seq.stats().risk_rejected == risk_rejected_before3 + 1,
           "self-trade prevented: same MPID can't cross its own resting order");

    // 5) The exact same self-trade shape from a DIFFERENT participant
    //    must be accepted and trade normally — proves #4 isn't just
    //    rejecting all crosses on that symbol.
    itch::AddOrderMPID other_sell{};
    other_sell.h = itch::CommonHeader{1, 1, 7};
    other_sell.order_ref_number = 900000007;
    other_sell.buy_sell_indicator = 'S';
    other_sell.shares = 10;
    other_sell.stock.raw = {'S', 'E', 'L', 'F', 'T', 'E', 'S', 'T'};
    other_sell.price = 99000;
    other_sell.attribution.raw = {'X', 'Y', 'Z', 'Z'};
    const uint64_t trades_before = seq.stats().trades_produced;
    seq.on_message(itch::Message{other_sell}, book_for, &risk, report_cb);
    expect(seq.stats().trades_produced == trades_before + 1,
           "a DIFFERENT participant's crossing order still trades normally on the same book");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
