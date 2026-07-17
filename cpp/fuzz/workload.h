#pragma once
#include <cstdint>
#include <random>
#include <vector>

// Shared, engine-agnostic op generator so v1 and v2 fuzz binaries see
// byte-identical operation sequences for a given seed. Deliberately has no
// dependency on either OrderBook's types (v1 and v2 both define `Side`,
// `Type` etc. at global scope with the same names, so they can't be
// included in the same translation unit — differential fuzzing runs them
// as two separate binaries and diffs the printed summaries instead).

enum class OpKind : uint8_t { AddLimit, AddMarket, Cancel };

struct Op {
    OpKind   kind;
    uint64_t id;      // new order id for Add*, id to cancel for Cancel
    bool     is_buy;
    int64_t  price;   // ticks; meaningful for AddLimit only
    uint64_t qty;
};

inline std::vector<Op> generate_ops(uint64_t seed, int count) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_offset(-50, 50);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> action_dist(0, 99);  // 0-84 add-limit, 85-94 add-market, 95-99 cancel

    std::vector<Op> ops;
    ops.reserve(static_cast<size_t>(count));
    std::vector<uint64_t> live_ids;
    uint64_t next_id = 1;

    for (int i = 0; i < count; ++i) {
        const int action = action_dist(rng);
        if (action < 95 || live_ids.empty()) {
            Op op;
            op.kind = (action < 85) ? OpKind::AddLimit : OpKind::AddMarket;
            op.id = next_id++;
            op.is_buy = side_dist(rng) != 0;
            op.price = 10000 + price_offset(rng);
            op.qty = static_cast<uint64_t>(qty_dist(rng));
            ops.push_back(op);
            live_ids.push_back(op.id);
        } else {
            std::uniform_int_distribution<size_t> pick(0, live_ids.size() - 1);
            const size_t idx = pick(rng);
            Op op;
            op.kind = OpKind::Cancel;
            op.id = live_ids[idx];
            op.is_buy = false;
            op.price = 0;
            op.qty = 0;
            ops.push_back(op);
            live_ids.erase(live_ids.begin() + static_cast<long>(idx));
        }
    }
    return ops;
}

// Order-dependent FNV-1a style checksum over the trade stream. Two engines
// that produce the same trades in the same order will produce the same
// checksum; any divergence in matching behavior changes it.
struct Checksum {
    uint64_t h = 1469598103934665603ull;  // FNV offset basis
    void mix(uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;  // FNV prime
    }
};
