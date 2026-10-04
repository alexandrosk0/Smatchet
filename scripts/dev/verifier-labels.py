#!/usr/bin/env python3
# scripts/dev/verifier-labels.py — turn recorded verifier runs into a labelled
# calibration set for verifier-calibrate.py, by pairing each run with the merge
# outcome of the PR it scored. Pure Python 3 stdlib, deterministic, offline.
# Docs: docs/agent-rules/verifier-sidecar.md § Calibration.
#
# WHY (tooling.md `verifier-calibration-traces-never-collected`): the verifier's
# continuous score can only graduate from advisory to blocking once
# verifier-calibrate.py clears its promotion policy, and that needs scores PAIRED
# WITH ground-truth outcomes. pre-ship.sh records a trace per produced verdict;
# this joins those runs to the merge-time snapshot ledger so the labels come
# from what actually happened to the change, not from anyone's opinion of it.
#
# Inputs:
#   --traces DIR        <trace>.meta.json files pre-ship.sh writes beside each
#                       recorded trace: {"branch","headSha","overall_score",
#                       "hard_veto"} (default .verifier-traces).
#   --ledger FILE       docs/self-improvement/merge-snapshots.jsonl — the join is
#                       meta.headSha == row.headSha (the merged PR head).
#   --postmortems FILE  docs/self-improvement/postmortems.md — a PR named in an
#                       entry heading (`PR #N`, `PR #A, #B`, `PR #A/#B`) is a
#                       known gate escape. Reverts land here too: postmortem-owed
#                       makes every revert owe an entry.
#
# Label — the cheapest honest outcome:
#   1  merged clean — gates GATES_PASSED (or BACKFILLED), redChecks empty, and
#      no postmortem names the PR;
#   0  merged past a gate — any other gates verdict (GATES_INCOMPLETE, …), a
#      non-empty redChecks (an override bypassed a red check), or a postmortem.
#   A run whose headSha never merged (abandoned, or the head moved on after the
#   run) gets NO label — it is counted in "unmatched", never guessed. Several
#   runs on one headSha keep the latest (meta filenames sort by timestamp).
#
# Output (stdout, or --out FILE) — verifier-calibrate.py's calibration-set form:
#   {"cases": [{"id": "<headSha12> <branch> (PR #N)", "score": s, "outcome": 0|1,
#               "hard_veto": bool}, ...], "matched": N, "unmatched": M}
# Then:  python scripts/dev/verifier-calibrate.py <that file> [--gate]
#
# Usage:
#   python scripts/dev/verifier-labels.py [--traces DIR] [--ledger F] [--postmortems F] [--out F]
#   python scripts/dev/verifier-labels.py --selftest
#
# selftest: asserts-failure — --selftest proves a gate-escape merge labels 0 and
# an unmerged run is excluded, not that everything labels 1.
#
# Exit codes:
#   0 — calibration set written (possibly with zero cases; see "matched").
#   1 — --selftest regression.
#   2 — usage / IO error / malformed input.

from __future__ import annotations

import argparse
import glob
import io
import json
import os
import re
import sys
import tempfile
from typing import Any, Dict, List, Set, Tuple

if isinstance(sys.stdout, io.TextIOWrapper):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        sys.stdout = io.TextIOWrapper(
            sys.stdout.buffer, encoding="utf-8", errors="replace", line_buffering=True
        )

CLEAN_GATES = {"GATES_PASSED", "BACKFILLED"}
# `PR #A`, optionally continued `, #B` / `/#C` — the combined-entry heading
# shapes postmortem-owed.sh dedups on.
PR_IN_HEADING = re.compile(r"PR #(\d+)((?:\s*[,/]\s*#\d+)*)")


class LabelError(Exception):
    pass


def _read_text(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError as exc:
        raise LabelError(f"file not found: {path}") from exc


def load_metas(traces_dir: str) -> Dict[str, Dict[str, Any]]:
    """headSha -> latest meta (by sorted filename) carrying a usable score."""
    by_head: Dict[str, Dict[str, Any]] = {}
    for path in sorted(glob.glob(os.path.join(traces_dir, "*.meta.json"))):
        try:
            meta = json.loads(_read_text(path))
        except json.JSONDecodeError as exc:
            raise LabelError(f"malformed meta {path}: {exc}") from exc
        if not isinstance(meta, dict):
            raise LabelError(f"meta {path} is not a JSON object")
        head = str(meta.get("headSha") or "")
        score = meta.get("overall_score")
        if not head or not isinstance(score, (int, float)) or isinstance(score, bool):
            continue  # a run with no score or head cannot be labelled
        by_head[head] = meta
    return by_head


def load_ledger(path: str) -> Dict[str, List[Dict[str, Any]]]:
    """headSha -> ledger rows (a re-merge can, rarely, repeat a head)."""
    rows: Dict[str, List[Dict[str, Any]]] = {}
    for n, line in enumerate(_read_text(path).splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise LabelError(f"malformed ledger line {n}: {exc}") from exc
        if isinstance(row, dict) and row.get("headSha"):
            rows.setdefault(str(row["headSha"]), []).append(row)
    return rows


def postmortem_prs(path: str) -> Set[int]:
    """PR numbers named in a postmortems.md entry heading. A missing file is an
    empty set (a fresh repo has none), not an error."""
    if not os.path.exists(path):
        return set()
    prs: Set[int] = set()
    for line in _read_text(path).splitlines():
        if not line.startswith("#"):
            continue
        for m in PR_IN_HEADING.finditer(line):
            prs.update(int(x) for x in re.findall(r"\d+", m.group(0)))
    return prs


def outcome_for(rows: List[Dict[str, Any]], escaped: Set[int]) -> Tuple[int, int]:
    """(outcome, pr) for a merged head: 0 when ANY row shows a gate bypass."""
    pr = int(rows[-1].get("pr") or 0)
    for row in rows:
        if str(row.get("gates", "")) not in CLEAN_GATES:
            return 0, pr
        if row.get("redChecks"):
            return 0, pr
        if int(row.get("pr") or 0) in escaped:
            return 0, pr
    return 1, pr


def build(traces_dir: str, ledger: str, postmortems: str) -> Dict[str, Any]:
    metas = load_metas(traces_dir)
    merged = load_ledger(ledger)
    escaped = postmortem_prs(postmortems)
    cases: List[Dict[str, Any]] = []
    unmatched = 0
    for head, meta in sorted(metas.items()):
        rows = merged.get(head)
        if not rows:
            unmatched += 1
            continue
        outcome, pr = outcome_for(rows, escaped)
        cases.append({
            "id": f"{head[:12]} {meta.get('branch', '')} (PR #{pr})",
            "score": float(meta["overall_score"]),
            "outcome": outcome,
            "hard_veto": bool(meta.get("hard_veto", False)),
        })
    return {"cases": cases, "matched": len(cases), "unmatched": unmatched}


def _selftest() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        traces = os.path.join(tmp, "traces")
        os.makedirs(traces)

        def meta(name: str, head: str, score: Any, veto: bool = False) -> None:
            with open(os.path.join(traces, name), "w", encoding="utf-8") as f:
                json.dump({"branch": "b-" + head, "headSha": head,
                           "overall_score": score, "hard_veto": veto}, f)

        meta("a-20261001T000000Z.meta.json", "aaaa", 0.2)   # superseded run on the same head
        meta("a-20261002T000000Z.meta.json", "aaaa", 0.9)   # latest run wins
        meta("b-20261001T000000Z.meta.json", "bbbb", 0.4)   # merged past a red check
        meta("c-20261001T000000Z.meta.json", "cccc", 0.7)   # postmortem-named PR
        meta("d-20261001T000000Z.meta.json", "dddd", 0.5)   # never merged
        meta("e-20261001T000000Z.meta.json", "eeee", None)  # no score: unusable
        with open(os.path.join(traces, "x-trace.json"), "w", encoding="utf-8") as f:
            f.write("{not a meta}")                          # the trace itself is ignored
        ledger = os.path.join(tmp, "ledger.jsonl")
        with open(ledger, "w", encoding="utf-8") as f:
            for row in (
                {"pr": 1, "headSha": "aaaa", "gates": "GATES_PASSED", "redChecks": []},
                {"pr": 2, "headSha": "bbbb", "gates": "GATES_PASSED", "redChecks": ["Coverage"]},
                {"pr": 3, "headSha": "cccc", "gates": "GATES_PASSED", "redChecks": []},
                {"pr": 4, "headSha": "eeee", "gates": "GATES_PASSED", "redChecks": []},
            ):
                f.write(json.dumps(row) + "\n")
        pm = os.path.join(tmp, "postmortems.md")
        with open(pm, "w", encoding="utf-8") as f:
            f.write("# Postmortems\n\n## 2026-10-01 · PR #9, #3 · merged red\nBody cites PR #1 in prose.\n")

        out = build(traces, ledger, pm)
        by_pr = {c["id"].rsplit("#", 1)[1].rstrip(")"): c for c in out["cases"]}
        # selftest: asserts-failure — the bad outcomes MUST label 0.
        assert by_pr["2"]["outcome"] == 0, "red-check merge must label 0"
        assert by_pr["3"]["outcome"] == 0, "postmortem-named PR must label 0"
        assert by_pr["1"]["outcome"] == 1, "clean merge (prose mention only) must label 1"
        assert by_pr["1"]["score"] == 0.9, "latest run on a head must win"
        assert out["matched"] == 3 and out["unmatched"] == 1, f"join counts wrong: {out}"
        assert "4" not in by_pr, "a run with no score must not be labelled"
        try:
            load_ledger(pm)  # markdown is not JSONL
        except LabelError:
            pass
        else:
            raise AssertionError("a malformed ledger must raise LabelError")
    print("verifier-labels --selftest: PASS")
    return 0


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--traces", default=".verifier-traces", help="Dir of <trace>.meta.json files.")
    parser.add_argument("--ledger", default="docs/self-improvement/merge-snapshots.jsonl",
                        help="Merge-snapshot ledger (JSONL).")
    parser.add_argument("--postmortems", default="docs/self-improvement/postmortems.md",
                        help="Postmortems ledger (markdown).")
    parser.add_argument("--out", help="Write the calibration set here instead of stdout.")
    parser.add_argument("--selftest", action="store_true", help="Run the deterministic self-test.")
    args = parser.parse_args(argv)
    if args.selftest:
        try:
            return _selftest()
        except AssertionError as exc:
            print(f"verifier-labels --selftest: FAIL — {exc}", file=sys.stderr)
            return 1
    if not os.path.isdir(args.traces):
        print(f"verifier-labels: traces dir not found: {args.traces}", file=sys.stderr)
        return 2
    try:
        out = build(args.traces, args.ledger, args.postmortems)
    except LabelError as exc:
        print(f"verifier-labels: {exc}", file=sys.stderr)
        return 2
    text = json.dumps(out, indent=2) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    print(f"verifier-labels: {out['matched']} labelled run(s), {out['unmatched']} unmatched (never merged at that head).",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
