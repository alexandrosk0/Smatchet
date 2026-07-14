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
from pathlib import Path
from typing import Any

# triage-rules-version: 4
# Shared rules-version marker — MUST match agents/core/coderabbit-triage.md.
# `coderabbit-triage.py selftest` greps both files for this token and fails if
# they disagree (run in CI by tests/bats/coderabbit_triage.bats). Bump in BOTH
# whenever the login allow-list, override table, severity
# parse, or noise filter changes. v4 (bugbot-merge-gate): PR-bot login allow-list
# {coderabbitai[bot], cursor[bot]} + Bugbot body-shape severity parse +
# couldn't-run / usage-limit noise filter.

# PR-bot login allow-list (lower-cased substring match — covers both the REST
# `[bot]` suffix and the GraphQL-stripped form). CodeRabbit + Cursor Bugbot are
# live today; Greptile / Sweep join here as they appear (same routing rules).
PR_BOT_LOGIN_TOKENS = ("coderabbit", "cursor")

# Bugbot conversation-tab run-status notices ("### Bugbot couldn't run …",
# "usage limit reached") are spend/availability status, NOT findings — they must
# never enter the triage set (they are the merge-gate's no-wedge TERMINAL signal,
# never a block). Mirrors merge-gates.sh $bbterminal.
_BUGBOT_NOISE_RE = re.compile(r"couldn.t run|usage limit", re.IGNORECASE)


def _is_pr_bot_login(login: str) -> bool:
    """True when `login` is an allow-listed PR-bot (CodeRabbit or Cursor Bugbot)."""
    low = (login or "").lower()
    return any(tok in low for tok in PR_BOT_LOGIN_TOKENS)


def _is_bugbot_noise(body: str) -> bool:
    """True when a body is a Bugbot run-status notice (drop it from the triage set)."""
    return bool(_BUGBOT_NOISE_RE.search(body or ""))


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

# Cursor Bugbot inline-finding body shape (per the bugbot-merge-gate plan's wire
# notes): a `### <title>` heading then a `**<Sev> Severity**` line then a
# `<!-- DESCRIPTION START -->` marker. Bugbot anchors file/line via the comment
# metadata, not the body, so a Bugbot body parses to {file: "", line_range: ""}
# here — the caller supplies path/line from the inline-comment JSON.
BUGBOT_SEVERITY_RE = re.compile(r"\*\*(High|Medium|Low)\s+Severity\*\*", re.IGNORECASE)
BUGBOT_MARKERS_RE = re.compile(r"<!--\s*BUGBOT_(?:REVIEW|FIX_ALL)\s*-->|<!--\s*DESCRIPTION START\s*-->|\*\*(?:High|Medium|Low)\s+Severity\*\*", re.IGNORECASE)


def looks_like_bugbot_body(body: str) -> bool:
    """True when a body carries Cursor Bugbot's wire markers (vs CodeRabbit's shape)."""
    return bool(BUGBOT_MARKERS_RE.search(body or ""))


def parse_bugbot_findings(body: str) -> list[dict[str, str]]:
    """Extract findings from a Cursor Bugbot finding/review body.

    Bugbot opens each finding with a `### <title>` heading then a
    `**<Sev> Severity**` line. Run-status notices (couldn't run / usage limit) are
    dropped as noise. Returns list of {file, line_range, body} (file/line empty —
    Bugbot line-anchors via the inline-comment metadata, not the body text).
    """
    if _is_bugbot_noise(body):
        return []
    findings: list[dict[str, str]] = []
    # Split on `### ` headings; each block is one finding. Fall back to the whole
    # body as a single finding when there are no headings (a bare inline comment).
    blocks = re.split(r"(?:^|\n)###\s+", body)
    candidates = blocks[1:] if len(blocks) > 1 else [body]
    for block in candidates:
        text = block.strip()
        if not text or _is_bugbot_noise(text):
            continue
        sev = BUGBOT_SEVERITY_RE.search(text)
        # Keep only blocks that actually look like a finding (carry a Severity
        # line or a DESCRIPTION marker) so prose/marker-only blocks are skipped.
        if not sev and "DESCRIPTION START" not in text.upper():
            continue
        findings.append({"file": "", "line_range": "", "body": text})
    return findings


def parse_findings(review_body: str) -> list[dict[str, str]]:
    """Extract findings from a PR-bot review body.

    Dispatches on body shape: Cursor Bugbot (`### <title>` + `**<Sev> Severity**`)
    vs CodeRabbit (the `In `@<file>`: - Around line N-M:` enumeration below).

    CR's format (post-2026):
      In `@<file>`:
      - Around line N-M: <body of finding>
        ... (multiple lines of explanation)
      - Around line P-Q: <next finding>

    Returns list of {file, line_range, body}.
    """
    if looks_like_bugbot_body(review_body):
        return parse_bugbot_findings(review_body)
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
    # Allow-list match (CodeRabbit + Cursor Bugbot); drop Bugbot run-status notices
    # (couldn't run / usage limit) — those are not findings.
    bot_reviews = [
        r
        for r in reviews
        if _is_pr_bot_login((r.get("author", {}) or {}).get("login", ""))
        and not _is_bugbot_noise(r.get("body", ""))
    ]
    if not bot_reviews:
        return None
    # Latest bot review on current head; fall back to most-recent stale.
    on_head = [
        r
        for r in bot_reviews
        if (r.get("commit") or {}).get("oid") == head
    ]
    pool = on_head if on_head else bot_reviews
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
        f"_Generated by `agents/scripts/core/coderabbit-triage.py` on PR #{pr}._",
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


# ---------------------------------------------------------------------------
# selftest — md<->py rules-version drift guard
# (core-agent-prompts-03: implement the sync-check the docs advertise;
#  core-scripts-python-03: give the .py a --selftest against the .md.)
# ---------------------------------------------------------------------------
_RULES_VERSION_RE = re.compile(r"triage-rules-version:\s*(\d+)")


def _triage_md_path() -> Path:
    # This script lives at agents/scripts/core/; the agent doc is at agents/core/.
    return Path(__file__).resolve().parents[2] / "core" / "coderabbit-triage.md"


def _extract_rules_version(text: str) -> int | None:
    m = _RULES_VERSION_RE.search(text)
    return int(m.group(1)) if m else None


def cmd_selftest(args: argparse.Namespace) -> int:
    """Drift-guard: the .md is source-of-truth for the rules, the .py is its
    executable mirror, and both carry a `triage-rules-version:` marker that must
    stay in lockstep. This is the end-of-CI sync check both files advertise —
    exercised by tests/bats/coderabbit_triage.bats in the Agentic self-tests lane.
    Also asserts the invariant-reject table is structurally sound. Exit 0 = OK,
    1 = drift/integrity failure."""
    errors: list[str] = []

    py_marker = _extract_rules_version(Path(__file__).read_text(encoding="utf-8"))
    if py_marker is None:
        errors.append("coderabbit-triage.py: missing `# triage-rules-version:` marker")

    md_path = _triage_md_path()
    md_marker: int | None = None
    if not md_path.is_file():
        errors.append(f"coderabbit-triage.md not found at {md_path}")
    else:
        md_marker = _extract_rules_version(md_path.read_text(encoding="utf-8"))
        if md_marker is None:
            errors.append(f"{md_path.name}: missing `triage-rules-version:` marker")

    if py_marker is not None and md_marker is not None and py_marker != md_marker:
        errors.append(
            f"rules-version DRIFT: coderabbit-triage.md={md_marker} vs "
            f"coderabbit-triage.py={py_marker} — bump the marker in BOTH files in lockstep"
        )

    # Invariant-reject table integrity: non-empty, each a (compiled regex, reason).
    if not INVARIANT_REJECTS:
        errors.append("INVARIANT_REJECTS is empty — no invariant rules compiled")
    for i, entry in enumerate(INVARIANT_REJECTS):
        pattern, reason = entry
        if not isinstance(pattern, re.Pattern):
            errors.append(f"INVARIANT_REJECTS[{i}]: not a compiled regex")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"INVARIANT_REJECTS[{i}]: empty reason")

    if errors:
        for e in errors:
            print(f"coderabbit-triage --selftest: FAIL — {e}", file=sys.stderr)
        return 1
    print(
        f"coderabbit-triage --selftest: OK — rules-version={md_marker} "
        f"(md<->py in sync), {len(INVARIANT_REJECTS)} invariant rules"
    )
    return 0


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
    # selftest takes no positional args (md<->py drift + table integrity).
    sp_self = sub.add_parser(
        "selftest",
        help="assert coderabbit-triage.md <-> .py rules-version sync + table integrity",
    )
    sp_self.set_defaults(func=cmd_selftest)
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
