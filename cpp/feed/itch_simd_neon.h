#pragma once
#include "itch_messages.h"
#include <vector>

// NEON-accelerated batch decoding for the ITCH 5.0 parser — see
// itch_simd_neon.cpp for the implementation and the file-header comment
// there for why NEON, not AVX2 (this repo's real hardware, including this
// dev sandbox, is ARM64; AVX2 is x86-only and would ship unverified).

namespace itch::simd {

// Batch-decodes Add Order — No MPID Attribution ('A', 36 bytes) messages
// — the single most common real ITCH message type (26.6% of messages in
// this repo's real Nasdaq sample, see data/itch50_sample_20191230.bin).
// `msgs` must all point to complete 36-byte 'A' messages; a real feed
// handler would already have classified messages by type before this
// call (dispatch-by-type is not SIMD-specific overhead).
void decode_add_order_batch(const std::vector<const uint8_t*>& msgs, std::vector<AddOrderNoMPID>& out);

} // namespace itch::simd
