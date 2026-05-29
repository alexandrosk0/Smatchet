#!/usr/bin/env python3
"""
coderabbit-triage — classify CR findings against Smatchet invariants.

Phase 3 of `docs/plans/shipped/smatchet-merge-watcher.md`. Python port of
`agents/core/coderabbit-triage.md`'s rejection table (the agent doc stays
the source-of-truth for the rules; this is the executable mirror).

When `merge-watcher.py`'s daemon polls a PR + sees the gate BLOCKED on
CR findings (state == BLOCKED + last-CR-review has `Actionable comments
posted: N` with N > 0), it invokes this script:

  coderabbit-triage classify <owner> <repo> <pr>     # print per-finding verdicts
  coderabbit-triage post-comment <owner> <repo> <pr> # also POST a triage summary

Each finding gets one of three verdicts:

- **VALID**: the finding is a real Smatchet bug or improvement. User
  should review + apply (Phase 3 doesn't auto-apply — see § Out of
  scope in plan-doc).
- **REJECT_INVARIANT**: the finding collides with a Smatchet invariant
  (C++14-hard, banned headers, RAII, LOG_*, etc.). Don't apply.
- **REJECT_AMBIGUOUS**: parser couldn't extract a confident
  classification. User decides.

Phase 3 v1 scope: classify + post the structured comment. Per-pattern
auto-fix dispatch deferred to a Phase 3.5 follow-up once we have
calibration data from a few real PR rounds.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum
from typing import Any


class Verdict(str, Enum):
    VALID = "VALID"
    REJECT_INVARIANT = "REJECT_INVARIANT"
    REJECT_AMBIGUOUS = "REJECT_AMBIGUOUS"


@dataclass
class Finding:
    file: str
    line_range: str
    body: str  # raw text of the finding
    verdict: Verdict
    reason: str  # why this verdict


# ---------------------------------------------------------------------------
# Smatchet invariant patterns
# ---------------------------------------------------------------------------
# Each entry: (regex, one-line reason). If CR's finding body matches the
# regex, the finding is REJECT_INVARIANT.
#
# Sourced from AGENTS.md § Project rules. The agent file
# (`agents/core/coderabbit-triage.md`) is the long-form authoritative source;
# this list is the executable mirror — keep in sync per the watcher
# plan-doc § Risks § Phase 3 duplication drift.
INVARIANT_REJECTS: list[tuple[re.Pattern[str], str]] = [
    (
        re.compile(r"\bstd::string_view\b|`string_view`|<string_view>", re.IGNORECASE),
        "string_view banned (AGENTS.md § Project rules § Language: C++14 hard)",
    ),
    (
        re.compile(r"\bstd::optional\b|`optional`|<optional>", re.IGNORECASE),
        "std::optional banned (C++14 hard)",
    ),
    (
        re.compile(r"\bstd::variant\b|`variant`|<variant>", re.IGNORECASE),
        "std::variant banned (C++14 hard)",
    ),
    (
        re.compile(r"\bif\s+constexpr\b", re.IGNORECASE),
        "if constexpr banned (C++14 hard)",
    ),
    (
        re.compile(r"structured\s+binding|auto\s*\[\s*\w+\s*,", re.IGNORECASE),
        "structured bindings banned (C++14 hard)",
    ),
    (
        re.compile(r"\bstd::filesystem\b|<filesystem>", re.IGNORECASE),
        "std::filesystem banned (use ghc::filesystem from FetchContent)",
    ),
    (
        re.compile(r"\bprintf\b|\bstd::cerr\b|\bstd::cout\b", re.IGNORECASE),
        "printf/cerr/cout banned (use LOG_DEBUG/INFO/WARN/ERROR/TRACE per AGENTS.md § Logging)",
    ),
    (
        re.compile(r"\braw\s+new\b|`\s*new\s+[A-Z]|delete\s+\w+|raw\s+pointer", re.IGNORECASE),
        "raw new/delete banned (use std::unique_ptr + make_unique per AGENTS.md § Quality)",
    ),
    (
        re.compile(r"GLFW.*Source/Core/(?:include|src)/.*\.h", re.IGNORECASE),
        "GLFW headers banned in Source/Core/ (DX12 compiles them too — AGENTS.md § Don't)",
    ),
    (
        re.compile(
            r"obj\s*=\s*\{[^}]*\}|json\s*=\s*\{[^}]*\}",
            re.IGNORECASE,
        ),
        "nlohmann json reassign-with-brace-list won't compile (use obj[\"k\"] = v per AGENTS.md)",
    ),
    (
        re.compile(r"C\+\+1[78]|C\+\+2[0-9]", re.IGNORECASE),
        "C++17/20 features banned (C++14 hard for Unreal compat)",
    ),
]


# ---------------------------------------------------------------------------
# CR review body parser
# ---------------------------------------------------------------------------
FINDING_HEADER_RE = re.compile(
    r"In `@(?P<file>[^`]+)`:\s*\n\s*-\s+Around line[s]?\s*(?P<line_range>[\d\-,\s]+):",
    re.MULTILINE,
)


def parse_findings(review_body: str) -> list[dict[str, str]]:
    """Extract findings from CR's review body.

    CR's format (post-2026):
      In `@<file>`:
      - Around line N-M: <body of finding>
        ... (multiple lines of explanation)
      - Around line P-Q: <next finding>

    Returns list of {file, line_range, body}.
    """
    findings = []
    # Split on per-file 'In `@...`:' headers, then scan each file-block for
    # '- Around line ...' bullets.
    file_blocks = re.split(r"\nIn `@", "\n" + review_body)
    for block in file_blocks[1:]:  # skip pre-first-block prose
        # Reattach the marker we split on for header parsing.
        block = "In `@" + block
        file_match = re.search(r"In `@([^`]+)`:", block)
        if not file_match:
            continue
        file_path = file_match.group(1).strip()
        # Each '- Around line ...' bullet starts a finding within this block.
        bullets = re.split(r"(?:^|\n)-\s+Around line[s]?\s*", block)
        for bullet in bullets[1:]:
            # Bullet starts with "<line_range>: <body...>"
            colon = bullet.find(":")
            if colon == -1:
                continue
            line_range = bullet[:colon].strip()
            body = bullet[colon + 1:].strip()
            # Truncate at next file-block / next bullet boundary (already split).
            findings.append({"file": file_path, "line_range": line_range, "body": body})
    return findings


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
def classify_finding(finding: dict[str, str]) -> Finding:
    body = finding["body"]
    for pattern, reason in INVARIANT_REJECTS:
        if pattern.search(body):
            return Finding(
                file=finding["file"],
                line_range=finding["line_range"],
                body=body,
                verdict=Verdict.REJECT_INVARIANT,
                reason=reason,
            )
    # No invariant violation — VALID unless the body is too short to be
    # meaningful (< 40 chars suggests CR couldn't articulate a real fix).
    if len(body) < 40:
        return Finding(
            file=finding["file"],
            line_range=finding["line_range"],
            body=body,
            verdict=Verdict.REJECT_AMBIGUOUS,
            reason=f"finding body too short ({len(body)} chars) — parser can't classify confidently",
        )
    return Finding(
        file=finding["file"],
        line_range=finding["line_range"],
        body=body,
        verdict=Verdict.VALID,
        reason="no Smatchet invariant violation detected; user should review + apply",
    )


# ---------------------------------------------------------------------------
# gh wrappers
# ---------------------------------------------------------------------------
def fetch_cr_review_body(owner: str, repo: str, pr: int) -> str | None:
    """Fetch the latest CodeRabbit review body. Returns None if no CR review."""
    result = subprocess.run(
        [
            "gh",
            "pr",
            "view",
            str(pr),
            "--repo",
            f"{owner}/{repo}",
            "--json",
            "reviews,headRefOid",
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh pr view {pr} failed: {result.stderr.strip()[:200]}")
    data = json.loads(result.stdout)
    head = data.get("headRefOid", "")
    reviews = data.get("reviews", [])
    cr_reviews = [
        r
        for r in reviews
        if "coderabbit" in (r.get("author", {}) or {}).get("login", "").lower()
    ]
    if not cr_reviews:
        return None
    # Latest CR review on current head; fall back to most-recent stale.
    on_head = [
        r
        for r in cr_reviews
        if (r.get("commit") or {}).get("oid") == head
    ]
    pool = on_head if on_head else cr_reviews
    pool.sort(key=lambda r: r.get("submittedAt", ""))
    return pool[-1].get("body", "")


def post_pr_comment(owner: str, repo: str, pr: int, body: str) -> bool:
    """POST a PR comment via gh api. Returns True on success."""
    result = subprocess.run(
        [
            "gh",
            "pr",
            "comment",
            str(pr),
            "--repo",
            f"{owner}/{repo}",
            "--body",
            body,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    return result.returncode == 0


# ---------------------------------------------------------------------------
# Report formatting
# ---------------------------------------------------------------------------
def format_triage_report(findings: list[Finding], pr: int, attempt: int, budget: int) -> str:
    lines = [
        "## `smatchet-merge-watcher` triage report",
        "",
        f"_Phase 3 classifier per `docs/plans/shipped/smatchet-merge-watcher.md`. Attempt {attempt}/{budget}._",
        "",
    ]
    if not findings:
        lines += [
            "_No findings parsed from CR's review body — either the format changed or there are no inline comments to triage._",
        ]
    else:
        valid = [f for f in findings if f.verdict == Verdict.VALID]
        invariant = [f for f in findings if f.verdict == Verdict.REJECT_INVARIANT]
        ambiguous = [f for f in findings if f.verdict == Verdict.REJECT_AMBIGUOUS]
        lines += [
            f"**Findings: {len(findings)} total — {len(valid)} VALID · {len(invariant)} REJECT-INVARIANT · {len(ambiguous)} REJECT-AMBIGUOUS**",
            "",
        ]
        if valid:
            lines += ["### ✅ VALID findings (user should review + apply)", ""]
            for f in valid:
                lines += [f"- [ ] **`{f.file}:{f.line_range}`** — {f.body[:200]}{'...' if len(f.body) > 200 else ''}", ""]
        if invariant:
            lines += ["### 🛑 REJECT-INVARIANT (Smatchet rule collision; do NOT apply)", ""]
            for f in invariant:
                lines += [f"- **`{f.file}:{f.line_range}`** — _{f.reason}_", f"  CR's suggestion: {f.body[:160]}{'...' if len(f.body) > 160 else ''}", ""]
        if ambiguous:
            lines += ["### ❓ REJECT-AMBIGUOUS (parser couldn't classify)", ""]
            for f in ambiguous:
                lines += [f"- **`{f.file}:{f.line_range}`** — {f.body[:160]}", ""]
    lines += [
        "",
        "---",
        "_Triage is advisory — no code changes have been applied. Apply VALID findings manually + push to clear._",
        f"_Generated by `scripts/dev/coderabbit-triage.py` on PR #{pr}._",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_classify(args: argparse.Namespace) -> int:
    body = fetch_cr_review_body(args.owner, args.repo, int(args.pr))
    if body is None:
        print("coderabbit-triage: no CR review on this PR", file=sys.stderr)
        return 1
    findings = [classify_finding(f) for f in parse_findings(body)]
    report = format_triage_report(findings, int(args.pr), int(args.attempt), int(args.budget))
    print(report)
    return 0


def cmd_post_comment(args: argparse.Namespace) -> int:
    body = fetch_cr_review_body(args.owner, args.repo, int(args.pr))
    if body is None:
        print("coderabbit-triage: no CR review on this PR", file=sys.stderr)
        return 1
    findings = [classify_finding(f) for f in parse_findings(body)]
    report = format_triage_report(findings, int(args.pr), int(args.attempt), int(args.budget))
    if post_pr_comment(args.owner, args.repo, int(args.pr), report):
        print(f"coderabbit-triage: posted triage report to PR #{args.pr}")
        return 0
    print(f"coderabbit-triage: failed to post comment to PR #{args.pr}", file=sys.stderr)
    return 2


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="coderabbit-triage",
        description=(
            "Classify CodeRabbit findings against Smatchet invariants. "
            "Python port of agents/core/coderabbit-triage.md for "
            "smatchet-merge-watcher Phase 3."
        ),
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn, helptext in (
        ("classify", cmd_classify, "classify findings; print report to stdout"),
        ("post-comment", cmd_post_comment, "classify + POST the report as a PR comment"),
    ):
        sp = sub.add_parser(name, help=helptext)
        sp.add_argument("owner")
        sp.add_argument("repo")
        sp.add_argument("pr")
        sp.add_argument("--attempt", default=1, help="current attempt number (default 1)")
        sp.add_argument(
            "--budget",
            default=3,
            help="MERGE_WATCH_TRIAGE_BUDGET (default 3)",
        )
        sp.set_defaults(func=fn)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except RuntimeError as exc:
        print(f"coderabbit-triage: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
