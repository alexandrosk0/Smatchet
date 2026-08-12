#!/usr/bin/env bash
# test-plan-claim-anchors.sh — a plan's § Deviations / § Implementation log may not
# assert that something ALREADY EXISTED without pointing at where.
#
# Why (plan-post-ship-claims-unverified, tooling P1): a shipped plan's closing
# prose is the only record of what actually landed, and nothing reads it.
# `msvc-build-onboarding-hardening.md:85` closed a row with "`build_standalone.ps1`
# (plan file 1) already had the MSVC bootstrap from slice 1" — `git log -S vcvars`
# on that file is EMPTY across all history. The promised vcvars/vswhere import was
# never written, in any revision. The claim then sat load-bearing for ~2 months and
# seeded a false premise into a downstream plan's § Context.
#
# Nothing contradicted it. § Verification (actual) listed 3/3 green, but all three
# cases test other behaviour, so a passing verification block is fully consistent
# with the capability being absent. And postmortem-owed.sh is structurally blind
# here: it reads MERGE signals (non-SUCCESS checks, override labels, Revert,
# overdue deviations), and an untrue sentence in a doc emits none. Detection has
# to happen at doc-gate time.
#
# This gate does NOT prove a claim true — nothing mechanical can. It forces the
# author to point at the code, and **there is no line to point at for a vcvars
# import that does not exist**, which is exactly where #495 would have stopped.
#
# Escape, for claims about state OUTSIDE the repo (branch-protection API,
# upstream releases) that genuinely have no file:line — e.g.
# solo-merge-review-gate.md:91 cites GitHub's `required_pull_request_reviews`:
#   <!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=…; owner=…; revisit=…) -->
# on the line ABOVE the claim, matching the lint-rules.d convention.
#
# KNOWN LIMITATION, stated rather than implied: the `revisit=` date is NOT
# enforced here. The `deviation-overdue` lint that expires these markers scans
# first-party C++ only, so a markdown escape never expires. Zero plans carry the
# marker today, so nothing is currently unpoliced — but the first author to use
# it gets a permanent exemption wearing an expiry date, which is worse than no
# date at all. Extending deviation-overdue to markdown is the follow-up; do not
# read the field as a control until then.
#
# Scope modes:
#   default    — diff-scope: only claims on lines ADDED vs origin/develop.
#   --all      — whole tree, with pre-existing claims grandfathered in
#                docs/high-integrity/plan-claim-anchor-baseline.md.
#   --baseline — regenerate that baseline from the current tree, then commit it.
#   --selftest — fixture assertions on the classifier.
#
# Diff-scope is ADDED-LINE granular, deliberately unlike its sibling
# test-markdown-links.sh, which scopes to changed FILES. A dangling link in a file
# you touched is plausibly yours; a two-year-old claim sentence you scrolled past
# is not. The 25 grandfathered claims cluster (mobile-app-fuller-integration.md
# carries 3), so file granularity would fail authors for prose they did not write
# — and a gate that cries wolf on untouched lines gets escaped rather than obeyed.
set -uo pipefail

# Resolve BEFORE the cd — a relative $0 stops resolving once the cwd moves.
_SCRIPT_PATH="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

cd "$(git rev-parse --show-toplevel)"

PLAN_GLOB_BASE="${SMATCHET_PLAN_BASE:-docs/plans}"

SCOPE="diff"
while [ $# -gt 0 ]; do
    case "$1" in
        --all)      SCOPE="all" ;;
        --baseline) SCOPE="baseline" ;;
        --selftest) SCOPE="selftest" ;;
        *) echo "usage: test-plan-claim-anchors.sh [--all|--baseline|--selftest]" >&2; exit 2 ;;
    esac
    shift
done

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes
# it but exits 49 when run. Probe each candidate; take the first that executes.
PY=""; for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && { echo "test-plan-claim-anchors: python not found" >&2; exit 2; }

# --selftest drives the REAL run against a fixture tree via SMATCHET_PLAN_BASE, so
# the classifier under test is the same code path production uses rather than a
# copy of it. Invoked as `bash "$path"`, never `"$path"`: every gate script here is
# mode 100644, so a direct exec fails 126 Permission denied — a non-zero that
# silently satisfies any negative asserting only "it failed"
# (asserts-failure-marker-does-not-prove-the-negative-is-reachable, tooling P1).
if [ "$SCOPE" = "selftest" ]; then
    fail=0
    tmp="$(mktemp -d)" || { echo "test-plan-claim-anchors: mktemp failed" >&2; exit 2; }
    # An unchecked mktemp leaves tmp empty, and `cd ""` SUCCEEDS in bash (no-op),
    # which would point the fixture at the repo root.
    [ -n "$tmp" ] && [ -d "$tmp" ] || { echo "test-plan-claim-anchors: bad tmpdir" >&2; exit 2; }
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/shipped"

    _out=""
    _case() {  # <name> <want-rc> <file-body>
        printf '%s' "$3" > "$tmp/shipped/case.md"
        _out="$(SMATCHET_PLAN_BASE="$tmp" bash "$_SCRIPT_PATH" --all 2>&1)"
        local rc=$?
        if [ "$rc" != "$2" ]; then
            echo "FAIL[$1]: want rc=$2 got rc=$rc"
            printf '%s\n' "$_out" | sed 's/^/    /'
            fail=1
        fi
    }

    # selftest: asserts-failure — cases (1) and (6) feed a known-bad plan and
    # require rc=1 AND the UNANCHORED_CLAIM token. Verified to DISCRIMINATE by
    # mutation, which is the only check that separates a test from a comment
    # (asserts-failure-marker-does-not-prove-the-negative-is-reachable, tooling
    # P1 — the marker below proves a negative EXISTS, not that it can fail):
    #   neutering CLAIM_RE            -> (1) and (6) go red
    #   deleting the ANCHOR_RE skip   -> (2) goes red
    #   dropping the section bound    -> (3) and (5) go red
    #   deleting the DEV_RE escape    -> (4) goes red
    #   dropping the heading-depth <= -> (6) goes red
    #
    # (1) unanchored claim inside § Deviations -> FAIL, and the output must name
    #     the rule. Asserting only "non-zero" would be satisfied by a python
    #     traceback, a missing interpreter, or a typo in this test itself.
    _case unanchored 1 '# P

## Deviations from plan
- `foo.ps1` already had the bootstrap.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[unanchored]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (2) the same claim WITH a file:line anchor -> PASS. Pins that the ANCHOR is
    #     what flips the verdict, not merely the presence of prose.
    _case anchored 0 '# P

## Deviations from plan
- `foo.ps1:71` already had the bootstrap.
'

    # (3) the same claim in a non-reporting section -> PASS. § Context stating a
    #     premise is not a delivery being closed out.
    _case out-of-section 0 '# P

## Context
- `foo.ps1` already had the bootstrap.
'

    # (4) the documented escape suppresses it -> PASS.
    _case deviation-escape 0 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=2027-01-01) -->
- branch protection already had reviews disabled.
'

    # (5) a SIBLING heading ends the section -> PASS. Without the depth check the
    #     section would run to EOF and swallow the rest of the plan.
    _case section-ends 0 '# P

## Deviations from plan
- nothing.

## Notes
- `foo.ps1` already had the bootstrap.
'

    # (6) a DEEPER heading does NOT end it -> FAIL. The mirror of (5): a `###`
    #     subsection under § Deviations is still the deviations report.
    _case subsection-stays 1 '# P

## Deviations from plan
### Phase B
- `foo.ps1` already had the bootstrap.
'

    if [ "$fail" = "0" ]; then
        echo "test-plan-claim-anchors --selftest: PASS (claim detection + anchor forms + section bounds + deviation escape)"
        echo "Passed: 1  Failed: 0"
        exit 0
    fi
    echo "Passed: 0  Failed: 1"
    exit 1
fi

"$PY" - "$SCOPE" "$PLAN_GLOB_BASE" <<'PY'
import os, re, subprocess, sys

scope, plan_base = sys.argv[1:3]

BASELINE_PATH = "docs/high-integrity/plan-claim-anchor-baseline.md"

# The two sections where a plan reports on its OWN delivery. Elsewhere in a plan
# ("§ Context: the tracker already has a cache") the same words are a premise
# being stated, not a delivery being closed out, and the author has no obligation
# to have verified it here.
SECTION_RE = re.compile(r'^(#{2,4})\s+(Deviations\b.*|Implementation log)\s*$', re.IGNORECASE)
ANY_HEAD_RE = re.compile(r'^(#{1,6})\s+')

# Pre-existing-delivery claims. Every form is "already" + a delivery verb, which
# is what makes the sentence an assertion about state the author did NOT create
# in this change — the only kind this gate can ask them to cite.
CLAIM_RE = re.compile(
    r'\b(already had|was already|already has|already exists|already existed'
    r'|already implemented|already landed|already shipped)\b', re.IGNORECASE)

# A verifiable pointer: `path:123` line-suffix, a `#1234` PR/issue ref, or a
# 7-40 char hex sha. Deliberately shape-only — resolving it is test-markdown-links
# and test-plan-ref-integrity's job, and duplicating that here would make one
# gate fail on another's evidence.
ANCHOR_RE = re.compile(r':\d+\b|#\d{2,}|\b[0-9a-f]{7,40}\b')

DEV_RE = re.compile(r'SMATCHET_DEVIATION\([^)]*rule=plan-claim-anchor[^)]*\)')


def scan(path, lines):
    """Yield (lineno, text) for unanchored delivery claims in the two sections."""
    depth = None
    for i, line in enumerate(lines, 1):
        hm = ANY_HEAD_RE.match(line)
        if hm:
            sm = SECTION_RE.match(line)
            if sm:
                depth = len(sm.group(1))
            elif depth is not None and len(hm.group(1)) <= depth:
                # A heading at the same or shallower level ends the section. A
                # DEEPER one (### under ##) is a subsection and stays in scope.
                depth = None
            continue
        if depth is None or not CLAIM_RE.search(line):
            continue
        if ANCHOR_RE.search(line):
            continue
        if i >= 2 and DEV_RE.search(lines[i - 2]):
            continue
        yield i, line.strip()


def plan_files():
    out = []
    for dirpath, _dirs, files in os.walk(plan_base):
        for fn in sorted(files):
            if fn.endswith(".md"):
                out.append(os.path.join(dirpath, fn).replace(os.sep, "/"))
    return sorted(out)


def read_lines(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.readlines()
    except OSError:
        return []


_BASELINE_ROW_RE = re.compile(r"^-\s+`([^`]+)`\s+—\s+`(.*)`\s*$")


def norm(text):
    """The stable, MARKDOWN-INERT form of a claim line — the baseline key.

    Backticks are STRIPPED, not preserved. Claim prose is full of them
    (`build_standalone.ps1`), and a backtick inside a single-backtick code span
    closes it early, letting whatever follows render as markdown. Several claims
    carry relative links, so verbatim rows made this evidence file emit dangling
    links of its own and redded test-markdown-links --all — one gate failing on
    another gate's evidence. With no backtick in the payload the span cannot be
    broken, so any `[label](target)` inside it stays literal text.

    Stripping backticks is NOT sufficient on its own: test-markdown-links scans
    by regex and does not honour code spans, so a `[label](target)` anywhere in
    this file is a link to it however it is delimited. A space is inserted after
    the `]` — its LINK_RE requires `](` adjacent — which neuters the syntax while
    leaving the row readable.

    Applied to BOTH sides of the comparison, so it only has to be stable, not
    faithful. Whitespace is collapsed so a reflowed paragraph does not silently
    un-grandfather a claim nobody edited.
    """
    return " ".join(text.replace("`", "").replace("](", "] (").split())


def read_baseline():
    """Grandfathered claims as a {(path, text)} set.

    A MISSING baseline file yields the EMPTY set — the honest answer for a fresh
    checkout, not an error. Keyed on the claim TEXT rather than the line number,
    so unrelated edits above a claim do not un-grandfather it.
    """
    keys = set()
    try:
        with open(BASELINE_PATH, encoding="utf-8") as fh:
            for raw in fh:
                m = _BASELINE_ROW_RE.match(raw.rstrip("\n"))
                if m:
                    keys.add((m.group(1), m.group(2)))
    except OSError:
        pass
    return keys


def render_baseline(entries):
    out = [
        "# Unanchored plan-delivery claims — grandfathered baseline",
        "",
        "_Generated by `agents/scripts/core/test-plan-claim-anchors.sh --baseline`.",
        "Do not hand-edit: regenerate and commit._",
        "",
        "Each row is a § Deviations / § Implementation log line asserting that something",
        "ALREADY existed, with no `file:line` / `#PR` / sha to check it against. They are",
        "grandfathered so `--all` is usable as a gate at all; NEW claims must anchor.",
        "Burn one down by adding the citation (or deleting the claim), then re-run",
        "`--baseline` to shrink this file. Adding a row here does not make a claim true.",
        "",
    ]
    for path, text in sorted(entries):
        out.append("- `%s` — `%s`" % (path, text))
    return "\n".join(out)


# ---------------------------------------------------------------- target set
violations = []   # (path, lineno, text)
checked = 0

if scope in ("all", "baseline"):
    for path in plan_files():
        checked += 1
        for lineno, text in scan(path, read_lines(path)):
            violations.append((path, lineno, text))
else:
    # Diff scope: ADDED lines vs origin/develop, plus uncommitted additions, plus
    # untracked-new plan files in full (they have no tracked base to diff, yet CI
    # sees them the moment they are committed — scope them identically or a new
    # plan false-passes pre-push then fails CI).
    def added_lines(*diff_args):
        """{(path, lineno)} for every + line in a diff, from the hunk headers."""
        try:
            out = subprocess.run(["git", "diff", "--unified=0", "--no-color", *diff_args],
                                 capture_output=True, text=True, check=True).stdout
        except subprocess.CalledProcessError as exc:
            sys.stderr.write("WARN: git diff failed (%s); nothing to scope\n" % exc)
            return set()
        added, path, lineno = set(), None, 0
        for raw in out.splitlines():
            if raw.startswith("+++ b/"):
                path = raw[6:]
            elif raw.startswith("@@"):
                m = re.search(r'\+(\d+)', raw)
                lineno = int(m.group(1)) if m else 0
            elif raw.startswith("+") and not raw.startswith("+++"):
                if path:
                    added.add((path, lineno))
                lineno += 1
        return added

    scoped = added_lines("origin/develop...HEAD") | added_lines("HEAD")
    try:
        untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard"],
                                   capture_output=True, text=True, check=True).stdout.split()
    except subprocess.CalledProcessError:
        untracked = []

    by_path = {}
    for path, lineno in scoped:
        by_path.setdefault(path, set()).add(lineno)
    for path in untracked:
        if path.startswith(plan_base + "/") and path.endswith(".md"):
            by_path[path] = None   # None = whole file in scope

    for path, allowed in sorted(by_path.items()):
        if not (path.startswith(plan_base + "/") and path.endswith(".md")):
            continue
        if not os.path.exists(path):
            continue          # deleted in this change
        checked += 1
        for lineno, text in scan(path, read_lines(path)):
            if allowed is None or lineno in allowed:
                violations.append((path, lineno, text))

# ---------------------------------------------------------------- report
if scope == "baseline":
    os.makedirs(os.path.dirname(BASELINE_PATH), exist_ok=True)
    entries = {(p, norm(t)) for p, _l, t in violations}
    with open(BASELINE_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(render_baseline(entries) + "\n")
    print("test-plan-claim-anchors: regenerated %s (%d claim(s))" % (BASELINE_PATH, len(entries)))
    sys.exit(0)

# --all consults the grandfather file. Diff scope does NOT: it already
# grandfathers by scope (it only ever sees lines the change actually ADDED), so
# consulting the baseline there would double-grandfather and let a re-added claim
# through on the strength of an old row.
baseline = read_baseline() if scope == "all" else set()
reportable = [v for v in violations if (v[0], norm(v[2])) not in baseline]
grandfathered = len(violations) - len(reportable)

for path, lineno, text in reportable:
    print("%s:%d: UNANCHORED_CLAIM: asserts pre-existing delivery with no "
          "file:line / #PR / sha to check it against" % (path, lineno), file=sys.stderr)
    print("    %s" % (text[:160]), file=sys.stderr)

if reportable:
    print("", file=sys.stderr)
    print("  Cite what you are claiming: `path/to/file.ext:123`, `#1234`, or a commit sha.", file=sys.stderr)
    print("  If the claim is about state outside this repo, put this on the line ABOVE it:", file=sys.stderr)
    print("    <!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=…; owner=…; revisit=YYYY-MM-DD) -->", file=sys.stderr)
    print("  If you cannot point at it, that is the finding — the claim may not be true.", file=sys.stderr)

if scope == "all" and grandfathered:
    print("NOTE: %d claim(s) grandfathered in %s — burn down and re-run --baseline."
          % (grandfathered, BASELINE_PATH), file=sys.stderr)

print("Passed: %d  Failed: %d  (scanned %d plan file(s); %d unanchored claim(s)%s)"
      % (0 if reportable else 1, 1 if reportable else 0, checked, len(reportable),
         "; %d grandfathered" % grandfathered if grandfathered else ""))
sys.exit(1 if reportable else 0)
PY
