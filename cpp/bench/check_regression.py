#!/usr/bin/env python3
"""Compare a bench_v2 run against cpp/bench/baseline.json and fail on a real
regression. Run as:  ./bench_v2 | python3 check_regression.py

Why a gate exists at all: ci.yml previously ran bench_v2 as "informational
only" (comment still in ci.yml explaining why: shared CI runners are noisy).
That's true, but "informational only" also means a genuine performance
regression — e.g. reintroducing an allocation on the hot path, or a change
that silently drops -march=native/LTO — could land and nobody would notice
until someone happened to eyeball a number by hand. This script exists to
turn that from "nobody's watching" into "watched, with thresholds wide
enough to survive real observed noise instead of crying wolf every run".

Why the tolerances are this wide: measured 5 consecutive bench_v2 runs on
this repo's own dev sandbox (see baseline.json's comment) and saw throughput
swing 6.34-7.79M ops/sec and p99 swing 209-250ns from noise alone, on
hardware this repo actually controls. A shared GitHub Actions runner is
almost certainly noisier. A tight gate here wouldn't catch regressions
faster, it would just fail intermittently on noise until someone disables
it out of frustration — worse than no gate at all. These thresholds are
picked to survive that noise while still catching an actual multi-x
regression, which is the class of bug this is actually meant to catch.

Only throughput and p99 are gated. p50 and p999 are printed for visibility
but not gated: p50 sits at 42-83ns in this repo's own measurements, where a
single-digit-nanosecond difference from scheduler jitter is a huge
percentage swing — not a meaningful signal. `max` is not even printed here;
bench_v2's own output showed it ranging from ~84,000ns to ~3,574,000ns
across 5 runs on idle hardware, purely from OS scheduling — noise, not
signal, at any threshold.
"""
import argparse
import json
import re
import sys
from pathlib import Path

BASELINE_PATH = Path(__file__).parent / "baseline.json"

# Wide on purpose — see module docstring.
THROUGHPUT_FLOOR_FRACTION = 0.55   # fail if throughput drops below 55% of baseline (a ~45% regression)
P99_CEILING_MULTIPLE = 2.5         # fail if p99 exceeds 2.5x baseline

LINE_RE = re.compile(
    r"throughput=\s*([0-9.]+)M ops/sec\s+p50=\s*(\d+)ns\s+p99=\s*(\d+)ns\s+p999=\s*(\d+)ns"
)


def parse_bench_output(text: str):
    m = LINE_RE.search(text)
    if not m:
        print("FAIL: could not find a bench_v2 result line in the given output", file=sys.stderr)
        print("--- input was ---", file=sys.stderr)
        print(text, file=sys.stderr)
        sys.exit(2)
    return {
        "throughput_M_ops_sec": float(m.group(1)),
        "p50_ns": int(m.group(2)),
        "p99_ns": int(m.group(3)),
        "p999_ns": int(m.group(4)),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update-baseline", action="store_true",
                     help="Overwrite baseline.json with this run's numbers instead of checking against it. "
                          "Deliberate, human-run, and reviewed in the resulting diff — never called by CI.")
    ap.add_argument("input", nargs="?", type=argparse.FileType("r"), default=sys.stdin)
    args = ap.parse_args()

    text = args.input.read()
    measured = parse_bench_output(text)

    if args.update_baseline:
        baseline = json.loads(BASELINE_PATH.read_text())
        baseline["throughput_M_ops_sec"] = measured["throughput_M_ops_sec"]
        baseline["p99_ns"] = measured["p99_ns"]
        BASELINE_PATH.write_text(json.dumps(baseline, indent=2) + "\n")
        print(f"Updated {BASELINE_PATH} -> throughput={measured['throughput_M_ops_sec']}M p99={measured['p99_ns']}ns")
        print("Review the diff before committing — this should be a deliberate, explained change, not a habit.")
        return

    baseline = json.loads(BASELINE_PATH.read_text())
    base_tput = baseline["throughput_M_ops_sec"]
    base_p99 = baseline["p99_ns"]

    tput_floor = base_tput * THROUGHPUT_FLOOR_FRACTION
    p99_ceiling = base_p99 * P99_CEILING_MULTIPLE

    tput_ok = measured["throughput_M_ops_sec"] >= tput_floor
    p99_ok = measured["p99_ns"] <= p99_ceiling

    print("=== bench_v2 regression check ===")
    print(f"throughput: {measured['throughput_M_ops_sec']:.3f}M ops/sec  "
          f"(baseline {base_tput}M, floor {tput_floor:.3f}M)  {'ok' if tput_ok else 'REGRESSION'}")
    print(f"p99:        {measured['p99_ns']}ns  "
          f"(baseline {base_p99}ns, ceiling {p99_ceiling:.0f}ns)  {'ok' if p99_ok else 'REGRESSION'}")
    print(f"p50 (informational, not gated — see script docstring): {measured['p50_ns']}ns")
    print(f"p999 (informational, not gated): {measured['p999_ns']}ns")

    if not (tput_ok and p99_ok):
        print("\nFAIL: performance regression past the noise-tolerant threshold.")
        print("If this is a real, understood tradeoff (not noise), update the baseline deliberately:")
        print("  ./bench_v2 | python3 cpp/bench/check_regression.py --update-baseline")
        sys.exit(1)

    print("\nPASS: within noise-tolerant thresholds of baseline.")


if __name__ == "__main__":
    main()
