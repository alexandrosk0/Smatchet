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

# An inline code span: a run of N backticks, content, then the SAME run. Prose
# that quotes link syntax inside a span is literal, not a navigable link, so the
# span must come out of the line BEFORE LINK_RE sees it — otherwise a doc that
# needs to discuss link syntax is reported as carrying a dangling link (the
# false-positive half of tooling 2026-08-07).
_CODE_SPAN_RE = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)")


def strip_code_spans(line):
    """(line_with_spans_blanked, [(span_text, end_offset), ...]).

    Spans are replaced by an equal run of SPACES rather than deleted, so column
    offsets stay truthful and two halves of a line can never be glued into a
    link shape that the author did not write."""
    spans = []

    def _blank(m):
        spans.append((m.group(2), m.end()))
        return " " * len(m.group(0))

    return _CODE_SPAN_RE.sub(_blank, line), spans


# A code span that LOOKS like a repo path: a known top-level dir, then a path
# with a file extension. Anchored and space-free so prose in a span never
# matches; an optional `:NNN[:NNN]` line-anchor suffix is tolerated because
# backlog entries cite file:line constantly.
_REPO_PATH_SPAN_RE = re.compile(
    r"^(?:scripts|Source|docs|agents|tests|tools)/"
    r"[A-Za-z0-9._/-]*\.[A-Za-z0-9]+(?::\d+(?::\d+)?)?$"
)
# The code-span path rule is scoped to the backlog: an entry is read months
# later by someone who will act on it, and its whole value is that the cited
# path is real. `applied.md` is exempt — it is the ARCHIVE of entries already
# actioned, so a path that has since moved or been deleted is expected there and
# nobody is going to act on it. Warning on it produced ~100 rows of pure noise
# that would drown the live signal. The rotated monthly partitions
# (applied-YYYY-MM.md, rotate-applied-md.sh) are the same archive, same
# exemption.
CODE_SPAN_SCOPE = "docs/self-improvement/categories/"
CODE_SPAN_EXEMPT_BASENAMES = ("applied.md",)
CODE_SPAN_EXEMPT_PREFIXES = ("applied-",)

# A backlog entry's PROPOSAL block names files it exists to CREATE — "Concrete
# next action: add a `scripts/dev/install-security-tools.sh`". Those paths are
# absent by design, and they are the single most common repo-path code span in
# the backlog: on the first real run they were 8 of 12 warnings. Warning on them
# would train readers to ignore the rule, which is the same failure the
# applied.md exemption avoids. The block is structural, not a verb guess: it
# opens at one of these labels...
_PROPOSAL_OPEN_RE = re.compile(r"^\s*-?\s*(?:Concrete next action|Proposed)\b.*?:")
# ...and closes at the next sibling label, or at the next entry header (`- YYYY-
# MM-DD ...` at column 0). Sub-bullets inside a proposal must NOT close it, so a
# bare `- ` is deliberately not a terminator.
_PROPOSAL_CLOSE_RE = re.compile(
    r"^(?:\s{0,3}(?:Status|Last-reviewed|Cross-ref|Details|Related|Mechanics)\b.*?:"
    r"|- \d{4}-\d{2}-\d{2}\b)"
)


def _develop_tree():
    """Repo-relative paths tracked at origin/develop, or None when that ref is
    unavailable (shallow clone / fresh fork). None means 'cannot answer', and
    the caller must then stay silent rather than warn on every path."""
    try:
        out = subprocess.run(
            ["git", "ls-tree", "-r", "--name-only", "origin/develop"],
            capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, OSError):
        return None
    return {ln.strip() for ln in out.splitlines() if ln.strip()}


def _added_lines(rel_path):
    """Line numbers ADDED/MODIFIED vs origin/develop for rel_path, or None for
    'every line' (untracked file, or git unavailable). Delta-scoping keeps the
    existing backlog from having to be clean on day one — only what a change
    actually writes is held to the rule."""
    # An UNTRACKED file has no baseline, so `git diff` reports no hunks for it —
    # indistinguishable from "tracked and unchanged" by hunk count alone. Ask git
    # directly, or a brand-new backlog entry (the common case for this rule)
    # would be scoped to zero lines and checked not at all.
    try:
        subprocess.run(["git", "ls-files", "--error-unmatch", "--", rel_path],
                       capture_output=True, text=True, check=True)
    except (subprocess.CalledProcessError, OSError):
        return None
    added = set()
    saw_git = False
    for args in (["git", "diff", "-U0", "origin/develop...HEAD", "--", rel_path],
                 ["git", "diff", "-U0", "HEAD", "--", rel_path]):
        try:
            out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, OSError):
            continue
        saw_git = True
        for ln in out.splitlines():
            m = re.match(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", ln)
            if m:
                start = int(m.group(1))
                count = 1 if m.group(2) is None else int(m.group(2))
                added.update(range(start, start + count))
    if not saw_git:
        return None
    # A tracked-but-unchanged file yields an empty set (nothing to check); an
    # untracked file yields empty too, but there git reported no hunks because
    # it has no baseline — so treat a file git does not track as all-lines.
    return added


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
span_warnings = []
_develop_cache = []  # lazily filled once; [] = not yet looked up
checked = 0
for path in TARGETS:
    rel_src_path = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
    span_scope = (rel_src_path.startswith(CODE_SPAN_SCOPE)
                  and os.path.basename(rel_src_path) not in CODE_SPAN_EXEMPT_BASENAMES
                  and not os.path.basename(rel_src_path).startswith(CODE_SPAN_EXEMPT_PREFIXES))
    span_lines = _added_lines(rel_src_path) if (span_scope and SCOPE == "diff") else None
    in_proposal = False
    try:
        with open(path, encoding="utf-8") as fh:
            in_fence = False
            for lineno, line in enumerate(fh, 1):
                if span_scope:
                    if _PROPOSAL_CLOSE_RE.match(line):
                        in_proposal = False
                    if _PROPOSAL_OPEN_RE.match(line):
                        in_proposal = True
                stripped = line.lstrip()
                # Skip fenced code blocks (``` / ~~~): a link inside a fence is
                # illustrative/literal (e.g. example paths in a doc template),
                # never a navigable link — checking it produces false positives.
                if stripped.startswith("```") or stripped.startswith("~~~"):
                    in_fence = not in_fence
                    continue
                if in_fence:
                    continue
                line, code_spans = strip_code_spans(line)
                # WARN-first: a repo path cited in a backlog code span must
                # resolve. Checked at HEAD (the worktree) FIRST, falling back to
                # origin/develop — checking only develop would false-warn on
                # every path the same PR adds, which is the common case, while
                # checking only HEAD would miss the failure this rule exists for
                # (a path that lives on some other feature branch).
                if span_scope and not in_proposal and (span_lines is None or lineno in span_lines):
                    for span_text, span_end in code_spans:
                        cand = span_text.strip()
                        if not _REPO_PATH_SPAN_RE.match(cand):
                            continue
                        # `Source/Core/.../Commands/Foo.h` is an ELIDED path —
                        # prose shorthand for "somewhere under", never a literal
                        # file. It can't resolve and was never meant to.
                        if "..." in cand:
                            continue
                        # Skip a link LABEL span — `[`docs/x.md`](docs/x.md)` —
                        # the href half is already checked as a real link, and
                        # reporting both would double-count one mistake.
                        if line[span_end:span_end + 2] == "](":
                            continue
                        bare = re.sub(r":\d+(?::\d+)?$", "", cand)
                        if os.path.exists(os.path.join(REPO_ROOT, bare)):
                            continue
                        # A tier-less `docs/plans/<slug>.md` resolves when the
                        # slug exists in any tier — same rule the link checker
                        # already applies, so a plan moving active -> shipped
                        # does not turn every citation of it into a warning.
                        if plan_tierless_resolves(os.path.join(REPO_ROOT, bare)):
                            continue
                        if not _develop_cache:
                            _develop_cache.append(_develop_tree())
                        tracked = _develop_cache[0]
                        # Unknown develop (shallow clone / fresh fork) means we
                        # cannot answer — stay silent rather than warn on every
                        # path in the file.
                        if tracked is None or bare in tracked:
                            continue
                        span_warnings.append((rel_src_path, lineno, cand))
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

# WARN-first and deliberately NOT part of the exit code: a reference to a path on
# another unmerged branch is legitimate, and that entry should then carry the
# "not on develop yet" caveat in prose — which is exactly the review this warning
# prompts (tooling 2026-08-07).
for src, lineno, cand in span_warnings:
    print(f"{src}:{lineno}: WARN: code-span path '{cand}' resolves neither in the worktree "
          f"nor at origin/develop — fix it, or note in prose that it is not on develop yet.",
          file=sys.stderr)

passed = checked - (1 if reportable else 0)
failed = 1 if reportable else 0
print(f"Passed: {passed}  Failed: {failed}  (scanned {checked} markdown file(s); "
      f"{len(reportable)} dangling link(s)"
      + (f"; {grandfathered} grandfathered" if grandfathered else "") + ")")
sys.exit(1 if reportable else 0)
PY
