#!/usr/bin/env bash
# Real hardware counters for bench_v2 — cycles, instructions, IPC, L1/LLC
# cache misses, branch misses. This answers "where does the 83ns actually
# go" with real numbers instead of guessing from the source.
#
#   cd cpp/bench && ./perf_stat.sh
#
# Linux only. `perf` doesn't exist on macOS — Apple's equivalent hardware
# counter tool is Xcode's Instruments app (the "CPU Counters" template),
# which is GUI-only and doesn't have a simple scriptable CLI form. Two ways
# to actually run this if you're on a Mac:
#
#   1. Xcode Instruments (native, no Docker):
#        xcode-select --install                 # if you don't have it
#        open -a Instruments
#      Then: File > New Trace Document > choose the "CPU Counters" template,
#      point it at the bench_v2 binary built below, hit record. It'll show
#      you cycles/instructions/cache-miss/branch-miss counts per function —
#      slower to set up than this script, but it's the real hardware PMU on
#      your M-series chip, which this script can't reach at all from a
#      shell.
#
#   2. Docker, with a real Linux kernel underneath (perf_events needs kernel
#      support `--privileged` alone doesn't fully guarantee, but is worth
#      trying first since Docker Desktop's VM is usually far less locked
#      down than a shared corporate sandbox):
#        docker build -t mp-orderbook .
#        docker run --rm --privileged mp-orderbook bash -c \
#          "apt-get update -qq && apt-get install -y -qq linux-tools-common linux-tools-generic && cd cpp/bench && ./perf_stat.sh"
#      Even if that installs cleanly, perf inside Docker containers commonly
#      still fails with something like "perf not found for kernel X.X.X" —
#      the installed binary is tied to a specific kernel build, and Docker
#      Desktop's VM kernel usually doesn't match any apt-packaged perf
#      version. If you hit that, it's a real, known Docker+perf limitation,
#      not a bug in this script — Instruments is the reliable fallback.
set -e

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "perf is Linux-only (this is $(uname -s)). See the comment at the top of this" >&2
    echo "script for two ways to get real hardware counters on macOS." >&2
    exit 1
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "perf not found. On Debian/Ubuntu: sudo apt-get install linux-perf (or linux-tools-\$(uname -r))." >&2
    exit 1
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CPP="$ROOT/cpp"
BIN="/tmp/bench_v2_perf"

g++ -std=c++20 -O3 -I"$CPP/include" -I"$CPP/bench" \
    "$CPP/src/order_book_v2.cpp" "$CPP/bench/bench_v2.cpp" -o "$BIN"

echo "Running bench_v2 under perf stat (2,000,000 ops)..."
echo

perf stat -e cycles,instructions,L1-dcache-load-misses,LLC-load-misses,branch-instructions,branch-misses "$BIN"

echo
echo "Reading the output:"
echo "  IPC (instructions/cycles)  — perf prints this directly. Below ~1.0 usually"
echo "    means the CPU is stalled waiting on something (memory, a mispredicted"
echo "    branch), not short on raw compute."
echo "  L1-dcache-load-misses      — how often a memory access misses L1 and has to"
echo "    go further out. High relative to instructions = the unordered_map's"
echo "    pointer-chasing and hashing showing up for real, not just in theory."
echo "  LLC-load-misses            — misses that went all the way to main memory."
echo "    These are the expensive ones (hundreds of cycles each) and are the most"
echo "    likely single cause of the p99.9 tail, if there's a real culprit."
echo "  branch-misses / branch-instructions — misprediction rate. The matching loop"
echo "    has real data-dependent branches (side, crossing-or-not, qty==0); a high"
echo "    rate here is what 'eliminate branches' in the optimization list is"
echo "    actually about — not a stylistic preference, a real measured cost."
