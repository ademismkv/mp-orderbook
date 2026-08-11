#include "itch_simd_neon.h"

#include <arm_neon.h>
#include <cstring>

// Why NEON and not AVX2: the original roadmap item said "AVX2 SIMD
// parser," written without checking the target hardware. This repo's
// real-hardware numbers all come from Apple Silicon (ARM64), and this
// dev sandbox is ARM64 too (`uname -m` = aarch64) — AVX2 is x86-only and
// cannot run on either machine. Writing AVX2 intrinsics here would mean
// shipping code nobody could actually compile or measure, which is
// exactly the "code that appeared, never verified on real hardware"
// failure mode this repo has avoided everywhere else (real profiler data
// for the alloc fix, real interleaved before/after runs for LTO, real
// sockets for the multicast layer). NEON is the real, correct target —
// see devlog for the correction.
//
// What's actually vectorized: byte-swapping big-endian wire fields to
// host order. The scalar decoder's be64()/be32() are an 8- or 4-step
// shift-and-or loop per field, per message. NEON's vrev64q_u8/vrev32q_u8
// reverse bytes within 64-/32-bit lanes of a 128-bit register in one
// instruction — covering 2 messages' order_ref_number (64-bit lanes) or
// 4 messages' shares/price (32-bit lanes) per call, instead of one
// scalar loop per field per message.

namespace itch::simd {

namespace {
inline itch::CommonHeader header_scalar(const uint8_t* p) { return itch::read_common_header(p); }
} // namespace

void decode_add_order_batch(const std::vector<const uint8_t*>& msgs, std::vector<AddOrderNoMPID>& out) {
    out.clear();
    out.reserve(msgs.size());

    const size_t n = msgs.size();
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        const uint8_t* m[4] = {msgs[i + 0], msgs[i + 1], msgs[i + 2], msgs[i + 3]};

        // order_ref_number (offset 11, 8 bytes each): gather 2 messages'
        // raw bytes into one 128-bit register, one vrev64q_u8 call
        // byte-swaps both 64-bit lanes at once. Two calls cover all 4.
        uint8x16_t ref01 = vcombine_u8(vld1_u8(m[0] + 11), vld1_u8(m[1] + 11));
        uint8x16_t ref23 = vcombine_u8(vld1_u8(m[2] + 11), vld1_u8(m[3] + 11));
        uint64x2_t refs01 = vreinterpretq_u64_u8(vrev64q_u8(ref01));
        uint64x2_t refs23 = vreinterpretq_u64_u8(vrev64q_u8(ref23));
        const uint64_t refs[4] = {vgetq_lane_u64(refs01, 0), vgetq_lane_u64(refs01, 1),
                                   vgetq_lane_u64(refs23, 0), vgetq_lane_u64(refs23, 1)};

        // shares (offset 20, 4 bytes each) and price (offset 32, 4 bytes
        // each): gather via memcpy (avoids strict-aliasing UB from
        // reinterpret-casting raw bytes to uint32_t*), one vrev32q_u8
        // call each byte-swaps all 4 messages' lanes at once.
        uint32_t shares_raw[4], price_raw[4];
        for (int k = 0; k < 4; ++k) {
            std::memcpy(&shares_raw[k], m[k] + 20, 4);
            std::memcpy(&price_raw[k], m[k] + 32, 4);
        }
        uint32x4_t shares_v =
            vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(shares_raw))));
        uint32x4_t price_v = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(price_raw))));
        alignas(16) uint32_t shares_arr[4];
        alignas(16) uint32_t price_arr[4];
        vst1q_u32(shares_arr, shares_v);
        vst1q_u32(price_arr, price_v);

        for (int k = 0; k < 4; ++k) {
            AddOrderNoMPID msg{};
            msg.h = header_scalar(m[k]);
            msg.order_ref_number = refs[k];
            msg.buy_sell_indicator = static_cast<char>(m[k][19]);
            msg.shares = shares_arr[k];
            msg.stock = itch::read_alpha<8>(m[k] + 24);
            msg.price = price_arr[k];
            out.push_back(msg);
        }
    }

    // Scalar tail (batch size not a multiple of 4) — reuses the exact
    // same scalar decoder path this is being differentially checked
    // against, so there's nothing new to get wrong here.
    for (; i < n; ++i) {
        auto parsed = itch::parse_message(msgs[i], itch::expected_length('A'));
        if (parsed) out.push_back(std::get<AddOrderNoMPID>(*parsed));
    }
}

} // namespace itch::simd
