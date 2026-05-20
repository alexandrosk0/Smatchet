#!/usr/bin/env python3
"""perf-improvement-detect.py — detect whether a fresh perf snapshot
shows a ≥ THRESHOLD_PCT improvement vs the checked-in baseline on any
row. Used by `.github/workflows/perf-full.yml` to decide whether to
queue a baseline-bump PR.

Exit codes:
    0 — improvement >= threshold on at least one row.
    1 — no improvement above threshold.
    2 — input error (malformed JSON, missing rows, etc.).

Extracted from an inline workflow heredoc — the heredoc body sat at
column 0 inside a YAML `run: |` block scalar, same defect class as
perf-baseline-bootstrap.py (PR #323 fix).
"""

import argparse
import json
import sys


def extract_rows(blob):
    if isinstance(blob, dict):
        if isinstance(blob.get("rows"), list):
            return blob["rows"]
        data = blob.get("data")
        if isinstance(data, dict) and isinstance(data.get("rows"), list):
            return data["rows"]
    return []


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", help="checked-in baseline JSON")
    parser.add_argument("fresh", help="fresh scenario.run JSON")
    parser.add_argument(
        "--threshold-pct", type=float, default=15.0,
        help="minimum improvement percent required to return 0 (default 15.0)"
    )
    args = parser.parse_args(argv)

    try:
        with open(args.baseline, "r", encoding="utf-8") as f:
            baseline = json.load(f)
        with open(args.fresh, "r", encoding="utf-8") as f:
            fresh = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    brows = {r["name"]: r.get("lastTotalMs", 0) for r in extract_rows(baseline) if isinstance(r, dict) and "name" in r}
    frows = {r["name"]: r.get("lastTotalMs", 0) for r in extract_rows(fresh) if isinstance(r, dict) and "name" in r}

    threshold = args.threshold_pct / 100.0
    for name, bv in brows.items():
        try:
            bv = float(bv)
        except (TypeError, ValueError):
            continue
        if bv <= 0:
            continue
        try:
            fv = float(frows.get(name, bv))
        except (TypeError, ValueError):
            continue
        if fv > 0 and (bv - fv) / bv >= threshold:
            print(f"improved: {name} {bv:.3f} → {fv:.3f} ({(bv-fv)/bv*100:.1f}%)")
            return 0
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
