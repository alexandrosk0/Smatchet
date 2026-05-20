#!/usr/bin/env python3
"""perf-baseline-bootstrap.py — first-run capture helper.

Reads a fresh `scenario.run --outPath` JSON, extracts its `rows[]`
payload, wraps it in the baseline-schema envelope (commit SHA, capture
date, host, runner OS), and writes the result to the destination
baseline path.

Used by `.github/workflows/perf-pr-fast.yml` and `.github/workflows/perf-full.yml`
when no `docs/perf/baselines/<scenario>.ci-windows-latest.json` exists
yet. The workflow uploads the captured baseline as an artefact + the
human reviews / opens a bootstrap PR to commit it.

Extracted from an inline workflow heredoc — the heredoc body lived at
column 0 inside a YAML block-scalar `run: |`, which terminated the
block early and prevented the workflows from scheduling. Moving the
Python into a standalone file keeps the YAML clean.

Usage:
    python scripts/dev/perf-baseline-bootstrap.py <result.json> <baseline.json> <scenarioId> [--host <host>]
"""

import argparse
import datetime
import json
import os
import sys


def extract_rows(raw):
    """Return the `rows[]` list from either `{"data":{"rows":[...]}}`
    (cmd-runner envelope) or `{"rows":[...]}` (raw scenario.run output).
    Returns [] for any other shape.
    """
    if isinstance(raw, dict):
        data = raw.get("data")
        if isinstance(data, dict) and isinstance(data.get("rows"), list):
            return data["rows"]
        if isinstance(raw.get("rows"), list):
            return raw["rows"]
    return []


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="fresh scenario.run JSON result")
    parser.add_argument("destination", help="baseline JSON to write")
    parser.add_argument("scenario_id", help="scenario id (e.g. idle)")
    parser.add_argument("--host", default="ci-windows-latest",
                        help="captureHost label written into the baseline envelope")
    parser.add_argument("--runner-os", default="windows-msys2-ucrt64",
                        help="captureRunnerOs label written into the baseline envelope")
    parser.add_argument("--hardware-note", default="GitHub Actions windows-2022 shared runner",
                        help="hardwareNote string written into the baseline envelope")
    args = parser.parse_args(argv)

    try:
        with open(args.source, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: could not load {args.source}: {exc}", file=sys.stderr)
        return 2

    rows = extract_rows(raw)
    if not rows:
        print(f"ERROR: no rows[] payload in {args.source}; refusing to capture an "
              f"empty baseline (an empty baseline would silently let future regressions pass).",
              file=sys.stderr)
        return 2

    baseline = {
        "schemaVersion": 1,
        "scenarioId": args.scenario_id,
        "captureCommit": os.environ.get("GITHUB_SHA", "?"),
        "captureDate": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "captureHost": args.host,
        "captureRunnerOs": args.runner_os,
        "rows": rows,
        "hardwareNote": args.hardware_note,
        "perScenarioPolicyOverride": None,
    }

    try:
        with open(args.destination, "w", encoding="utf-8") as f:
            json.dump(baseline, f, indent=2)
            f.write("\n")
    except OSError as exc:
        print(f"ERROR: could not write {args.destination}: {exc}", file=sys.stderr)
        return 2

    print(f"captured baseline: {args.destination} ({len(rows)} rows, host={args.host})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
