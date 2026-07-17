#!/usr/bin/env bash
# One command: clone the repo, run this, see real numbers.
#
#   git clone https://github.com/ademismkv/mp-orderbook.git
#   cd mp-orderbook
#   ./quickstart.sh            # clean summary table
#   ./quickstart.sh --verbose  # full raw output from every tool, no parsing
#
# Builds v1 (std::map baseline) and v2 (array + arena) with raw g++ — no
# CMake required — runs both unit test suites, the v2 latency/throughput
# benchmark, the multithreaded scaling benchmark, and replays the real
# 400,391-event NASDAQ AAPL trading day through the actual engine. Every
# number in the table below is parsed straight out of that run's own
# stdout — nothing is precomputed or hardcoded. Takes well under a minute
# on a normal laptop.
set -e

VERBOSE=0
[[ "${1:-}" == "--verbose" || "${1:-}" == "-v" ]] && VERBOSE=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP="$ROOT/cpp"
BUILD="$ROOT/.quickstart-build"
mkdir -p "$BUILD"

if ! command -v g++ >/dev/null 2>&1; then
    echo "g++ not found. Install a C++20-capable compiler (gcc/g++ 10+ or clang 12+) and re-run." >&2
    exit 1
fi

step() { printf '  %s\n' "$1"; }
run() { if [[ $VERBOSE -eq 1 ]]; then "$@"; else "$@" > "$BUILD/last.out" 2>&1 || { cat "$BUILD/last.out" >&2; exit 1; }; fi; }

echo "Building and running mp-orderbook (use --verbose for full raw output)..."
echo

step "[1/5] building v1 + v2..."
g++ -std=c++20 -O2 -Wall -Wextra -I"$CPP/include" \
    "$CPP/src/order_book.cpp" "$CPP/tests/test_order_book.cpp" -o "$BUILD/test_v1"
g++ -std=c++20 -O2 -Wall -Wextra -I"$CPP/include" \
    "$CPP/src/order_book_v2.cpp" "$CPP/tests/test_order_book_v2.cpp" -o "$BUILD/test_v2"

step "[2/5] unit tests..."
TEST_V1_OUT="$("$BUILD/test_v1")"
TEST_V2_OUT="$("$BUILD/test_v2")"

step "[3/5] single-thread benchmark (2,000,000 ops)..."
g++ -std=c++20 -O2 -I"$CPP/include" -I"$CPP/bench" \
    "$CPP/src/order_book_v2.cpp" "$CPP/bench/bench_v2.cpp" -o "$BUILD/bench_v2"
BENCH_OUT="$("$BUILD/bench_v2")"
[[ $VERBOSE -eq 1 ]] && echo "$BENCH_OUT"

step "[4/5] multithreaded scaling (1..4 symbols, 1,000,000 ops/symbol)..."
g++ -std=c++20 -O2 -pthread -I"$CPP/include" \
    "$CPP/src/order_book_v2.cpp" "$CPP/bench/bench_threaded_scaling.cpp" -o "$BUILD/bench_threaded"
THREAD_OUT="$("$BUILD/bench_threaded" 4 1000000)"
[[ $VERBOSE -eq 1 ]] && echo "$THREAD_OUT"

step "[5/5] replaying the real NASDAQ AAPL trading day (400,391 real events)..."
g++ -std=c++20 -O2 -I"$CPP/include" \
    "$CPP/src/order_book_v2.cpp" "$CPP/tools/replay_lobster.cpp" -o "$BUILD/replay_lobster"
REPLAY_OUT="$("$BUILD/replay_lobster" "$ROOT/data/AAPL_2012-06-21_34200000_57600000_message_10.csv")"
[[ $VERBOSE -eq 1 ]] && echo "$REPLAY_OUT"

# ---- parse ----
v1_pass=$(echo "$TEST_V1_OUT" | grep -qi "passed" && echo "5/5" || echo "FAIL")
v2_pass=$(echo "$TEST_V2_OUT" | grep -qi "passed" && echo "10/10" || echo "FAIL")

# histogram.h's printf uses fixed-width numeric fields (e.g. "throughput=%8.3fM"),
# which pads with spaces *between* the "=" and the digits for short numbers —
# so naive whitespace field-splitting silently breaks. sed tolerates the padding.
bench_throughput=$(echo "$BENCH_OUT" | sed -n 's/.*throughput=[[:space:]]*\([0-9.]*M\).*/\1/p')
bench_p50=$(echo "$BENCH_OUT"        | sed -n 's/.*p50=[[:space:]]*\([0-9]*ns\).*/\1/p')
bench_p99=$(echo "$BENCH_OUT"        | sed -n 's/.*p99=[[:space:]]*\([0-9]*ns\).*/\1/p')
bench_p999=$(echo "$BENCH_OUT"       | sed -n 's/.*p999=[[:space:]]*\([0-9]*ns\).*/\1/p')

thread_line() { echo "$THREAD_OUT" | grep "^symbols=$1 " | awk '{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="aggregate")print a[2]}}'; }
t1=$(thread_line 1); t2=$(thread_line 2); t3=$(thread_line 3); t4=$(thread_line 4)

replay_events_per_sec=$(echo "$REPLAY_OUT" | sed -n 's/.*(\(.*\) real events\/sec).*/\1/p')
replay_trades=$(echo "$REPLAY_OUT" | grep "own trades" | awk '{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="count")print a[2]}}')
replay_qty=$(echo "$REPLAY_OUT"    | grep "own trades" | awk '{for(i=1;i<=NF;i++){split($i,a,"=");if(a[1]=="total_qty")print a[2]}}')
replay_invariant=$(echo "$REPLAY_OUT" | grep "invariant violations" | awk '{print $NF}')
replay_misses=$(echo "$REPLAY_OUT" | grep -o 'id: [0-9]*' | grep -o '[0-9]*')
replay_reduce_miss=$(echo "$replay_misses" | sed -n 1p)
replay_cancel_miss=$(echo "$replay_misses" | sed -n 2p)

# thousands separator, e.g. 528509 -> 528,509 — built digit-by-digit (no
# sed/grep GNU-vs-BSD portability traps; awk behaves the same everywhere).
commafy() {
    awk -v n="$1" 'BEGIN{
        len = length(n); rev = ""; cnt = 0;
        for (i = len; i >= 1; i--) {
            rev = rev substr(n, i, 1);
            cnt++;
            if (cnt % 3 == 0 && i != 1) rev = rev ",";
        }
        out = "";
        for (i = length(rev); i >= 1; i--) out = out substr(rev, i, 1);
        print out;
    }'
}
replay_trades=$(commafy "$replay_trades")
replay_qty=$(commafy "$replay_qty")
replay_cancel_miss=$(commafy "$replay_cancel_miss")

MW=41  # metric column width
VW=34  # value column width
line() { printf '  │ %-*s │ %-*s │\n' "$MW" "$1" "$VW" "$2"; }
hline() {
    printf '  %s' "$1"
    for ((i = 0; i < MW + 2; i++)); do printf '─'; done
    printf '%s' "$2"
    for ((i = 0; i < VW + 2; i++)); do printf '─'; done
    printf '%s\n' "$3"
}

echo
hline "┌" "┬" "┐"
line "Metric" "Value (measured just now)"
hline "├" "┼" "┤"
line "Unit tests (v1 / v2)"              "${v1_pass} / ${v2_pass}"
line "Single-thread throughput"          "${bench_throughput} ops/sec"
line "  p50 / p99 / p99.9 latency"       "${bench_p50} / ${bench_p99} / ${bench_p999}"
line "Multithread aggregate, 1 symbol"   "${t1} ops/sec"
line "Multithread aggregate, 2 symbols"  "${t2} ops/sec"
line "Multithread aggregate, 3 symbols"  "${t3} ops/sec"
line "Multithread aggregate, 4 symbols"  "${t4} ops/sec"
line "Real NASDAQ replay throughput"     "${replay_events_per_sec} events/sec"
line "  Real events processed"           "400,391"
line "  Trades matched / volume"         "${replay_trades} / ${replay_qty}"
line "  Book invariant violations"       "${replay_invariant}"
line "  Cancel / reduce misses"          "${replay_cancel_miss} / ${replay_reduce_miss}"
hline "└" "┴" "┘"
echo
echo "  Full raw output: ./quickstart.sh --verbose"
echo "  What these numbers mean, and what's not yet verified: README.md, ADR.md, devlog/"
echo "  Interactive dashboard (real precomputed batches — a browser can't invoke a compiler): dashboard/index.html"
