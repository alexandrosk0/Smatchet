#!/usr/bin/env bash
# test-markdown-links.sh — flag dangling relative markdown links across docs.
#
# Why: sed-based path renames (e.g. docs/plans/active/applied/ → archive/) routinely
# update body text but miss `[label](href)` link hrefs when only the label
# happens to look like the path. PR #496 + #497 each shipped with 3-4
# CR-caught broken-href findings of exactly this shape; this lint catches
# them mechanically before push.
#
# Rules:
#   1. Only relative links are checked (paths starting with `/`, `http://`,
#      `https://`, `mailto:`, or `#` are skipped).
#   2. The href is resolved relative to the source markdown file's directory.
#   3. Anchor fragments (`#section`) are stripped before existence check.
#   4. Image links (`![alt](path)`) are NOT checked — image assets vary by
#      build state.
#   5. Targets: docs/**/*.md, agents/**/*.md, and the top-level repo docs
#      AGENTS.md, BUILD.md, README.md, AI_POLICY.md, CONTEXT-MAP.md,
#      QUICKSTART.md (each if present). QUICKSTART.md joined in
#      dev-onboarding-first-run-quickstart slice 3 — it is the first-run entry
#      doc and links out to BUILD.md, so a dangling link there is the highest-
#      cost one in the tree. The user-facing guides joined the set in gate-blind-spot-sweep
#      Slice 3 — they were the only actively-maintained docs the gate could not
#      see, and cli.md carried two dangling links the whole time. They are no
#      longer named individually here: the root-declutter move relocated them to
#      docs/guides/ and the dated audit reports to docs/audits/, so both groups
#      are covered by the docs/** branch and cannot silently fall out of scope
#      again. KEEP IN SYNC with the is_active_md() root-file tuple below; this
#      header is the documented contract, so a target added there must be added
#      here too.
#   6. Links inside fenced code blocks (``` / ~~~) are skipped — illustrative
#      example paths in a template are literal, not navigable links.
#
# Bypass: SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 (logged when used).
#
# Modes:
#   default — diff-scope: only check markdown files modified vs origin/develop.
#             Grandfathers pre-existing broken refs; catches new regressions.
#   --all   — repo-wide scan of every actively-maintained markdown, minus the
#             entries grandfathered in docs/high-integrity/markdown-link-baseline.md.
#             The baseline exists so --all is usable as a gate at all: widening
#             the target set (rule 5) would otherwise have meant burning down the
#             pre-existing dangling links first. Note the two counts differ and
#             both are correct: the baseline holds 9 distinct `file::href` KEYS,
#             which `--all` reports as 14 line-level OCCURRENCES (one file repeats
#             three hrefs across eight lines). Entries are keyed by
#             `file::href`, not by line number, so unrelated edits above a
#             grandfathered link do not un-grandfather it.
#   --baseline — regenerate that baseline from the current tree, then commit it.
#             The burn-down path the plain diff-scope mode never had: every
#             --all run prints the remaining grandfathered count, and an entry
#             that has been fixed is reported as stale.
#   --merge-tree-warn — advisory (always exit 0): flag TIERED plan links
#             (docs/plans/{active,shipped,deferred}/<slug>.md) in changed
#             markdown that would 404 post-archive on CI though they pass the
#             local existence check. Remedy: the tier-less docs/plans/<slug>.md
#             form (markdown-links-local-passes-ci-fails-after-plan-archive).
#
# Exit codes:
#   0 — every relative link in every scanned markdown resolves to an existing file
#   1 — at least one dangling link
#   2 — missing binary (python)

set -euo pipefail
cd "$(dirname "$0")/../../.."

if [ "${SMATCHET_SKIP_MARKDOWN_LINK_CHECK:-0}" = "1" ]; then
    echo "test-markdown-links: SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 — skipping" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

PY="${PYTHON:-}"
if [ -n "$PY" ]; then
    if ! "$PY" -c "" >/dev/null 2>&1; then
        echo "test-markdown-links: PYTHON='$PY' is not executable" >&2
        exit 2
    fi
else
    for _c in python3 python py; do
        _p="$(command -v "$_c" 2>/dev/null)" || continue
        if "$_p" -c "" >/dev/null 2>&1; then
            PY="$_p"
            break
        fi
    done
fi
[ -n "$PY" ] || { echo "test-markdown-links: python not found" >&2; exit 2; }

# --selftest — prove the diff-scope path INCLUDES untracked files (Slice 8a).
# Regression killed: an UNTRACKED new markdown (e.g. a fresh
# docs/plans/active/<slug>.md) is invisible to `git diff` so it false-passed
# pre-push then failed CI on the same content. We synthesize an untracked
# markdown with a dangling link, run the diff-scope scan, and assert it FAILS
# (caught) — then a clean untracked file must PASS.
if [ "${1:-}" = "--selftest" ]; then
    fail=0
    stamp="docs/.slice8-mdlink-selftest-$$"
    bad="$stamp-bad.md"
    good="$stamp-good.md"
    # Ensure these never linger as tracked refs and always get cleaned.
    cleanup() { rm -f "$bad" "$good"; }
    trap cleanup EXIT
    # An untracked markdown with a dangling relative link.
    printf '# selftest\n\nsee [missing](./this-target-does-not-exist-xyz.md)\n' > "$bad"
    # selftest: asserts-failure — an untracked file with a dangling link MUST be
    # detected; if the diff-scope skips untracked files this scan wrongly passes.
    # Re-invoke with no args -> the default diff-scope path (which now unions
    # untracked files). The new untracked file must drive a non-zero exit.
    # Re-invoked as `bash "$0"`, not `"$0"`: this script is tracked mode 100644
    # (like most of agents/scripts/core), so executing it directly fails with
    # "permission denied" on a POSIX checkout — which made BOTH assertions below
    # read the same non-zero exit and reported the second one as a false failure.
    if bash "$0" >/dev/null 2>&1; then
        echo "test-markdown-links --selftest: FAIL — untracked dangling link NOT detected (scope skipped untracked)" >&2
        fail=1
    fi
    rm -f "$bad"
    # A clean untracked markdown (no relative links) must PASS.
    printf '# selftest\n\nno relative links here\n' > "$good"
    if ! bash "$0" >/dev/null 2>&1; then
        echo "test-markdown-links --selftest: FAIL — clean untracked file wrongly flagged" >&2
        fail=1
    fi
    cleanup
    trap - EXIT
    if [ "$fail" = "0" ]; then
        echo "test-markdown-links --selftest: PASS (untracked files are in diff-scope)"
        exit 0
    fi
    echo "test-markdown-links --selftest: FAIL" >&2
    exit 1
fi

# --merge-tree-warn — synthetic-behind-develop WARN mode
# (markdown-links-local-passes-ci-fails-after-plan-archive). A TIERED plan link
# `[…](…/docs/plans/active/<slug>.md)` resolves locally while the plan still
# lives in active/, but the SAME branch's archival `git mv active -> shipped`
# (or a sibling PR's archival merged into develop) moves the target — so the
# link 404s on CI/post-merge though it passed the local existence check. The
# durable fix is the TIER-LESS form `docs/plans/<slug>.md` (the gates resolve it
# against any tier — see plan_tierless_resolves below); this advisory mode flags
# tiered plan links in CHANGED markdown so the author rewrites them tier-less
# before they rot. WARN-only: always exits 0 (it never blocks a push).
if [ "${1:-}" = "--merge-tree-warn" ]; then
    "$PY" - <<'PY'
import os
import re
import subprocess
import sys

REPO_ROOT = os.getcwd()

# Markdown changed vs origin/develop (committed + working-tree + untracked),
# mirroring the default diff-scope so the warning fires on exactly what a PR
# would ship.
def _changed():
    cmds = (
        ["git", "diff", "--name-only", "origin/develop...HEAD"],
        ["git", "diff", "--name-only", "HEAD"],
        ["git", "ls-files", "--others", "--exclude-standard"],
    )
    out = ""
    for c in cmds:
        try:
            out += subprocess.run(c, capture_output=True, text=True, check=True).stdout
        except subprocess.CalledProcessError:
            pass
    return sorted({ln.strip() for ln in out.splitlines() if ln.strip()})

# A TIERED plan link: `docs/plans/<tier>/<slug>.md` with tier in active/shipped/
# deferred. These are fragile across an archival move; the tier-less form is not.
TIERED_RE = re.compile(
    r'(?<!\!)\[[^\]]*\]\(([^)\s]*docs/plans/(?:active|shipped|deferred)/[A-Za-z0-9._-]+\.md)'
    r'(?:#[^)\s]*)?(?:\s+"[^"]*")?\)'
)

warnings = []
for rel in _changed():
    if not rel.endswith(".md") or not os.path.exists(rel):
        continue
    try:
        with open(rel, encoding="utf-8") as fh:
            in_fence = False
            for lineno, line in enumerate(fh, 1):
                s = line.lstrip()
                if s.startswith("```") or s.startswith("~~~"):
                    in_fence = not in_fence
                    continue
                if in_fence:
                    continue
                for m in TIERED_RE.finditer(line):
                    warnings.append(
                        "%s:%d: TIERED_PLAN_LINK: '%s' — use the tier-less form "
                        "'docs/plans/<slug>.md' so it survives an archival "
                        "git mv (active->shipped)." % (rel, lineno, m.group(1))
                    )
    except (OSError, UnicodeDecodeError):
        pass

for w in warnings:
    print("WARN: " + w, file=sys.stderr)

# WARN-only: never block. Report count on stdout for the wrapper.
print("Passed: 1  Failed: 0  (merge-tree-warn: %d tiered plan-link warning(s))" % len(warnings))
sys.exit(0)
PY
    exit 0
fi

# Diff-scope by default; --all overrides for whole-repo audit; --baseline
# regenerates the grandfather file from a whole-repo audit (and never fails).
SCOPE="diff"
case "${1:-}" in
    --all) SCOPE="all" ;;
    --baseline) SCOPE="baseline" ;;
    "") : ;;
    *)
        echo "test-markdown-links: unknown argument '$1'" >&2
        echo "  usage: test-markdown-links.sh [--all|--baseline|--merge-tree-warn|--selftest]" >&2
        exit 2
        ;;
esac
export SCOPE

"$PY" - <<'PY'
import os
import re
import sys

import subprocess

REPO_ROOT = os.getcwd()
SCOPE = os.environ.get("SCOPE", "diff")
# Excluded paths under docs/ (active vs archived): docs/plans/active/applied/ and
# docs/plans/shipped/ hold shipped/historical plans whose link-paths drifted
# over time without being maintained — they're documentation-history, not
# active. Scope to docs that ARE actively maintained.
# Use POSIX separators throughout — git diff --name-only emits forward
# slashes on every platform, including Windows. Mixing os.sep here would
# silently misclassify paths in diff scope under cmd/powershell.
EXCLUDED_PREFIXES = (
    "docs/plans/active/applied",
    "docs/plans/shipped",
)


def is_active_md(rel_path):
    """True if rel_path is one of the actively-maintained markdown files
    (docs/, agents/, root-level repo docs) and NOT under an archived dir."""
    if not rel_path.endswith(".md"):
        return False
    # Normalize to POSIX so the same logic works for git-diff output and
    # os.walk output on Windows (os.sep == '\\').
    rel = rel_path.replace(os.sep, "/")
    parts = rel.split("/")
    # Top-level repo docs. KEEP IN SYNC with rule 5 in this script's header —
    # that header is the documented contract for what the gate covers. The
    # user-facing guides (docs/guides/) and the dated audit reports
    # (docs/audits/) used to be listed here as root files; the root-declutter
    # move put them under docs/, where the docs/** branch below already covers
    # them — so they need no entry in this tuple.
    if parts[0] in ("AGENTS.md", "BUILD.md", "README.md", "AI_POLICY.md",
                    "CONTEXT-MAP.md", "QUICKSTART.md") and len(parts) == 1:
        return True
    if parts[0] not in ("docs", "agents"):
        return False
    for p in EXCLUDED_PREFIXES:
        if rel == p or rel.startswith(p + "/"):
            return False
    return True


BASELINE_PATH = "docs/high-integrity/markdown-link-baseline.md"
# Matches a rendered row `- `<src>` — `<href>` (resolves to `<target>`)`. Deliberately NOT
# anchored at end-of-line: the trailing "(resolves to ...)" is human context, not part of the key.
_BASELINE_ROW_RE = re.compile(r"^-\s+`([^`]+)`\s+—\s+`([^`]+)`")


def read_baseline():
    """Grandfathered `(source_md, href)` pairs. Keyed by href rather than line number so an
    unrelated edit above a grandfathered link does not silently un-grandfather it. A missing
    baseline is an EMPTY set — the honest answer for a fresh checkout, not an error."""
    keys = set()
    try:
        with open(os.path.join(REPO_ROOT, BASELINE_PATH), encoding="utf-8") as fh:
            for raw in fh:
                m = _BASELINE_ROW_RE.match(raw.strip())
                if m:
                    keys.add((m.group(1), m.group(2)))
    except OSError:
        return set()
    return keys


def render_baseline(entries):
    """entries: sorted list of (source_md, href, resolved_target)."""
    out = [
        "# Dangling markdown links — grandfathered baseline",
        "",
        "_Auto-generated. Do not hand-edit; run `bash agents/scripts/core/test-markdown-links.sh "
        "--baseline` and commit._",
        "_Scope: `--all` (repo-wide). The DEFAULT diff-scope mode does not consult this file — it "
        "already grandfathers by scope, only ever checking markdown a change actually touches._",
        "_Keyed by `source::href`, not by line number, so an edit above a grandfathered link does "
        "not un-grandfather it. Burn these down and re-run `--baseline`; `--all` reports any entry "
        "that is already fixed as stale._",
        "",
        "## dangling links (%d)" % len(entries),
    ]
    if entries:
        for src, href, target in entries:
            out.append("- `%s` — `%s` (resolves to `%s`)" % (src, href, target))
    else:
        out.append("- _(none — every relative link in every actively-maintained markdown resolves)_")
    out += ["", "## Totals", "- dangling links grandfathered: %d" % len(entries), ""]
    return "\n".join(out)


TARGETS = []
if SCOPE == "diff":
    # Find every markdown changed vs origin/develop (ahead-range).
    try:
        out = subprocess.run(
            ["git", "diff", "--name-only", "origin/develop...HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout
        # Also include uncommitted working-tree changes.
        out += subprocess.run(
            ["git", "diff", "--name-only", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout
        # And UNTRACKED files (respecting .gitignore via --exclude-standard).
        # A brand-new uncommitted markdown file is invisible to `git diff` (it
        # has no tracked baseline) yet CI sees it the moment it's committed — so
        # the local mirror must scope it identically or a new doc/plan file
        # false-passes pre-push then fails CI (close-gate-gaps Slice 8a).
        out += subprocess.run(
            ["git", "ls-files", "--others", "--exclude-standard"],
            capture_output=True, text=True, check=True,
        ).stdout
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(f"WARN: git diff failed ({exc}); falling back to repo-wide\n")
        out = ""
    changed = sorted(set(
        line.strip() for line in out.splitlines() if line.strip()
    ))
    for rel_path in changed:
        if is_active_md(rel_path) and os.path.exists(rel_path):
            TARGETS.append(os.path.join(REPO_ROOT, rel_path))
else:
    # Repo-wide scan.
    for root, dirs, files in os.walk(REPO_ROOT):
        skip = {".git", "build", "node_modules", "_deps", "FetchContent",
                "external", "__pycache__", ".claude"}
        dirs[:] = [d for d in dirs if d not in skip]
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, REPO_ROOT)
            if is_active_md(rel):
                TARGETS.append(full)

# Match `[label](href)` but NOT image links `![alt](path)`. Negative
# look-behind for `!`. The href group is everything up to the closing `)`
# but stops at whitespace (defensive against malformed links).
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)')

# Match a tier-LESS plan path `docs/plans/<slug>.md` (no active/shipped/deferred
# segment) anywhere in the repo-relative resolved target.
_TIERLESS_PLAN_RE = re.compile(r"(?:^|/)docs/plans/([A-Za-z0-9._-]+\.md)$")


def plan_tierless_resolves(target):
    """A tier-less plan link `docs/plans/<slug>.md` resolves when <slug>.md
    exists in active/, shipped/, or deferred/ — so a plan can move between tiers
    without breaking the reference (no ref-sweep on archival). Returns True iff
    the target is such a path AND the slug exists in some tier."""
    rel = os.path.relpath(target, REPO_ROOT).replace(os.sep, "/")
    m = _TIERLESS_PLAN_RE.search("/" + rel)
    if not m:
        return False
    slug = m.group(1)
    return any(
        os.path.exists(os.path.join(REPO_ROOT, "docs", "plans", tier, slug))
        for tier in ("active", "shipped", "deferred")
    )


violations = []
checked = 0
for path in TARGETS:
    try:
        with open(path, encoding="utf-8") as fh:
            in_fence = False
            for lineno, line in enumerate(fh, 1):
                stripped = line.lstrip()
                # Skip fenced code blocks (``` / ~~~): a link inside a fence is
                # illustrative/literal (e.g. example paths in a doc template),
                # never a navigable link — checking it produces false positives.
                if stripped.startswith("```") or stripped.startswith("~~~"):
                    in_fence = not in_fence
                    continue
                if in_fence:
                    continue
                for m in LINK_RE.finditer(line):
                    href = m.group(1)
                    # Skip absolute / external / anchor-only / mailto.
                    if href.startswith(("/", "http://", "https://", "mailto:",
                                        "#", "ftp://", "file://", "data:")):
                        continue
                    # Strip anchor fragment.
                    href_path = href.split("#", 1)[0]
                    if not href_path:
                        continue
                    # Strip `:NNN` line-anchor suffix — `[label](Foo.cpp:123)`
                    # is widely used in plan-docs as a file+line reference.
                    # Treat as if pointing at the file.
                    line_match = re.match(r"^(.*?):\d+(?::\d+)?$", href_path)
                    if line_match:
                        href_path = line_match.group(1)
                    # Resolve relative to the file's directory.
                    src_dir = os.path.dirname(path)
                    target = os.path.normpath(os.path.join(src_dir, href_path))
                    if not os.path.exists(target) and not plan_tierless_resolves(target):
                        rel_src = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
                        rel_tgt = os.path.relpath(target, REPO_ROOT).replace(os.sep, "/")
                        violations.append((rel_src, lineno, href, rel_tgt))
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"WARN: could not read {path}: {exc}\n")
    checked += 1

if SCOPE == "baseline":
    entries = sorted({(src, href, tgt) for src, _lineno, href, tgt in violations})
    out_path = os.path.join(REPO_ROOT, BASELINE_PATH)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(render_baseline(entries) + "\n")
    print(f"test-markdown-links: regenerated {BASELINE_PATH} "
          f"({len(entries)} grandfathered link(s)) — review + commit it.")
    print(f"Passed: 1  Failed: 0  (scanned {checked} markdown file(s))")
    sys.exit(0)

# --all consults the grandfather file so the target set could be widened without a
# 14-item cleanup blocking the slice. Diff-scope does NOT: it already grandfathers by
# scope (it only ever sees markdown a change actually touched), so consulting the
# baseline there would double-grandfather and let a NEW break slip through on a file
# that happens to carry an old one.
baseline = read_baseline() if SCOPE == "all" else set()
reportable = [v for v in violations if (v[0], v[2]) not in baseline]
grandfathered = len(violations) - len(reportable)

for src, lineno, href, tgt in reportable:
    print(f"{src}:{lineno}: BROKEN_LINK: '{href}' does not resolve (looked at {tgt})",
          file=sys.stderr)

if SCOPE == "all":
    stale = sorted(baseline - {(v[0], v[2]) for v in violations})
    if grandfathered:
        print(f"NOTE: {grandfathered} dangling link(s) grandfathered in {BASELINE_PATH} "
              f"— burn down and re-run --baseline.", file=sys.stderr)
    if stale:
        print(f"NOTE: {len(stale)} baselined link(s) already fixed — re-run --baseline to shrink "
              f"the grandfather set: " + ", ".join(f"{s}::{h}" for s, h in stale), file=sys.stderr)

passed = checked - (1 if reportable else 0)
failed = 1 if reportable else 0
print(f"Passed: {passed}  Failed: {failed}  (scanned {checked} markdown file(s); "
      f"{len(reportable)} dangling link(s)"
      + (f"; {grandfathered} grandfathered" if grandfathered else "") + ")")
sys.exit(1 if reportable else 0)
PY
