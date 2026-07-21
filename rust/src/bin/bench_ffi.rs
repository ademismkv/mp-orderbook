//! Standalone benchmark: measures the real cost of Rust -> `cxx` -> C++ for
//! every `add()` call, using the same workload shape as
//! cpp/bench/bench_v2.cpp (mixed buy/sell around a fixed mid price, same
//! price/qty ranges, same order count) so the two numbers are directly
//! comparable — this is "pure C++" vs "Rust calling C++ through the FFI
//! boundary, once per order."
//!
//! Run in release mode — a debug build inflates per-call overhead with
//! optimizer noise that has nothing to do with the FFI boundary itself:
//!
//!   cargo run --release --bin bench_ffi
//!
//! Why this matters: ADR-3 deliberately keeps the hot data-plane path (every
//! order, every trade) off this boundary — cxx is used for the low-frequency
//! control plane only, with a hand-rolled SPSC ring buffer for the
//! high-frequency path instead. This benchmark is what justifies that
//! decision with a real number instead of an assumption: it shows what
//! calling straight through cxx per-order actually costs, so "the ring
//! buffer avoids this" is a measured tradeoff, not a guess.
//!
//! Not bit-identical to bench_v2.cpp's workload: this uses a small
//! dependency-free xorshift64* PRNG (no external crate — same
//! zero-dependency spirit as cpp/bench/histogram.h) instead of C++'s
//! mt19937_64, so the exact trade sequence differs. The ranges and mix are
//! the same, which is what makes the throughput/latency numbers comparable;
//! the final depth/trade count will differ slightly from the C++ run, and
//! that's expected, not a bug.

use sidecar::{MatchingEngine, OrderRequest, OrderType, Side};
use std::time::Instant;

struct Xorshift64 {
    state: u64,
}

impl Xorshift64 {
    fn new(seed: u64) -> Self {
        Self {
            state: if seed == 0 { 1 } else { seed },
        }
    }

    fn next_u64(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.state = x;
        x
    }

    // Inclusive [lo, hi].
    fn range(&mut self, lo: i64, hi: i64) -> i64 {
        let span = (hi - lo + 1) as u64;
        lo + (self.next_u64() % span) as i64
    }
}

fn percentile(sorted_ns: &[u64], p: f64) -> u64 {
    let idx = (p * (sorted_ns.len() - 1) as f64) as usize;
    sorted_ns[idx]
}

fn main() {
    const N: usize = 2_000_000;

    // Same defaults as OrderBookV2's C++ constructor (order_book_v2.h):
    // arena_capacity = 1<<20, initial_window = 20000.
    let mut eng = MatchingEngine::new(1u32 << 20, 20_000);
    let mut rng = Xorshift64::new(1234);

    let mut latencies_ns: Vec<u64> = Vec::with_capacity(N);
    let mut next_id: u64 = 1;

    let t0 = Instant::now();
    for _ in 0..N {
        let side = if rng.range(0, 1) == 0 {
            Side::Buy
        } else {
            Side::Sell
        };
        let price = 10_000 + rng.range(-50, 50);
        let qty = rng.range(1, 100) as u64;

        let req = OrderRequest {
            id: next_id,
            side,
            kind: OrderType::Limit,
            price,
            qty,
        };
        next_id += 1;

        let s = Instant::now();
        let _trades = eng.add(req);
        let e = Instant::now();
        latencies_ns.push(e.duration_since(s).as_nanos() as u64);
    }
    let wall = t0.elapsed().as_secs_f64();

    let mut sorted = latencies_ns.clone();
    sorted.sort_unstable();
    let throughput = N as f64 / wall;

    println!(
        "ffi(add)   ops={:<8} wall={:.3}s  throughput={:8.3}M ops/sec  p50={:6}ns  p99={:7}ns  p999={:8}ns  max={:8}ns",
        N,
        wall,
        throughput / 1e6,
        percentile(&sorted, 0.50),
        percentile(&sorted, 0.99),
        percentile(&sorted, 0.999),
        sorted[sorted.len() - 1],
    );
    println!("final depth={}", eng.depth());
}
