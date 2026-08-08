#!/usr/bin/env python3
# agents/scripts/core/panel_verdicts.py — panel-verdict -> verifier-sample adapter.
#
# Phase 4 of docs/plans/absorb-whip-process.md: a post-implementation review
# panel's non-authoring legs are independent of the authoring agent by
# construction, so their structured verdicts can feed the verifier-scored
# review gate. This adapter is the one genuinely new piece: it collects the
# `## Verdict` trailer each panel leg appends to its review file
# (`5-review-<round>-<harness>-<model>.md`, grammar below) and emits a
# `verifier-sidecar.py aggregate` INPUT — one candidate, one sample per leg.
# It deliberately does NOT go through verifier-produce.py: that is the
# model-calling half, and these verdicts already exist.
#
# The pipeline it feeds (see docs/agent-rules/review-panels.md § Verdicts):
#
#   python agents/scripts/core/panel_verdicts.py --subject NN --round N \
#     | python scripts/dev/verifier-sidecar.py aggregate - > verdict.json
#   bash agents/scripts/core/review-ack.sh --record --branch --verdict verdict.json
#
# Verdict trailer grammar (per leg, at the end of its review file):
#
#   ## Verdict
#
#   ```json
#   {"criteria": {"correctness": 0.8, ...}, "confidence": 0.7,
#    "hard_veto": false, "veto_reason": ""}
#   ```
#
# The JSON object is passed through as one sidecar sample, so its fields are
# exactly the sidecar's sample fields: `criteria` (name -> score in [0,1]),
# optional `overall_score`, optional `confidence`, optional `hard_veto`
# (JSON boolean), optional `veto_reason`. At least one of `criteria` /
# `overall_score` is required — an empty verdict scores nothing.
#
# Failure asymmetry (mirrors ra_verdict_from_json's fail-closed stance):
#   * A leg file with NO trailer is SKIPPED with a warning — panels degrade
#     (a harness that predates the trailer, or ignored the instruction, costs
#     one sample, not the round).
#   * A trailer that EXISTS but does not parse / fails type checks is FATAL
#     (exit 2): a broken verdict may be hiding a veto, and silently dropping
#     it would drop the only signal the gate blocks on.
#   * Zero usable samples is FATAL (exit 2): the adapter never invents a
#     verdict.
#
# Independence (--authoring-harness): the legs are independent of the authoring
# agent only BY ROSTER, and a degraded panel routinely leaves just the author's
# own harness standing — the pilot item ran 2/2 claude legs under a claude
# author and recorded their score. The verifier plan forbids exactly that
# ("must never fall back to self-scoring"), so same-harness legs are dropped
# from the score while their vetoes are kept (see apply_independence).
#
# Exit codes:
#   0 — samples emitted on stdout
#   2 — usage error, unresolvable subject, malformed trailer, or no samples
#   3 — every leg belongs to the authoring harness and none vetoes: nothing
#       independent to score, so nothing is emitted. Not an error — record a
#       presence-only ack (review-ack.sh --record --branch, no --verdict).
#
# Portable layer: no project literals; paths resolve relative to the git root.

from __future__ import annotations

import argparse
import io
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, NoReturn, Optional, Tuple

for _stream in (sys.stdout, sys.stderr):
    if isinstance(_stream, io.TextIOWrapper):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

DEFAULT_ITEMS_DIR = "docs/work/items"
# Post-implementation reviews only: the verifier gate scores the CODE diff, and
# `5-` is the post-gate prefix (review-panels.md § Reviewer identity). Pre-gate
# reviews judge artifacts, not code, so they carry no verdict trailer.
PREFIX = "5-"

VERDICT_HEADING = re.compile(r"^##\s+Verdict\s*$", re.MULTILINE)
FENCE = re.compile(r"```json\s*\n(.*?)\n```", re.DOTALL)


def fail(message: str) -> NoReturn:
    print(f"panel-verdicts: {message}", file=sys.stderr)
    sys.exit(2)


def warn(message: str) -> None:
    print(f"panel-verdicts: {message}", file=sys.stderr)


def git_root() -> Path:
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        fail("not inside a git work tree")
    return Path(out)


def resolve_item_folder(items_dir: Path, subject: str) -> Path:
    """Same resolution as run-review.sh: exact basename, or NN prefix."""
    if not items_dir.is_dir():
        fail(f"items dir not found: {items_dir}")
    pattern = re.compile(r"^0*" + re.escape(subject) + r"(-|$)")
    matches = [
        d for d in sorted(items_dir.iterdir())
        if d.is_dir() and (d.name == subject or pattern.match(d.name))
    ]
    if len(matches) != 1:
        fail(
            f"subject '{subject}' did not resolve to exactly one "
            f"{items_dir}/NN-slug folder (matched {len(matches)})"
        )
    return matches[0]


def extract_trailer(text: str, name: str) -> Optional[Dict[str, Any]]:
    """The first ```json fence after the LAST `## Verdict` heading, or None.

    Returns None only when the heading itself is absent (leg skipped). A
    heading with no parseable fence, or a fence that is not a JSON object,
    is fatal — see the failure asymmetry in the header.
    """
    headings = list(VERDICT_HEADING.finditer(text))
    if not headings:
        return None
    tail = text[headings[-1].end():]
    fence = FENCE.search(tail)
    if not fence:
        fail(f"{name}: '## Verdict' present but no ```json fence follows it")
    try:
        blob = json.loads(fence.group(1))
    except json.JSONDecodeError as exc:
        fail(f"{name}: verdict JSON does not parse ({exc})")
    if not isinstance(blob, dict):
        fail(f"{name}: verdict must be a JSON object")
    return blob


def check_sample(blob: Dict[str, Any], name: str) -> Dict[str, Any]:
    """Type-check the blocking-relevant fields BEFORE the sidecar sees them.

    The sidecar re-validates, but failing here names the LEG FILE, not a
    candidate index — and a wrong-typed `hard_veto` must never be coerced
    (bool("false") is True; the sidecar rejects it too, this just fails
    earlier and clearer).
    """
    criteria = blob.get("criteria")
    if criteria is not None and not isinstance(criteria, dict):
        fail(f"{name}: 'criteria' must be an object")
    has_scores = isinstance(criteria, dict) and len(criteria) > 0
    overall = blob.get("overall_score")
    if overall is not None:
        if isinstance(overall, bool) or not isinstance(overall, (int, float)):
            fail(f"{name}: 'overall_score' must be a number")
        has_scores = True
    if not has_scores:
        fail(f"{name}: verdict carries neither 'criteria' nor 'overall_score'")
    if has_scores and isinstance(criteria, dict):
        for key, value in criteria.items():
            if isinstance(value, bool) or not isinstance(value, (int, float, dict)):
                fail(f"{name}: criteria.{key} must be a number (or logprobs object)")
    veto = blob.get("hard_veto")
    if veto is not None and not isinstance(veto, bool):
        fail(f"{name}: 'hard_veto' must be a JSON boolean, not {type(veto).__name__}")
    confidence = blob.get("confidence")
    if confidence is not None and (
        isinstance(confidence, bool) or not isinstance(confidence, (int, float))
    ):
        fail(f"{name}: 'confidence' must be a number")
    reason = blob.get("veto_reason")
    if reason is not None and not isinstance(reason, str):
        fail(f"{name}: 'veto_reason' must be a string")
    return blob


def harness_of(name: str, round_no: int) -> Optional[str]:
    """The harness tag out of `5-review-<round>-<harness>-<model>.md`, or None.

    None means the name does not match the launcher's grammar, so the leg's
    provenance is unknown — callers must treat that as NOT independent rather
    than guess (review-panels.md § Reviewer identity owns the grammar).
    """
    m = re.match(
        r"^" + re.escape(PREFIX) + r"review-" + str(round_no) + r"-([a-z0-9]+)-(.+)\.md$",
        name,
    )
    return m.group(1) if m else None


def collect(folder: Path, round_no: int) -> Tuple[List[Tuple[str, Dict[str, Any]]], List[str]]:
    legs_out: List[Tuple[str, Dict[str, Any]]] = []
    skipped: List[str] = []
    legs = sorted(folder.glob(f"{PREFIX}review-{round_no}-*.md"))
    if not legs:
        fail(f"no {PREFIX}review-{round_no}-*.md files under {folder}")
    for leg in legs:
        text = leg.read_text(encoding="utf-8", errors="replace")
        blob = extract_trailer(text, leg.name)
        if blob is None:
            skipped.append(leg.name)
            continue
        legs_out.append((leg.name, check_sample(blob, leg.name)))
    return legs_out, skipped


def apply_independence(
    legs: List[Tuple[str, Dict[str, Any]]], round_no: int, authoring: str
) -> List[Dict[str, Any]]:
    """Keep the score independent of the authoring harness; keep every veto.

    verifier-scored-code-review-gate.md § Approach: "The verifier must not be
    the reviewer ... If no independent backend is configured, the gate records
    *no score* ... it must never fall back to self-scoring." A panel leg run on
    the AUTHORING harness is that case — a different context and a different
    model tag, but not an independent backend.

    That plan's deviation 2 carves out the veto and only the veto: a veto is a
    confession, and self-reported bad news is trusted where self-reported good
    news is not. So a same-harness leg is dropped from the score but retained
    when it vetoes.
    """
    auth = authoring.lower()
    kept: List[Dict[str, Any]] = []
    for name, blob in legs:
        tag = harness_of(name, round_no)
        if tag is not None and tag != auth:
            kept.append(blob)
            continue
        why = "same harness as the author" if tag == auth else "harness tag unreadable"
        if blob.get("hard_veto") is True:
            warn(f"leg NOT independent ({why}) but VETOES — kept: {name}")
            kept.append(blob)
        else:
            warn(f"leg DROPPED from the score — {why}: {name}")
    return kept


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="panel_verdicts.py",
        description="Collect panel-leg verdict trailers into verifier-sidecar aggregate input.",
    )
    parser.add_argument("--subject", required=True,
                        help="work-item NN or slug under the items dir")
    parser.add_argument("--round", required=True, type=int, dest="round_no",
                        help="review round whose leg files are read (1-999)")
    parser.add_argument("--items-dir", default=None,
                        help=f"items tree (default: <git root>/{DEFAULT_ITEMS_DIR})")
    parser.add_argument("--authoring-harness", default=None,
                        help="harness tag of the agent that WROTE the diff (e.g. 'claude'). "
                             "Legs from that harness are dropped from the score — they are "
                             "not an independent backend — but are kept when they veto. "
                             "Omit only when no leg could share the author's harness.")
    args = parser.parse_args(argv)

    if not 1 <= args.round_no <= 999:
        fail("--round must be 1-999")

    items_dir = Path(args.items_dir) if args.items_dir else git_root() / DEFAULT_ITEMS_DIR
    folder = resolve_item_folder(items_dir, args.subject)
    legs, skipped = collect(folder, args.round_no)

    for name in skipped:
        warn(f"leg SKIPPED — no '## Verdict' trailer: {name}")

    if args.authoring_harness:
        samples = apply_independence(legs, args.round_no, args.authoring_harness)
        # `legs and` is load-bearing: rc 3 means "legs existed, none could
        # score". Zero PARSED legs is the different, FATAL case — every trailer
        # was missing — and must fall through to the 0-usable-verdicts failure
        # below. Without the guard, a round where the panel produced no verdicts
        # at all would report as a routine degraded roster and the caller would
        # record a presence-only ack for a round that never actually reviewed.
        if legs and not samples:
            # Not fail(): "every leg was the author's own" is a legitimate,
            # expected panel outcome (roster degradation routinely leaves one
            # harness standing), and it has a defined answer — record a
            # presence-only ack, exactly as the gate does with no independent
            # backend. rc 3 lets the caller tell that apart from a broken run.
            print(
                f"panel-verdicts: no independent leg in {folder} round "
                f"{args.round_no} (authoring harness '{args.authoring_harness}', "
                f"{len(legs)} leg(s), none vetoing) — refusing to emit a "
                "self-scored verdict; record a presence-only ack instead",
                file=sys.stderr,
            )
            return 3
    else:
        warn("independence UNCHECKED — pass --authoring-harness to keep the "
             "author's own legs out of the score")
        samples = [blob for _name, blob in legs]

    if not samples:
        fail(
            f"0 usable verdicts in {folder} round {args.round_no} "
            "— the adapter never invents one"
        )

    json.dump(
        {"candidates": [{
            "id": f"{folder.name}-{PREFIX}round-{args.round_no}",
            "samples": samples,
        }]},
        sys.stdout, indent=2,
    )
    print()
    warn(f"{len(samples)} sample(s) from {len(legs) + len(skipped)} leg file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
