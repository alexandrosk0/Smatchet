#!/usr/bin/env bash
# test-plan-claim-anchors.sh — a plan's § Deviations / § Implementation log may not
# assert that something ALREADY EXISTED without pointing at where.
#
# Why (plan-post-ship-claims-unverified, tooling P1): a shipped plan's closing
# prose is the only record of what actually landed, and nothing reads it.
# `msvc-build-onboarding-hardening.md:85` closed a row with "`build_standalone.ps1`
# (plan file 1) already had the MSVC bootstrap from slice 1" — when measured
# (2026-08-03), `git log -S vcvars` on that file was EMPTY across all history:
# the promised vcvars/vswhere import was never written, in any revision. (Dated
# deliberately — the file has since been deleted in the bash port, #1955, so the
# measurement is no longer reproducible verbatim; an undated restatement here
# would itself be the unanchored stale claim this gate polices.) The claim sat
# load-bearing for ~2 months and seeded a false premise into a downstream plan's
# § Context.
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
# The `revisit=` date IS enforced here (closing the limitation this header
# originally declared): the C++ deviation-overdue lint never scans markdown, so
# this gate expires its own escapes — a marker whose revisit expiry has passed
# stops suppressing and the claim reports again. Value semantics mirror the
# C++ rule (00-common.sh revisit_overdue): YYYY-MM-DD | YYYY-Qn (quarter-end
# expiry) | slug / never (no calendar expiry); NO revisit field stays active
# (optional in the grammar; slugs may lead with digits — 2026-roadmap stays
# active). Stricter than C++ in one spot: a calendar ATTEMPT that parses as
# neither form (2026-02-30, 2027-08-11typo, 2026-Q5, 20270811) stops
# suppressing — fail closed, never a typo-made permanent exemption.
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

# Guarded: on a rev-parse failure the substitution is empty and `cd ""` SUCCEEDS
# as a no-op — the gate would then scan whatever $PWD happens to be, and
# --baseline would write outside the repo. Same trap the selftest's mktemp guard
# documents; this is the top-of-script instance of it.
_root="$(git rev-parse --show-toplevel 2>/dev/null)" || _root=""
[ -n "$_root" ] || { echo "test-plan-claim-anchors: not inside a git repo" >&2; exit 2; }
cd "$_root" || { echo "test-plan-claim-anchors: cannot cd to '$_root'" >&2; exit 2; }

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

    # Dynamic dates so the fixtures cannot rot: the hardcoded revisit=2027-01-01
    # this replaced would silently flip case (4) from PASS-test to FAIL in 2027.
    # 364-day offsets, not year+1: .replace(year=+1) raises on Feb 29.
    _future="$("$PY" -c 'import datetime; print(datetime.date.today()+datetime.timedelta(days=364))')"
    _past="$("$PY" -c 'import datetime; print(datetime.date.today()-datetime.timedelta(days=364))')"
    # Quarter fixtures: next year's Q4 is always in the future, last year's Q1
    # always past — no leap-day or quarter-boundary sensitivity either way.
    _future_q="$(( $(date +%Y) + 1 ))-Q4"
    _past_q="$(( $(date +%Y) - 1 ))-Q1"

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
    #   neutering escape_active()     -> (7) (8) (9) (10) (12) go red
    #   ValueError -> return True     -> (8) goes red
    #   prefix-capture REVISIT_RE     -> (9) and (10) go red
    #   deleting the quarter branch   -> (11) goes red
    #   dropping the DATE_RE gate     -> (12) goes red (python >= 3.11)
    #   first-substring revisit match -> (13) goes red
    #   unguarded quarter date()      -> (14) goes red (escaped ValueError)
    #   digit-leading -> fail closed  -> (15) goes red
    #   empty value -> return True    -> (16) and (17) go red
    #   re-anchoring CALENDARISH_RE   -> (18) and (19) go red
    #   first-paren DEV_BODY_RE       -> (20) goes red
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

    # (4) the documented escape (future revisit) suppresses it -> PASS.
    _case deviation-escape 0 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_future"') -->
- branch protection already had reviews disabled.
'

    # (7) the SAME escape with a PASSED revisit date must stop suppressing —
    #     the expiry is enforced by this gate itself, since deviation-overdue
    #     never scans markdown. Without it, an escape is a permanent exemption
    #     wearing an expiry date.
    _case escape-expired 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_past"') -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-expired]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (8) a date-SHAPED revisit that is not a real calendar date must also stop
    #     suppressing (fail closed) — lexical comparison would misorder it and
    #     fromisoformat() rejecting it must not fall back to "active forever"
    #     (CodeRabbit finding on PR #2002; resolution deviates from its
    #     keep-active suggestion, which would mint a typo-made permanent
    #     exemption — the exact hole cases (4)/(7) exist to close).
    _case escape-invalid-date 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=2026-02-30) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-invalid-date]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (9) a valid FUTURE date with trailing junk must fail closed — the whole
    #     field is the value, not its date-shaped prefix, and prefix-capture
    #     would keep this active until the embedded date passes.
    _case escape-trailing-junk 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_future"'typo) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-trailing-junk]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (12) an ISO BASIC date must fail closed even though python >= 3.11
    #      fromisoformat() would happily parse it — the grammar permits only
    #      the dashed form, and a future-dated 20270811 riding through as
    #      valid would be indistinguishable from a sanctioned escape.
    _case escape-basic-date 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"${_future//-/}"') -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-basic-date]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (14) a quarter-SHAPED value whose year cannot construct a date
    #      (0000-Q1) must fail closed via the token, not crash the scan with
    #      an escaped ValueError.
    _case escape-invalid-quarter 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=0000-Q1) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-invalid-quarter]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (15) a digit-LEADING slug is still a slug — the grammar allows it, and
    #      2026-roadmap must stay active, not be mistaken for a calendar
    #      attempt (fail-closed applies only to date/quarter-shaped values).
    _case escape-digit-slug 0 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=2026-roadmap) -->
- branch protection already had reviews disabled.
'

    # (16) `revisit=` typed but EMPTY must fail closed — a malformed attempt,
    #      not the sanctioned absent-field case. The C++ rule used to fold empty
    #      into unset and mint a permanent exemption; it now fails closed too,
    #      so the two enforcers agree here.
    _case escape-empty-revisit 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-empty-revisit]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (17) an empty FINAL duplicate field wins over a valid earlier one
    #      (last-field-wins, matching 10-line-rules.sh) and must fail closed
    #      — else appending `; revisit=` would blank out a real expiry.
    _case escape-empty-final-dup 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_future"'; revisit=) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-empty-final-dup]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (17b) an UNPADDED past date (2020-1-1) is a calendar attempt, not a slug.
    #       CALENDARISH_RE required a full \d{4}-\d{2}-\d{2} prefix, so this
    #       genuinely-past value classified as a never-expiring slug — the exact
    #       permanent-exemption class this function closes. `\d{4}-\d` catches it,
    #       matching 00-common.sh revisit_datelike_but_invalid().
    _case escape-unpadded-past 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=2020-1-1) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-unpadded-past]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (18) a QUARTER shape with trailing junk must fail closed, not ride as
    #      a slug — same typo class as (9), unanchored quarter alternative.
    _case escape-quarter-junk 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_future_q"'typo) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-quarter-junk]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (19) an ISO-BASIC shape with trailing junk likewise fails closed.
    _case escape-basic-junk 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"${_future//-/}"'typo) -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-basic-junk]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (20) a `)` inside reason= prose must not truncate the body and drop the
    #      real (expired) revisit field — the body parse runs to the LAST `)`.
    _case escape-paren-reason 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=see foo(bar) note; owner=x; revisit='"$_past"') -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-paren-reason]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (20b) a parenthetical in reason= placed BEFORE rule= must still SUPPRESS.
    #       Case (20) covers the body/value half; this covers the MATCHING half.
    #       DEV_RE gates whether escape_active() is consulted at all, so while it
    #       used `[^)]*` the `)` inside `(branch protection)` put rule= beyond its
    #       reach: no match, no escape, claim reported despite a valid future
    #       marker sitting directly above it. Expect exit 0 (suppressed).
    _case escape-paren-before-rule 0 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(reason=github api state (branch protection); rule=plan-claim-anchor; owner=x; revisit='"$_future"') -->
- branch protection already had reviews disabled.
'

    # (21) a marker MISSING its closing paren never engages suppression at all
    #      (DEV_RE requires the paren) — the claim reports.
    _case escape-unclosed-marker 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=x; owner=x; revisit=never -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-unclosed-marker]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (13) the FINAL revisit= FIELD wins, matching 10-line-rules.sh's field
    #      parse — a `revisit=` embedded in reason= prose must not shadow the
    #      real (expired) field, else prose mints a permanent exemption.
    _case escape-embedded-revisit 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=see revisit=never note; owner=x; revisit='"$_past"') -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-embedded-revisit]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (10) a PAST quarter must expire at quarter end, mirroring the C++
    #      revisit_overdue() semantics the grammar promises.
    _case escape-past-quarter 1 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_past_q"') -->
- branch protection already had reviews disabled.
'
    case "$_out" in
        *UNANCHORED_CLAIM*) ;;
        *) echo "FAIL[escape-past-quarter]: exited 1 but printed no UNANCHORED_CLAIM"
           printf '%s\n' "$_out" | sed 's/^/    /'; fail=1 ;;
    esac

    # (11) a FUTURE quarter and the grammar's `never` form both stay active.
    _case escape-quarter-never 0 '# P

## Implementation log
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit='"$_future_q"') -->
- branch protection already had reviews disabled.
<!-- SMATCHET_DEVIATION(rule=plan-claim-anchor; reason=github api state; owner=x; revisit=never) -->
- the tracker already had the bootstrap.
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

TODAY="$(date +%F)"
export TODAY
"$PY" - "$SCOPE" "$PLAN_GLOB_BASE" <<'PY'
import datetime, os, re, subprocess, sys

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
    r'\b(already had|was already|already has|already have|already exists|already exist'
    r'|already existed|already implemented|already landed|already shipped)\b', re.IGNORECASE)

# A verifiable pointer: `path:123` line-suffix, a `#1234` PR/issue ref, or a
# 7-40 char hex sha. Deliberately shape-only — resolving it is test-markdown-links
# and test-plan-ref-integrity's job, and duplicating that here would make one
# gate fail on another's evidence.
ANCHOR_RE = re.compile(r':\d+\b|#\d{2,}|\b[0-9a-f]{7,40}\b')

# Paren-safe for the SAME reason DEV_BODY_RE below is. This is the GATE: scan()
# requires DEV_RE to match before escape_active() is consulted, so `[^)]*` here
# defeated the whole escape — not just its expiry — whenever reason= carried a
# parenthetical BEFORE rule=, e.g. `reason=github api state (branch protection);
# rule=plan-claim-anchor`. `[^)]*` cannot cross the `)` inside the prose, so
# rule= sat beyond its reach, DEV_RE did not match, and the claim reported with a
# valid escape sitting above it. Fixing DEV_BODY_RE's value parse (#2002) left
# this matching half untouched; CodeRabbit caught it on PR #2090. Selftest (21).
DEV_RE = re.compile(r'SMATCHET_DEVIATION\(.*rule=plan-claim-anchor.*\)')
# The deviation BODY parses as ;-delimited fields and the FINAL revisit= field
# wins, mirroring 10-line-rules.sh's `IFS=';' read -ra kvs` loop — a substring
# match would let `reason=see revisit=never note; revisit=<past>` ride on the
# embedded first occurrence. Value grammar (AGENTS.md deviation spec):
# YYYY-MM-DD | YYYY-Qn | slug | never.
# Greedy to the LAST `)` on the line: `[^)]*` truncated at a `)` inside
# reason= prose, dropping the real revisit= field and minting a permanent
# exemption (CodeRabbit round-11 finding on PR #2002).
DEV_BODY_RE = re.compile(r'SMATCHET_DEVIATION\((.*)\)')
QUARTER_RE = re.compile(r'^(\d{4})-Q([1-4])$')
# Exact dashed form only, gating fromisoformat(): python >= 3.11 accepts ISO
# BASIC dates (20270811) there, and the grammar permits only YYYY-MM-DD.
DATE_RE = re.compile(r'^\d{4}-\d{2}-\d{2}$')
# A calendar ATTEMPT that parses as neither legal form: YYYY- followed by a
# DIGIT, YYYY-Q…, or ISO-basic — all prefix matches, so trailing junk still
# classifies as an attempt and fails closed (2027-08-11typo, 2026-Q1typo,
# 20270811typo, 2026-Q5). A merely digit-LEADING value that matches none of them
# (2026-roadmap, 2026-H2) is a slug — legal, no calendar expiry.
# `\d{4}-\d` rather than the full `\d{4}-\d{2}-\d{2}`: the narrower form read the
# UNPADDED, genuinely-past `2020-1-1` as a never-expiring slug, which is the
# permanent-exemption class this function exists to close. Kept in sync with
# 00-common.sh revisit_datelike_but_invalid(), which classifies identically.
CALENDARISH_RE = re.compile(r'\d{4}-\d|\d{4}-Q|\d{8}')
TODAY = os.environ.get("TODAY", "")


def escape_active(comment_line):
    """True while a plan-claim-anchor deviation escape still suppresses.

    The revisit= date is the escape's expiry, and here it is ENFORCED: the
    deviation-overdue lint that expires these markers scans first-party C++
    only, so without this an author's markdown escape would be a permanent
    exemption wearing an expiry date (the KNOWN LIMITATION this closes). A
    marker with NO revisit field stays active — the date is optional in the
    grammar — but a PASSED date stops suppressing and the claim reports
    again, exactly like an overdue C++ deviation. Value semantics mirror
    00-common.sh's revisit_overdue(): YYYY-MM-DD compares against today,
    YYYY-Qn expires at quarter end (Q1=03-31 .. Q4=12-31), slug/never carry
    no calendar expiry (slugs may lead with digits: 2026-roadmap). One place
    this is STRICTER than the C++ side: a CALENDAR ATTEMPT that parses as
    neither legal form — 2026-02-30, 2027-08-11typo, 2026-Q5, 20270811 —
    stops suppressing. Fail closed, so the typo surfaces instead of minting
    a permanent exemption.
    """
    m = DEV_BODY_RE.search(comment_line)
    if not m:
        # Only reachable when the caller's DEV_RE matched but the body is
        # unparseable — a malformed marker must not mint an exemption: fail
        # CLOSED, same reasoning as every other malformed escape.
        return False
    if not TODAY:
        # Defense in depth: the shell wrapper always exports TODAY, but if it
        # ever arrived empty the expiry rule would silently vanish — abort
        # loudly instead of failing open.
        raise SystemExit("test-plan-claim-anchors: TODAY is unset/empty — "
                         "refusing to run with expiry enforcement disabled")
    value = None
    for field in m.group(1).split(';'):
        field = field.strip()
        if field.startswith('revisit='):
            value = field[len('revisit='):].strip()
    if value is None:
        return True
    if not value:
        # `revisit=` typed but left EMPTY: a malformed attempt, not the
        # sanctioned absent-field case — fail CLOSED. Deliberate divergence
        # from the C++ rule (which folds empty into unset): an empty field
        # must not mint a permanent suppression.
        return False
    qm = QUARTER_RE.match(value)
    if qm:
        year, quarter = int(qm.group(1)), int(qm.group(2))
        try:
            end = datetime.date(year, 3 * quarter, (31, 30, 30, 31)[quarter - 1])
        except ValueError:
            # Quarter-shaped but not constructible (0000-Q1 — year 0 is out
            # of range): fail CLOSED like every other malformed expiry, and
            # never let the exception escape and crash the whole scan.
            return False
        return end >= datetime.date.fromisoformat(TODAY)
    if DATE_RE.match(value):
        try:
            revisit = datetime.date.fromisoformat(value)
        except ValueError:
            # Dashed shape but not a real calendar date (2026-02-30): fail
            # CLOSED — the author demonstrably tried to set an expiry, and
            # treating the typo as "no date, active forever" would recreate
            # the permanent exemption this function exists to end.
            return False
        return revisit >= datetime.date.fromisoformat(TODAY)
    if CALENDARISH_RE.match(value):
        # A calendar ATTEMPT that parses as neither legal form (trailing
        # junk, 2026-Q5, ISO basic 20270811): same fail-CLOSED reasoning —
        # not suppressing surfaces both the claim and the typo.
        return False
    # slug (including digit-leading: 2026-roadmap) / never / empty: no
    # calendar expiry — the C++ rule's "slug / never / unknown -> never
    # overdue" arm.
    return True


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
        if i >= 2 and DEV_RE.search(lines[i - 2]) and escape_active(lines[i - 2]):
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
    """Grandfathered claims as a {(basename, normalised-text)} set.

    A MISSING baseline file yields the EMPTY set — the honest answer for a fresh
    checkout, not an error. Keyed on the claim TEXT rather than the line number,
    so unrelated edits above a claim do not un-grandfather it — and on the plan's
    BASENAME rather than its full path, because archiving a plan (the routine
    `git mv` active/ -> shipped/; 188 plans have taken it) changes the path while
    the prose is untouched. Full-path keying made that move red the required doc
    job on claims the archiving PR did not write (reproduced live before this
    fix: `git mv` alone flipped --all to FAIL). Rows still RENDER the full path
    for readability; only the comparison drops the directory.
    """
    keys = {}
    try:
        with open(BASELINE_PATH, encoding="utf-8") as fh:
            for raw in fh:
                m = _BASELINE_ROW_RE.match(raw.rstrip("\n"))
                if m:
                    k = (os.path.basename(m.group(1)), m.group(2))
                    keys[k] = keys.get(k, 0) + 1
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
    for path, text in entries:
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
            # -c core.quotepath=off: with the default quotepath, a non-ASCII filename is
            # emitted octal-escaped in quotes (+++ "b/..."), which the startswith
            # parser below silently misses — added lines in that file then either drop
            # out of scope or attach to the PREVIOUS file at wrong line numbers.
            out = subprocess.run(["git", "-c", "core.quotepath=off", "diff", "--unified=0", "--no-color", *diff_args],
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
                                   capture_output=True, text=True, check=True).stdout.splitlines()  # splitlines, not split(): a path with a space is one file, not two tokens
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
    # One row PER OCCURRENCE (a list, not a set) so multiplicity round-trips
    # into the count-bounded comparison above.
    entries = sorted((p, norm(t)) for p, _l, t in violations)
    with open(BASELINE_PATH, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(render_baseline(entries) + "\n")
    print("test-plan-claim-anchors: regenerated %s (%d claim(s))" % (BASELINE_PATH, len(entries)))
    sys.exit(0)

# --all consults the grandfather file. Diff scope does NOT: it already
# grandfathers by scope (it only ever sees lines the change actually ADDED), so
# consulting the baseline there would double-grandfather and let a re-added claim
# through on the strength of an old row.
# The baseline is a MULTISET: norm() is many-to-one (whitespace collapsed,
# backticks stripped), so set-membership would grandfather a brand-NEW claim
# that happens to reuse a grandfathered row's sentence in the same file — and
# CI runs only --all, so the diff-scope protection never covers the gate of
# record. Count-bounded: a file gets AT MOST as many occurrences of a given
# claim text as the committed baseline recorded; the excess is reportable.
baseline = dict(read_baseline()) if scope == "all" else {}
reportable = []
for v in violations:
    k = (os.path.basename(v[0]), norm(v[2]))
    if baseline.get(k, 0) > 0:
        baseline[k] -= 1
    else:
        reportable.append(v)
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
