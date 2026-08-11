#include "itch_binaryfile_reader.h"
#include "itch_messages.h"
#include "itch_simd_neon.h"

#include <chrono>
#include <cstdio>
#include <vector>

// Differential test: every real 'A' (Add Order — No MPID) message in the
// sample file is decoded two ways — the scalar path (itch::parse_message,
// already verified in test_itch_parser.cpp) and the NEON batch path
// (itch::simd::decode_add_order_batch) — and every field must match
// exactly. Same discipline as v1-vs-v2 and the alloc fix, applied to a
// new axis (decode strategy) instead of algorithm/data structure.
//
// Also reports real wall-clock throughput for both paths over the same
// real messages, since "SIMD is faster" is a claim to measure, not assert.

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    // Collect every real 'A' message's raw bytes (owned, since the batch
    // decoder needs stable pointers to gather from).
    itch::BinaryFileReader reader(path);
    std::vector<std::vector<uint8_t>> a_messages;
    while (auto raw = reader.next_raw()) {
        if (!raw->empty() && static_cast<char>((*raw)[0]) == 'A') {
            a_messages.push_back(std::move(*raw));
        }
    }
    std::printf("real 'A' messages collected: %zu\n", a_messages.size());
    if (a_messages.empty()) {
        std::printf("FATAL: no 'A' messages found — check the sample file path\n");
        return 1;
    }

    std::vector<const uint8_t*> ptrs;
    ptrs.reserve(a_messages.size());
    for (auto& m : a_messages) ptrs.push_back(m.data());

    // --- correctness: differential check against the scalar decoder ---
    std::vector<itch::AddOrderNoMPID> scalar_out;
    scalar_out.reserve(a_messages.size());
    for (auto& m : a_messages) {
        auto parsed = itch::parse_message(m.data(), m.size());
        scalar_out.push_back(std::get<itch::AddOrderNoMPID>(*parsed));
    }

    std::vector<itch::AddOrderNoMPID> neon_out;
    itch::simd::decode_add_order_batch(ptrs, neon_out);

    int mismatches = 0;
    if (neon_out.size() != scalar_out.size()) {
        std::printf("FAIL: count mismatch scalar=%zu neon=%zu\n", scalar_out.size(), neon_out.size());
        ++mismatches;
    } else {
        for (size_t i = 0; i < scalar_out.size(); ++i) {
            const auto& s = scalar_out[i];
            const auto& v = neon_out[i];
            const bool ok = s.h.stock_locate == v.h.stock_locate && s.h.tracking_number == v.h.tracking_number &&
                             s.h.timestamp_ns == v.h.timestamp_ns && s.order_ref_number == v.order_ref_number &&
                             s.buy_sell_indicator == v.buy_sell_indicator && s.shares == v.shares &&
                             s.stock.raw == v.stock.raw && s.price == v.price;
            if (!ok) {
                ++mismatches;
                if (mismatches <= 5) {
                    std::printf("MISMATCH at #%zu: order_ref %llu vs %llu, shares %u vs %u, price %u vs %u\n", i,
                                (unsigned long long)s.order_ref_number, (unsigned long long)v.order_ref_number,
                                s.shares, v.shares, s.price, v.price);
                }
            }
        }
    }
    std::printf("differential check: %d mismatch%s out of %zu real messages\n", mismatches,
                mismatches == 1 ? "" : "es", scalar_out.size());

    // --- throughput: real wall-clock, both paths, same real messages ---
    // Two scalar baselines, reported separately, because they measure
    // different things and conflating them would be a confound (found by
    // measuring, not assumed): parse_message() builds a std::variant
    // sized for the LARGEST of 23 message types (72 bytes) plus
    // std::optional, even for a 48-byte AddOrderNoMPID — that overhead is
    // real for a caller that dispatches through parse_message() normally,
    // but it's not what the NEON path is actually trying to speed up.
    // decode_add_order_scalar() is the fair apples-to-apples comparison:
    // same struct in, same struct out, only the byte-swap strategy
    // differs.
    constexpr int kReps = 200;   // real set is small (one message type from one 20MB slice); repeat for a stable number

    std::vector<itch::AddOrderNoMPID> scalar_direct_out;
    scalar_direct_out.reserve(a_messages.size());
    const auto d0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) {
        scalar_direct_out.clear();
        for (auto* p : ptrs) scalar_direct_out.push_back(itch::decode_add_order_scalar(p));
    }
    const auto d1 = std::chrono::steady_clock::now();

    std::vector<itch::AddOrderNoMPID> tmp;
    for (int r = 0; r < kReps; ++r) {
        itch::simd::decode_add_order_batch(ptrs, tmp);
    }
    const auto d2 = std::chrono::steady_clock::now();

    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) {
        for (auto& m : a_messages) {
            auto parsed = itch::parse_message(m.data(), m.size());
            if (!parsed) std::abort();   // keep the call from being optimized away without volatile's overhead
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double scalar_direct_s = std::chrono::duration<double>(d1 - d0).count();
    const double neon_s = std::chrono::duration<double>(d2 - d1).count();
    const double scalar_dispatch_s = std::chrono::duration<double>(t1 - t0).count();
    const double total_msgs = static_cast<double>(a_messages.size()) * kReps;

    std::printf("scalar (direct, fair comparison):    %.4fs -> %.2fM msgs/sec\n", scalar_direct_s,
                total_msgs / scalar_direct_s / 1e6);
    std::printf("neon   (batch decode):                %.4fs -> %.2fM msgs/sec\n", neon_s,
                total_msgs / neon_s / 1e6);
    std::printf("scalar (via parse_message dispatch):  %.4fs -> %.2fM msgs/sec\n", scalar_dispatch_s,
                total_msgs / scalar_dispatch_s / 1e6);
    std::printf("speedup vs. direct scalar:   %.2fx\n", scalar_direct_s / neon_s);
    std::printf("speedup vs. dispatch scalar: %.2fx\n", scalar_dispatch_s / neon_s);

    const bool pass = (mismatches == 0);
    std::printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
