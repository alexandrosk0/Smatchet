#!/usr/bin/env python3
# scripts/dev/perf-median.py — pick the median perf snapshot from N runs.
#
# Companion to scripts/dev/perf-run.sh in CI: the perf gate runs each scenario
# multiple times (after a discarded warmup) and gates on the MEDIAN run, so a
# single scheduler hiccup on a shared runner can't fail the absolute p99/max
# ceilings (perf-gate-revival § Flake mitigations; first false positive observed
# 2026-06-07: SmatchetUI::Draw p99 17.354 ms on an otherwise-clean run).
#
# Median selection key: the umbrella scope's p99Ms (the metric the Pillar-1
# floor gates on), falling back to lastTotalMs when p99 is absent, falling back
# to the max lastTotalMs across rows when the umbrella scope is missing.
# Pure stdlib. Prints the chosen file path on the LAST line (caller contract
# identical to perf-run.sh).
#
# Usage: python scripts/dev/perf-median.py <snapshot1.json> <snapshot2.json> ...
# Exit:  0 + path on last line; 2 on unusable input.

from __future__ import annotations

import json
import sys

UMBRELLA_SCOPE = "SmatchetUI::Draw"


def rows_of(blob):
    if isinstance(blob, dict):
        data = blob.get("data")
        if isinstance(data, dict) and "rows" in data:
            return data.get("rows") or []
        if "rows" in blob:
            return blob.get("rows") or []
    return []


def key_metric(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            blob = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print("perf-median: unreadable snapshot %s: %s" % (path, exc), file=sys.stderr)
        return None
    rows = rows_of(blob)
    if not rows:
        print("perf-median: no rows in %s" % path, file=sys.stderr)
        return None
    for r in rows:
        if r.get("name") == UMBRELLA_SCOPE:
            v = r.get("p99Ms")
            if v is None:
                v = r.get("lastTotalMs")
            if v is not None:
                return float(v)
    # Umbrella scope missing — fall back to the worst row by lastTotalMs.
    vals = [float(r["lastTotalMs"]) for r in rows if r.get("lastTotalMs") is not None]
    return max(vals) if vals else None


def main(argv):
    scored = []
    for p in argv:
        m = key_metric(p)
        if m is not None:
            scored.append((m, p))
    if not scored:
        print("perf-median: no usable snapshots among %d inputs" % len(argv), file=sys.stderr)
        return 2
    scored.sort()
    metric, chosen = scored[len(scored) // 2]
    print("perf-median: %d usable run(s); median %s=%.4f ms -> %s"
          % (len(scored), UMBRELLA_SCOPE, metric, chosen), file=sys.stderr)
    print(chosen)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
