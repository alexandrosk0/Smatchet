#!/usr/bin/env bash
# postmortem-owed.sh — detect gate escapes on recent `develop` merges and nudge
# for a blameless postmortem (see docs/plans/shipped/gate-escape-postmortem.md +
# the gate-escape-postmortem skill). A gate escape = something shipped that a
# gate should have caught; the "gate, don't trust" response is a NEW gate, filed
# via the postmortem's mandatory `### Preventing gate` field.
#
# Triggers (an escape lacking a postmortems.md entry referencing its PR):
#   1. non-SUCCESS check on the merged head (PRIMARY) — a BLOCKING-scope check
#      whose LATEST run was terminal non-SUCCESS (FAILURE, or CANCELLED with no
#      later run) yet merged. The most common escape. "Blocking scope" +
#      "latest-run terminal" follow merge-gates.sh's curation (see CURATION
#      below) so a CANCELLED concurrency twin beside a later SUCCESS / advisory
#      lane / in-progress check never reads as a red required check.
#   2. override label on the merged PR (project.config.json
#      merge_gates.override_labels) — but ONLY when the override was
#      LOAD-BEARING (its gated check did not pass on its own). A moot override
#      (gate green anyway) owes no postmortem.
#   3. a `Revert` commit on develop (a post-merge undo).
#   4. a PR-less direct push to develop — a commit with no `(#N)` subject suffix
#      that no merged PR backs (`commits/{sha}/pulls == 0`). A direct push
#      bypasses review AND every required CI check (the highest-trust escape) yet
#      creates no PR + writes no snapshot, so triggers 1+2 are structurally blind
#      to it.
#   (overdue SMATCHET_DEVIATION is covered by the strict `deviation-overdue` lint.)
#
# Modes:
#   --list     (default) plain "postmortem owed: PR #N — <trigger>" lines.
#   --nudge    SessionStart-formatted block (silent when nothing is owed).
#   --blocking opt-in enforcement: same detection + `--list` output, but EXIT 1
#              when the owed-escape count exceeds POSTMORTEM_BLOCKING_GRACE
#              (default 0 → any owed escape fails). For a CI/pre-merge gate that
#              wants un-postmortemed escapes to actually stop the line, not just
#              nudge. Deliberately NOT wired into SessionStart (that stays
#              advisory) — a caller opts in explicitly. Degrades to advisory
#              exit 0 when gh is unavailable (no gate should hard-fail on a
#              missing/​unauthenticated gh, same fail-open as the other modes).
#
# Mirrors memory-drain-nudge.sh: deterministic check -> nudge, no investigation.
# Default modes (--list / --nudge) are advisory — never block, exit 0 always
# (even without gh; degrades to a notice). Only --blocking can exit non-zero,
# and only on a real owed-escape count over the grace.
#
# Test seams (production leaves all unset → identical behaviour):
#   POSTMORTEM_LEDGER             override the postmortems.md path (has_entry).
#   POSTMORTEM_CONFIG_FILE        override project.config.json (override labels +
#                                 required contexts).
#   POSTMORTEM_REQUIRED_CONTEXTS  override the branch-protection required-context
#                                 set (newline/comma separated). Set-but-EMPTY is
#                                 honoured (inert required-by-name scope) so a
#                                 fixture can opt in explicitly. Mirrors
#                                 merge-gates.sh MERGE_GATES_REQUIRED_CONTEXTS.
#   POSTMORTEM_OVERRIDE_LABELS    override the override-label set (space-separated)
#                                 without sourcing project-config.sh (python-free).
#   SNAPSHOT_LEDGER               override merge-snapshots.jsonl path.
#   POSTMORTEM_DIRECTPUSH_SINCE   git-log window for trigger 4 (default 7 days).
#   POSTMORTEM_DIRECTPUSH_MAX     cap on pulls-confirmation gh calls (default 40).
#   POSTMORTEM_ABSENT_GRACE_SECONDS
#                                 age a merge must reach before it is judged by the
#                                 required-ABSENT cross-check (default 3600). Covers
#                                 GitHub's check-suite creation lag; non-numeric →
#                                 default. Notification threshold only — see the
#                                 $ABSENT_GRACE_SECONDS comment.

set -euo pipefail
# Resolve our own dir BEFORE the repo-root cd (BASH_SOURCE still resolves here)
# so we can source merge-gates.sh by absolute path regardless of cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../../.."

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --blocking) MODE="blocking" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: postmortem-owed.sh [--list|--nudge|--blocking]" >&2; exit 2 ;;
esac

# Blocking-mode grace: number of outstanding owed escapes tolerated before
# --blocking exits non-zero. Default 0 (any owed escape blocks). Raise to
# absorb a known backlog while still catching runaway accumulation. Inert in
# --list / --nudge (those never block).
BLOCKING_GRACE="${POSTMORTEM_BLOCKING_GRACE:-0}"

# Resolve the target repo dynamically (no hardcoded slug — core-scripts-bash-07).
# $REPO overrides (test seam); else derive via gh. Advisory degrade: if the slug
# can't be resolved (gh absent / unauthenticated / not a gh checkout) there is no
# repo to scan, so exit 0 with a notice — same fail-open as the gh check below.
# shellcheck source=agents/scripts/core/lib/resolve-repo.sh
. "$SCRIPT_DIR/lib/resolve-repo.sh"
if ! REPO="$(resolve_repo)"; then
    case "$MODE" in list|blocking) echo "postmortem-owed: cannot resolve repo (set REPO=owner/name or authenticate gh) — skipped (advisory)" >&2 ;; esac
    exit 0
fi
LEDGER="${POSTMORTEM_LEDGER:-docs/self-improvement/postmortems.md}"
CONFIG_FILE="${POSTMORTEM_CONFIG_FILE:-project.config.json}"
SCAN_N="${POSTMORTEM_SCAN_N:-20}"

# Pin the has_entry/has_sha_entry ledger read to origin/develop — the same ref
# the merge scans below trust. Reading the cwd working-tree file nags phantom
# owes when the tree is parked on a stale branch and, worse, false-SUPPRESSES a
# genuinely-owed postmortem when an entry exists only locally (tooling
# 2026-06-19). POSTMORTEM_LEDGER stays an explicit working-file override (the
# bats rig); without it, resolve via `git show` once, falling back to the
# working tree only when origin/develop is unresolvable (degraded, not blind).
if [ -z "${POSTMORTEM_LEDGER+x}" ]; then
    _ledger_tmp="$(mktemp)"
    trap 'rm -f "$_ledger_tmp"' EXIT
    if git show "origin/develop:$LEDGER" > "$_ledger_tmp" 2>/dev/null; then
        LEDGER="$_ledger_tmp"
    fi
fi

# Curated blocking-scope allow-list — SINGLE SOURCE OF TRUTH lives in
# merge-gates.sh as MERGE_GATES_BLOCK_ALLOWLIST_RE. Source it rather than keep a
# hand-synced copy: the duplicate drifted once (a stale branch still carried
# `Bucket-|` after #1259 dropped it from both lists), false-flagging #1258 as a
# gate-escape (tooling.md `postmortem-owed-overreports-nonblocking-and-cancelled-twins`).
# Sourcing is side-effect-free — merge-gates.sh's CLI entry is guarded by
# `[ "${BASH_SOURCE[0]}" = "${0}" ]`, false when sourced (mirrors safe-admin-merge.sh).
# A non-required failing context owes a postmortem (when merged red) only if its
# name matches this regex AND is not "advisory".
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$SCRIPT_DIR/merge-gates.sh"
if [ -z "${MERGE_GATES_BLOCK_ALLOWLIST_RE:-}" ]; then
    echo "postmortem-owed: merge-gates.sh did not export MERGE_GATES_BLOCK_ALLOWLIST_RE — refusing (fail-closed)." >&2
    exit 2
fi
ALLOW_LIST_RE="$MERGE_GATES_BLOCK_ALLOWLIST_RE"

# gh is required; without it, degrade to a quiet notice (advisory tool).
if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
    # --blocking degrades to advisory here too: a missing/unauthenticated gh is
    # an environment gap, not a detected escape, so it must never hard-fail a gate.
    case "$MODE" in list|blocking) echo "postmortem-owed: gh unavailable/unauthenticated — skipped (advisory)" >&2 ;; esac
    exit 0
fi

# Override-label set (config-sourced). POSTMORTEM_OVERRIDE_LABELS overrides the
# config read entirely (tests inject a fixed set, python-independent); production
# leaves it unset and sources project-config.sh.
if [ -n "${POSTMORTEM_OVERRIDE_LABELS+x}" ]; then
    OVERRIDE_LABELS="$POSTMORTEM_OVERRIDE_LABELS"
else
    OVERRIDE_LABELS=""
    # shellcheck source=scripts/dev/project-config.sh
    if . scripts/dev/project-config.sh >/dev/null 2>&1; then
        OVERRIDE_LABELS="${PC_OVERRIDE_LABELS:-}"
    fi
fi

# Required-context ground-truth — branch_protection.required_contexts from
# project.config.json (the authoritative set GitHub branch protection enforces,
# same source merge-gates.sh uses). A failing context flags only if it is in
# this set OR matches ALLOW_LIST_RE. Read UTF-8-safe via jq (the config carries
# an em-dash in "Windows + MSVC (Smatchet light — …)" that python's cp1252
# default open() would mojibake on Windows). jq is a soft dep here; absent →
# empty set (required-by-name scope inert, allow-list scope still applies).
# POSTMORTEM_REQUIRED_CONTEXTS overrides the file read entirely (even to "").
REQ_CTX_JSON='[]'
req_ctx_raw=""
if [ -n "${POSTMORTEM_REQUIRED_CONTEXTS+x}" ]; then
    req_ctx_raw="${POSTMORTEM_REQUIRED_CONTEXTS//,/$'\n'}"
elif [ -f "$CONFIG_FILE" ] && command -v jq >/dev/null 2>&1; then
    req_ctx_raw=$(jq -r '.branch_protection.required_contexts[]? // empty' "$CONFIG_FILE" 2>/dev/null) || req_ctx_raw=""
fi
if [ -n "$req_ctx_raw" ] && command -v jq >/dev/null 2>&1; then
    REQ_CTX_JSON=$(printf '%s\n' "$req_ctx_raw" | jq -R . | jq -sc 'map(select(length>0))') || REQ_CTX_JSON='[]'
fi

# Run-creation grace window for the required-ABSENT detector (below). GitHub
# creates a head's check suite LAGGING the push — measured at ~27 min on #1976
# under backlog (push 21:20Z, `total_count: 0` at 21:25Z, full 15-run set created
# 21:46:58Z, all subsequently green). So a rollup read shortly after a merge can
# show a required context absent that is merely NOT CREATED YET, and there is no
# API field that distinguishes "not yet" from "never" at an instant. A PR merged
# inside this window is therefore NOT judged for absence; it is judged on the next
# sweep once it ages out (this is an after-the-fact detector run at SessionStart —
# reporting an escape one sweep late costs nothing, false-nagging every active
# session costs the detector's credibility). The window is deliberately ~2x the
# measured lag. NOTE this is a *notification* threshold only — it must never be
# read as "absent for < grace is safe to merge"; refusing the merge itself is
# merge-gates.sh's $reqAbsent block, which has no time-based escape hatch.
ABSENT_GRACE_SECONDS="${POSTMORTEM_ABSENT_GRACE_SECONDS:-3600}"
case "$ABSENT_GRACE_SECONDS" in
    ''|*[!0-9]*) ABSENT_GRACE_SECONDS=3600 ;;
esac
# Resolve the window to an absolute ISO-8601 UTC cutoff HERE, with the local jq, so
# the row filter compares plain STRINGS. ISO-8601 UTC sorts lexicographically in
# time order, so `.mergedAt < $cutoff` needs no date builtins in the filter — which
# matters because that filter is executed by `gh`'s EMBEDDED jq, not this one, and
# keeping it to string compare + field selection avoids depending on `now` /
# `fromdateiso8601` being present and identically-behaved there. `date +%s` is POSIX;
# jq's `todate` renders the same `…Z` shape GitHub returns for mergedAt.
ABSENT_CUTOFF_ISO=""
if command -v jq >/dev/null 2>&1; then
    ABSENT_CUTOFF_ISO=$(jq -n --argjson e "$(date +%s)" --argjson g "$ABSENT_GRACE_SECONDS" \
        '($e - $g) | todate' 2>/dev/null | tr -d '"') || ABSENT_CUTOFF_ISO=""
fi

# Expected-present allow-listed NON-required checks (merge-gate-absence-blind-
# nonrequired-allowlist, PR-3). The poller's `$reqAbsent` covers only the
# branch-protection REQUIRED contexts; an allow-listed NON-required check that is
# simply ABSENT from a head's rollup is silently treated as pass — so a PR that
# deletes/renames its own gating workflow makes the check absent and the gate
# self-disables. This is the cross-check: every name here MUST appear on a merged
# PR's rollup; if it is absent, flag it (fail-closed) as an owed escape. We do NOT
# parse workflow `on:` triggers (brittle); the expected-present set is config/env-
# driven and starts EMPTY (inert) — a name is added as each non-required gate is
# trusted to always run on develop PRs. POSTMORTEM_EXPECTED_PRESENT (newline/comma)
# overrides the config read entirely (even to "" — explicit inert opt-in).
EXPECTED_PRESENT_JSON='[]'
exp_present_raw=""
if [ -n "${POSTMORTEM_EXPECTED_PRESENT+x}" ]; then
    exp_present_raw="${POSTMORTEM_EXPECTED_PRESENT//,/$'\n'}"
elif [ -f "$CONFIG_FILE" ] && command -v jq >/dev/null 2>&1; then
    exp_present_raw=$(jq -r '.merge_gates.expected_present_contexts[]? // empty' "$CONFIG_FILE" 2>/dev/null) || exp_present_raw=""
fi
if [ -n "$exp_present_raw" ] && command -v jq >/dev/null 2>&1; then
    EXPECTED_PRESENT_JSON=$(printf '%s\n' "$exp_present_raw" | jq -R . | jq -sc 'map(select(length>0))') || EXPECTED_PRESENT_JSON='[]'
fi

# Known broken-lane check-names (bucket-lane-status-broken-sentinel-auditable, PR-3).
# A block-scope lane whose harness is dead reports a RED conclusion that is NOT a
# product regression — its lane-integrity step wrote `status=broken passed=0
# failed=N` (e.g. a Mesa software-GL collapse: "lane-integrity — bucket-E is
# BROKEN … dead harness, not a regression"). When such a lane is (re-)promoted to
# the block-list, its RED-because-BROKEN must NOT owe a gate-escape postmortem —
# it is an auditable WARN (the harness needs fixing; nothing merged past a real
# gate). This is the machine-readable registry of which lane names are in that
# broken state; a red check matching a name here is downgraded to a WARN line on
# stderr instead of an owed escape. POSTMORTEM_BROKEN_LANES (newline/comma)
# overrides the config read; production starts EMPTY (no lane mis-classified).
BROKEN_LANES_RAW=""
if [ -n "${POSTMORTEM_BROKEN_LANES+x}" ]; then
    BROKEN_LANES_RAW="${POSTMORTEM_BROKEN_LANES//,/$'\n'}"
elif [ -f "$CONFIG_FILE" ] && command -v jq >/dev/null 2>&1; then
    BROKEN_LANES_RAW=$(jq -r '.merge_gates.broken_lanes[]? // empty' "$CONFIG_FILE" 2>/dev/null) || BROKEN_LANES_RAW=""
fi

# is_broken_lane <red-check-trigger> — return 0 when EVERY red-check name in the
# trigger is a known broken lane (so the whole trigger is a broken-harness WARN,
# not an owed escape). A trigger mixing a broken lane with a genuine red check is
# NOT downgraded (the real red still owes). Override parts are ignored here (they
# are handled separately). Returns 1 when no broken-lanes are configured.
is_broken_lane() {
    local trig="$1" red_part name
    [ -n "$BROKEN_LANES_RAW" ] || return 1
    # Extract the "red-check: a, b, c" segment (if any) from the `; `-joined trigger.
    case "$trig" in
        *"red-check: "*) red_part="${trig#*red-check: }"; red_part="${red_part%%;*}" ;;
        *) return 1 ;;   # no red-check part → nothing to downgrade
    esac
    # An override part alongside means a real bypass — never a pure broken-lane WARN.
    case "$trig" in *"override: "*) return 1 ;; esac
    # Every comma-separated red name must be a known broken lane.
    local IFS=','
    for name in $red_part; do
        name="${name#"${name%%[![:space:]]*}"}"   # ltrim
        name="${name%"${name##*[![:space:]]}"}"    # rtrim
        [ -z "$name" ] && continue
        local matched=0 lane
        while IFS= read -r lane; do
            lane="${lane%$'\r'}"   # strip a trailing CR (git-bash jq output on Windows)
            [ -z "$lane" ] && continue
            if printf '%s' "$name" | grep -qiF "$lane"; then matched=1; break; fi  # fail-open-ok: broken-lane tokens are config-controlled name fragments — substring membership is intended (POSTMORTEM_BROKEN_LANES)
        done <<EOF
$BROKEN_LANES_RAW
EOF
        [ "$matched" -eq 1 ] || return 1
    done
    return 0
}

# Already has a postmortem? (ledger references "PR #<n>")
has_entry() {
    [ -f "$LEDGER" ] || return 1
    # Match either a "PR #N" reference or a "commit <sha>" reference, so the
    # commit-only revert/direct-push paths (which pass a sha) dedupe too.
    grep -qE "PR #$1([^0-9]|$)|commit $1([^0-9A-Fa-f]|$)" "$LEDGER" && return 0  # fail-open-ok: a no-match is NOT a clean signal — it falls through to the combined-PR heading probe below
    # Combined-PR postmortem: one blameless RCA can cover several PRs in a single
    # heading written `PR #A, #B, #C` OR slash-joined `PR #A/#B/#C` — only the first
    # carries the literal `PR #` prefix; the rest are bare `, #N` / `/#N`. Match #N
    # inside such a heading line (scoped to `^#+ … PR #…` so a #N mention in prose
    # body can't false-suppress a real owe). The `/` in the separator class fixes
    # slash-joined trailers (`#906/#907/#908`) that used to re-flag every SessionStart.
    grep -qE "^#+ .*PR #[0-9].*[,[:space:]/]#$1([^0-9]|$)" "$LEDGER"
}

# has_sha_entry <sha> — true when the ledger mentions this commit sha in ANY
# form (the sha bounded by non-hex chars), not just the literal `commit <sha>`.
# Existing direct-push / revert postmortems phrase the sha freely — `` `93c63d0f` ``,
# "PR-less direct push 93c63d0f", "(`a678741f` —" — none of which match has_entry's
# `commit <sha>` form. An 8-hex token in a postmortems ledger is a commit sha in
# practice, so bounded-bare matching is the right dedup key for triggers 3+4 (the
# backlog's "deduped by SHA"); the tiny false-dedup risk (an unrelated 8-hex token
# in prose) is acceptable for an advisory nudge.
has_sha_entry() {
    [ -f "$LEDGER" ] || return 1
    grep -qiE "(^|[^0-9A-Fa-f])$1([^0-9A-Fa-f]|\$)" "$LEDGER"
}

# Both the `Test-delta gate` (coverage-delta-gate.sh) and the `cr-out-of-band` override
# are Core-cpp-scoped: the delta gate PASSES any diff with zero `Source/Core/src/*.cpp`
# files (PROD_CHANGES==0 → exit 0), and cr-out-of-band only waives the (advisory)
# CodeRabbit review. So when a flagged PR's SOLE trigger(s) are those two AND it touches
# no Core cpp, the "escape" is a false positive — a transient non-terminal check state
# captured at snapshot time, or an advisory CR waiver on a docs/prose diff. Drop it (same
# spirit as the revert-subject false-positive fix). Genuine Core-cpp escapes still flag.
core_scoped_only_trigger() {
    case "$1" in
        "override: cr-out-of-band") return 0 ;;
        "red-check: Test-delta gate") return 0 ;;
        "red-check: Test-delta gate; override: cr-out-of-band") return 0 ;;
        *) return 1 ;;
    esac
}

# True when the merged PR changed at least one Source/Core/src/*.cpp file (the only
# surface the Core-scoped gates above act on). Only the rare flagged PR pays this query.
pr_touches_core_cpp() {
    gh pr view "$1" --repo "$REPO" --json files --jq '.files[].path' 2>/dev/null \
        | grep -qE '^Source/Core/src/.*\.cpp$'
}

# True when the merged PR added/modified at least one *.test.cpp (the test-delta
# the `tests-out-of-band` override stands in for). Used by the moot-override check.
pr_touches_test_files() {
    gh pr view "$1" --repo "$REPO" --json files --jq '.files[].path' 2>/dev/null \
        | grep -qE '\.test\.cpp$'
}

# Collapsed conclusion across EVERY rollup context whose name/context matches the
# given (case-insensitive regex) needle, on the merged PR head: "SUCCESS" only when
# all matching contexts' latest runs are SUCCESS, else the first non-SUCCESS
# conclusion, or "" when none match. Dedupes to the latest run per context
# (group_by name, max startedAt) like the trigger-1 curation, so a CANCELLED twin
# can't mask a real SUCCESS; the "all must be SUCCESS" rule means a second
# matching lane (e.g. a future "Coverage upload" beside "Coverage (…)") can't let
# a moot-check pass on a partial green. jq required (degrades to "").
gate_conclusion() {
    command -v jq >/dev/null 2>&1 || { echo ""; return 0; }
    # gh's `--jq` does NOT accept jq's `--arg`; pass the needle via the
    # environment (`env.NEEDLE`) instead, which gh's bundled jq honours.
    NEEDLE="$2" gh pr view "$1" --repo "$REPO" --json statusCheckRollup --jq '
        [ .statusCheckRollup[]?
          | { n: (.name // .context // ""),
              s: (.startedAt // .completedAt // .createdAt // ""),
              c: (.conclusion // .state // "") }
          | select(.n | test(env.NEEDLE; "i")) ]
        | group_by(.n) | map(sort_by(.s) | .[-1].c)
        | if length == 0 then ""
          elif all(.[]; . == "SUCCESS") then "SUCCESS"
          else (map(select(. != "SUCCESS")) | .[0]) end
    ' 2>/dev/null | tr -d '\r' || echo ""
}

# override_is_moot <pr> <label> — return 0 (moot, drop) when the override was
# NON-load-bearing: its gated check passed on the head on its own, so the label
# dismissed nothing. Return 1 (load-bearing → keep / owes a postmortem) otherwise.
# Only a load-bearing override owes a postmortem (postmortems.md 2026-06-08 #991).
# Note: gate_conclusion reads the (possibly post-merge re-run) live rollup, the
# documented lossy fallback. This is why the moot-filter runs on the LIVE-fallback
# path ONLY (see the trigger-1 loop) — never on a snapshot-derived trigger, where
# re-judging against the lossy live rollup could drop a real load-bearing override.
override_is_moot() {
    local pr="$1" label="$2"
    case "$label" in
        tests-out-of-band)
            # Moot iff Test-delta gate passed AND the diff actually carried a test
            # delta (the entry's explicit two-part condition, #991).
            [ "$(gate_conclusion "$pr" 'Test-delta gate')" = "SUCCESS" ] && pr_touches_test_files "$pr" ;;
        perf-out-of-band)
            [ "$(gate_conclusion "$pr" 'Perf PR-fast')" = "SUCCESS" ] ;;
        coverage-out-of-band)
            [ "$(gate_conclusion "$pr" 'Coverage')" = "SUCCESS" ] ;;
        intent-out-of-band)
            # Moot iff the Intent section gate passed on its own (gate-specific,
            # like Coverage/Perf). pr-intent-capture-hardening #5 / ADR-0022.
            [ "$(gate_conclusion "$pr" 'Intent section')" = "SUCCESS" ] ;;
        *)
            # cr-out-of-band (advisory CR only — handled by core_scoped_only_trigger)
            # and any unknown label: never treated as moot here.
            return 1 ;;
    esac
}

# filter_moot_overrides <pr> <trigger> — rebuild a `; `-joined trigger string
# keeping every `red-check: …` part and only the LOAD-BEARING `override: …` parts.
# Applied to the LIVE-fallback trigger only (the snapshot path is authoritative —
# see the trigger-1 loop). Prints the filtered trigger (possibly empty).
filter_moot_overrides() {
    local pr="$1" trig="$2" out="" part
    local IFS=';'
    set -f   # disable globbing — `for part in $trig` must not glob-expand a part
    for part in $trig; do
        part="${part# }"; part="${part% }"
        [ -z "$part" ] && continue
        case "$part" in
            "override: "*)
                local lbl="${part#override: }"
                if override_is_moot "$pr" "$lbl"; then
                    continue   # moot — drop
                fi
                out="${out:+$out; }$part" ;;
            *)
                out="${out:+$out; }$part" ;;
        esac
    done
    set +f
    printf '%s' "$out"
}

# --- Lossless merge-time snapshot ledger (PRIMARY source for trigger 1+2) -----
# docs/self-improvement/merge-snapshots.jsonl is the committed, append-only
# gate-verdict ledger written by every merge actor at the decision instant
# (docs/adr/0017-merge-time-snapshot-ledger.md). It is LOSSLESS where the live
# `statusCheckRollup` query below is provably lossy — GitHub overwrites rollup
# contexts by name on re-run, and override labels are stripped post-merge
# (merge-gates.sh). So for any in-window merged PR that HAS a ledger line (keyed
# by pr + mergeCommit), derive the escape trigger from the snapshot; fall through
# to the live query ONLY when no ledger entry exists (documented degraded
# fallback for un-instrumented / pre-ledger merges — the detector never goes
# blind). schema=1: {"pr","mergeCommit","redChecks":[...],"overrideLabels":[...]}.
SNAPSHOT_LEDGER="${SNAPSHOT_LEDGER:-docs/self-improvement/merge-snapshots.jsonl}"

# snapshot_trigger <pr> <mergeCommit> — print the trigger derived from the most
# recent ledger line matching BOTH pr and mergeCommit (redChecks / overrideLabels
# non-empty), or nothing if no such line exists. Requires jq; degrades to "no
# entry" (empty output) when jq is absent so the live fallback still runs.
snapshot_trigger() {
    local pr="$1" mc="$2"
    [ -f "$SNAPSHOT_LEDGER" ] || return 0
    command -v jq >/dev/null 2>&1 || return 0
    jq -rj --argjson pr "$pr" --arg mc "$mc" '
        select(.pr == $pr and .mergeCommit == $mc)
        | [ (if (.redChecks | length) > 0 then "red-check: " + (.redChecks | join(", ")) else empty end),
            (.overrideLabels[]? | "override: " + .) ]
        | join("; ")
    ' "$SNAPSHOT_LEDGER" 2>/dev/null | tail -1 || true
}

# has_snapshot <pr> <mergeCommit> — is there ANY ledger line for this pr+commit?
# (distinguishes "ledger says clean" from "no ledger entry → use live fallback").
has_snapshot() {
    local pr="$1" mc="$2"
    [ -f "$SNAPSHOT_LEDGER" ] || return 1
    command -v jq >/dev/null 2>&1 || return 1
    jq -e --argjson pr "$pr" --arg mc "$mc" \
        'select(.pr == $pr and .mergeCommit == $mc)' \
        "$SNAPSHOT_LEDGER" >/dev/null 2>&1
}

owed=()           # "PR #N — <trigger>"
warns=()          # auditable WARN lines (broken-lane downgrades — not owed escapes)
merged_commits="" # space-delimited set of mergeCommit oids seen this run (trigger 4 reuse)

# --- Trigger 1 + 2: per merged PR (checks + labels), ONE batched gh call ------
# `gh pr list --json statusCheckRollup` returns every PR's checks in a single
# API call — fast enough for a SessionStart hook (no per-PR `gh pr view` loop).
# Each row: number <TAB> mergeCommit-oid <TAB> space-joined-labels <TAB>
# comma-joined-CURATED-red-checks. The mergeCommit column lets us key the lossless
# snapshot-ledger lookup (snapshot_trigger) on pr+mergeCommit before falling
# back to the (lossy) live labels/statusCheckRollup columns.
#
# CURATION (the red-checks column) follows merge-gates.sh's GATE_FILTER so the
# escape detector and the merge gate agree on what "red" means. The red-checks
# column alone is absence-BLIND — a required check that NEVER RAN produces no
# rollup row at all, so it is neither red nor pending, and a "merged with a
# required check absent" escape emits none of the four label/red/revert/deviation
# signals. That is now covered separately by the required-ABSENT cross-check
# below (field 5 vs $reqNames), mirroring merge-gates' pre-merge $reqAbsent block
# so the same escape is caught both before and after the merge.
#   (a) dedupe to the LATEST run per context (group_by name, max startedAt) — a
#       CANCELLED concurrency twin beside a later SUCCESS for the SAME context is
#       dropped (the dominant false positive: every perf/coverage workflow sets
#       cancel-in-progress; the `…/merge` ref run cancels the PR-head run).
#   (b) require a TERMINAL verdict — CheckRun COMPLETED with conclusion in
#       FAILURE/TIMED_OUT/CANCELLED/ACTION_REQUIRED/STARTUP_FAILURE, or
#       StatusContext state FAILURE/ERROR (same set merge-gates.sh blocks on).
#       CANCELLED counts only when it survives dedupe (a) — i.e. it is the
#       LATEST run for its context with no later SUCCESS — so a CANCELLED-latest
#       meant-to-block check that native-merged is an escape (#1566), while the
#       concurrency twin stays invisible; IN_PROGRESS/QUEUED/PENDING never count.
#   (c) require BLOCKING scope — the context is in branch_protection
#       .required_contexts ($reqNames) OR its name matches the meant-to-block
#       allow-list (ALLOW_LIST_RE) and is not "advisory". An advisory / non-
#       allow-list lane can never block a merge, so its red owes no postmortem.
# (postmortems.md 2026-06-09 PR #1046–#1072 over-report; #923 allow-list.)
#
# `gh pr list` has no mergedAt sort key, so over-fetch by its default
# createdAt-desc order, then re-sort by mergedAt and keep the most-recently
# MERGED SCAN_N. Without this, a long-lived branch created early but merged
# late falls outside a createdAt-ordered window and its escape is never seen.
FETCH_N="${POSTMORTEM_FETCH_N:-$((SCAN_N * 3))}"
# shellcheck disable=SC2016  # $c/$reqNames are jq variables, not shell expansions
JQ_ROWS='(sort_by(.mergedAt) | reverse | .[0:__SCAN_N__]) | .[] | [
    (.number|tostring),
    (.mergeCommit.oid // ""),
    ([.labels[].name] | join(" ")),
    ( (__REQ_CTX__) as $reqNames
      | [ ( (.statusCheckRollup // [])
            | group_by(if .__typename == "CheckRun" then "C " + (.name // "")
                       else "S " + (.context // "") end)
            | map(sort_by(.startedAt // .completedAt // .createdAt // "") | .[-1])
            | .[] )
          | (if .__typename == "CheckRun" then (.name // "") else (.context // "") end) as $n
          | select(
              (
                (.__typename == "CheckRun" and (.status // "") == "COMPLETED"
                 and ((.conclusion // "") | IN("FAILURE","TIMED_OUT","CANCELLED","ACTION_REQUIRED","STARTUP_FAILURE")))
                or
                (.__typename == "StatusContext" and ((.state // "") | IN("FAILURE","ERROR")))
              )
              and
              (
                ($reqNames | any(. == $n))
                or
                (($n | test("__ALLOW__"; "i")) and (($n | ascii_downcase | contains("advisory")) | not))
              )
            )
          | $n
        ] | unique | join(", ") ),
    ( [ ( (.statusCheckRollup // [])
          | .[] )
        | (if .__typename == "CheckRun" then (.name // "") else (.context // "") end)
        | select(length > 0)
      ] | unique | join("|||") ),
    ( if ((.mergedAt // "") | length) == 0 then "1"
      elif (.mergedAt < "__CUTOFF__") then "1" else "0" end )
  ] | @tsv'
JQ_ROWS="${JQ_ROWS//__SCAN_N__/$SCAN_N}"
JQ_ROWS="${JQ_ROWS//__REQ_CTX__/$REQ_CTX_JSON}"
JQ_ROWS="${JQ_ROWS//__ALLOW__/$ALLOW_LIST_RE}"
JQ_ROWS="${JQ_ROWS//__CUTOFF__/$ABSENT_CUTOFF_ISO}"
while IFS= read -r row; do
    [ -z "$row" ] && continue
    # Split the 6-field @tsv row by tab MANUALLY (not `IFS=$'\t' read`): tab is
    # IFS-whitespace, so `read` collapses consecutive tabs and DROPS empty middle
    # fields — a row with empty labels but a real red-check ("num⇥mc⇥⇥check")
    # would shift the check into `labels` and blank `redchecks`, silently missing
    # the single most common escape (red required check, no override label).
    # Parameter expansion preserves empty fields. @tsv always emits 6 fields
    # (field 5 = the `|||`-joined PRESENT context names, for the absence checks;
    # field 6 = "1" when the merge is older than the run-creation grace window,
    # i.e. old enough to judge absence — see $ABSENT_GRACE_SECONDS).
    num="${row%%$'\t'*}";         row="${row#*$'\t'}"
    mergecommit="${row%%$'\t'*}"; row="${row#*$'\t'}"
    labels="${row%%$'\t'*}";      row="${row#*$'\t'}"
    redchecks="${row%%$'\t'*}";   row="${row#*$'\t'}"
    present_names="${row%%$'\t'*}"; absent_judgeable="${row#*$'\t'}"
    [ -z "$num" ] && continue
    [ -n "$mergecommit" ] && merged_commits="$merged_commits $mergecommit"
    has_entry "$num" && continue
    trigger=""
    # PRIMARY: lossless ledger first. If this pr+mergeCommit has a snapshot line,
    # the verdict at the merge instant is authoritative — derive the trigger from
    # it and do NOT consult the (lossy) live labels/statusCheckRollup columns.
    if [ -n "$mergecommit" ] && has_snapshot "$num" "$mergecommit"; then
        # The snapshot is the lossless merge-instant authority — take its trigger
        # verbatim and do NOT moot-filter it. The moot-filter consults the LOSSY
        # live rollup, which can read SUCCESS for a check that was red at merge but
        # healed on a post-merge re-run; re-judging a snapshotted override against
        # that would silently DROP a real load-bearing override (false negative —
        # the worse direction for an escape detector). As of the watcher's
        # GATE_SNAPSHOT capture (mandatory-merge-snapshot-on-override-merge,
        # 2026-06-13), an override merge records the bypassed checks IN the snapshot
        # (redChecks names them; a clean merge leaves redChecks=[]), so snapshot_trigger
        # emits a "red-check: <names>" part only when the override was load-bearing —
        # the snapshot self-distinguishes moot from load-bearing without consulting
        # the lossy live rollup. The override-label part still flags regardless (a
        # bypass owes a postmortem); the red-check part adds WHAT it bypassed.
        trigger="$(snapshot_trigger "$num" "$mergecommit")"
    else
        # FALLBACK (documented degraded path): no ledger entry (un-instrumented /
        # pre-ledger merge) → derive from the live (curated) query, then drop moot
        # (non-load-bearing) override parts (the live rollup is the same in-time
        # source the label decision was made against, so moot-filtering here is
        # self-consistent). Keeps red-checks + load-bearing overrides.
        [ -n "$redchecks" ] && trigger="red-check: ${redchecks}"
        if [ -n "$OVERRIDE_LABELS" ] && [ -n "$labels" ]; then
            for lbl in $OVERRIDE_LABELS; do
                case " $labels " in
                    *" $lbl "*) trigger="${trigger:+$trigger; }override: $lbl" ;;
                esac
            done
        fi
        [ -n "$trigger" ] && trigger="$(filter_moot_overrides "$num" "$trigger")"
    fi
    # Broken-lane downgrade (bucket-lane-status-broken-sentinel-auditable, PR-3):
    # a block-scope lane whose harness is DEAD reports RED but is not a regression
    # (status=broken passed=0 failed=N). When the trigger's red-check part is
    # ENTIRELY known-broken lanes (no real red, no override), it is an auditable
    # WARN — the harness needs fixing; nothing merged past a real gate. Emit a WARN
    # line (never silently dropped) and do NOT count it as an owed escape.
    if [ -n "$trigger" ] && is_broken_lane "$trigger"; then
        warns+=("PR #$num — broken-lane (auditable WARN, not an escape): $trigger")
        trigger=""
    fi
    # Required-context ABSENT cross-check (required-check-that-never-reports-is-
    # invisible, infra P1; admin-merge-past-absent-checks-undetected, process P1).
    # Every name in branch_protection.required_contexts MUST appear on a merged
    # PR's rollup: the always-report invariant
    # (docs/agent-rules/ci-required-check-pattern.md) forbids a workflow-level
    # `on.pull_request.paths:` filter on any required check precisely so absence is
    # never legitimate — a required context missing from the rollup means it never
    # reported, and the PR merged past it. That escape emits NO red check, NO
    # override label and NO revert, so it is invisible to every other trigger here;
    # it is exactly how #1941 (`--admin` merge past 22 absent contexts) and
    # #1972-#1974 (merged with zero workflow runs) read as "clean" to this script.
    # Runs on BOTH the snapshot and live paths — a snapshot records the merge-instant
    # red set, not rollup MEMBERSHIP, so absence is only ever observable here.
    # Gated on $absent_judgeable so a merge still inside the run-creation lag window
    # is deferred to the next sweep rather than false-flagged (see
    # $ABSENT_GRACE_SECONDS). Fail-closed: jq absent → REQ_CTX_JSON is '[]' → inert,
    # same degradation as the required-by-name red scope above.
    if [ "$absent_judgeable" = "1" ] && [ "$REQ_CTX_JSON" != "[]" ] && command -v jq >/dev/null 2>&1; then
        req_absent_names=""
        while IFS= read -r reqname; do
            reqname="${reqname%$'\r'}"   # strip a trailing CR (git-bash jq on Windows)
            [ -z "$reqname" ] && continue
            case "|||${present_names}|||" in
                *"|||${reqname}|||"*) ;;   # present (any state — absence is the signal)
                *) req_absent_names="${req_absent_names:+$req_absent_names, }$reqname" ;;
            esac
        done < <(printf '%s\n' "$REQ_CTX_JSON" | jq -r '.[]' 2>/dev/null || true)
        if [ -n "$req_absent_names" ]; then
            trigger="${trigger:+$trigger; }required-absent: ${req_absent_names}"
        fi
    fi
    # Absence-present cross-check (merge-gate-absence-blind-nonrequired-allowlist,
    # PR-3): an allow-listed NON-required check that is configured to always run on
    # develop PRs but is ABSENT from this merged PR's rollup self-disabled the gate
    # (e.g. a PR that deleted its own gating workflow). The live curated path is
    # absence-BLIND for these (only $reqAbsent covers REQUIRED contexts). Cross-check
    # each EXPECTED_PRESENT name against the PR's present-context list (field 5);
    # flag any missing one fail-closed. Skipped on the snapshot path (a snapshot
    # records the merge-instant verdict, not the live rollup membership).
    if [ "$EXPECTED_PRESENT_JSON" != "[]" ] && command -v jq >/dev/null 2>&1; then
        absent_names=""
        while IFS= read -r exp; do
            exp="${exp%$'\r'}"   # strip a trailing CR (git-bash jq output on Windows)
            [ -z "$exp" ] && continue
            # present_names is the `|||`-joined live rollup context names.
            case "|||${present_names}|||" in
                *"|||${exp}|||"*) ;;   # present
                *) absent_names="${absent_names:+$absent_names, }$exp" ;;
            esac
        done < <(printf '%s\n' "$EXPECTED_PRESENT_JSON" | jq -r '.[]' 2>/dev/null || true)
        if [ -n "$absent_names" ]; then
            trigger="${trigger:+$trigger; }absent-allowlisted: ${absent_names}"
        fi
    fi
    # De-noise: Core-cpp-scoped trigger(s) on a PR that touched no Core cpp = false escape.
    if [ -n "$trigger" ] && core_scoped_only_trigger "$trigger" && ! pr_touches_core_cpp "$num"; then
        continue
    fi
    [ -n "$trigger" ] && owed+=("PR #$num — $trigger")
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$FETCH_N" \
            --json number,labels,mergedAt,mergeCommit,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null \
            | tr -d '\r' || true)

# --- Trigger 3: Revert commits on develop ------------------------------------
while IFS= read -r line; do
    [ -z "$line" ] && continue
    sha="${line%% *}"
    subject="${line#* }"
    # `git log --grep='^Revert'` matches the pattern against EVERY line of the
    # message (multiline `^`), so a commit whose *body* merely says "Reverts the
    # index row …" / "Reverted the read-only widget …" is a false positive (seen:
    # #512, #199 — feature/docs PRs with revert prose). A genuine revert commit
    # has a SUBJECT of the form `Revert "<original subject>"` (git revert default).
    # Gate on the subject so prose mentions don't manufacture phantom postmortems.
    case "$subject" in
        Revert\ \"*) : ;;
        *) continue ;;
    esac
    # Try to map to a PR number in the commit subject ("(#N)"); else use sha.
    prnum="$(printf '%s' "$line" | grep -oE '#[0-9]+' | head -1 | tr -d '#' || true)"
    if [ -n "$prnum" ]; then
        has_entry "$prnum" && continue
        owed+=("PR #$prnum — revert: $sha")
    else
        has_sha_entry "$sha" && continue
        owed+=("commit $sha — revert (no PR ref)")
    fi
done < <(git log origin/develop --grep='^Revert' --since='30 days ago' \
            --format='%h %s' 2>/dev/null | tr -d '\r' || true)

# --- Trigger 4: PR-less direct pushes to develop -----------------------------
# A commit on develop with no `(#N)` subject suffix that no merged PR backs
# (`commits/{sha}/pulls == 0`) is a direct push — it bypassed review AND every
# required check, writing no PR + no snapshot, so triggers 1+2 never see it
# (postmortems.md 2026-06-07 direct-push 93c63d0f). The `(#N)`-suffix test is a
# cheap git-log prefilter; the per-sha pulls confirmation is what actually
# distinguishes a direct push from a squash-merge with a non-standard subject
# (NOTE: `commits/{sha}/pulls` is the GitHub associated-PR endpoint — strictly
# more precise than the backlog's suggested `gh pr list --search <sha>`, which
# false-matches any PR merely mentioning the sha). Window-bounded
# (POSTMORTEM_DIRECTPUSH_SINCE, default 7 days) because this repo carries many
# historical direct pushes; a 30-day scan would flood the nudge. A sha already in
# the merged-PR set (trigger 1 fetch) skips the gh call (PR-backed by construction).
DIRECTPUSH_SINCE="${POSTMORTEM_DIRECTPUSH_SINCE:-7 days ago}"
DIRECTPUSH_MAX="${POSTMORTEM_DIRECTPUSH_MAX:-40}"
dp_confirmed=0
dp_seen=0
while IFS=$'\t' read -r sha subject; do
    [ -z "$sha" ] && continue
    # Normal squash-merge subjects end with "(#N)" → not a direct push.
    printf '%s' "$subject" | grep -qE '\(#[0-9]+\)' && continue
    # Already flagged this run (e.g. a direct-pushed revert caught by trigger 3)?
    case " ${owed[*]-} " in *" commit $sha "*) continue ;; esac
    has_sha_entry "$sha" && continue
    # Cheap path: sha is a known mergeCommit from the trigger-1 fetch → PR-backed.
    # git log gives a SHORT %h; gh pr list gives the FULL 40-char oid — match by
    # prefix (no trailing space in the glob) so the short sha matches the full oid.
    case " $merged_commits " in *" $sha"*) continue ;; esac
    dp_seen=$((dp_seen + 1))
    if [ "$dp_seen" -gt "$DIRECTPUSH_MAX" ]; then
        echo "postmortem-owed: trigger-4 suspect cap ($DIRECTPUSH_MAX) reached; older direct-push candidates not confirmed this run (raise POSTMORTEM_DIRECTPUSH_MAX or narrow POSTMORTEM_DIRECTPUSH_SINCE)." >&2
        break
    fi
    # Confirm not PR-backed. On a gh error (count "?") flag conservatively — a
    # no-(#N) develop commit is rare and a direct push is the highest-trust
    # escape, so over-nudging a rare ambiguous case beats missing a real one.
    cnt="$(gh api "repos/$REPO/commits/$sha/pulls" --jq 'length' 2>/dev/null || echo '?')"
    if [ "$cnt" = "0" ] || [ "$cnt" = "?" ]; then
        owed+=("commit $sha — direct push (no PR/CI/CR)")
        dp_confirmed=$((dp_confirmed + 1))
    fi
done < <(git log origin/develop --no-merges --since="$DIRECTPUSH_SINCE" \
            --format='%h%x09%s' 2>/dev/null | tr -d '\r' || true)

# --- Emit --------------------------------------------------------------------
# Auditable WARNs (broken-lane downgrades) go to stderr ALWAYS — they are never an
# owed escape but must never be silently dropped (bucket-lane-status-broken-
# sentinel-auditable). They do NOT affect the owed exit-state.
if [ "${#warns[@]}" -gt 0 ]; then
    for w in "${warns[@]}"; do
        echo "postmortem-owed: WARN — $w" >&2
    done
fi

if [ "${#owed[@]}" -eq 0 ]; then
    # Qualify the CLEAN result specifically (gate-tooling-run-from-stale-session-
    # branch, process P1). "no gate escapes owed" from a checkout running months-old
    # detector logic is a false GREEN — the exact reading that let #1941 pass as
    # clean — and unlike a stale BLOCK nobody re-examines it. Advisory: never
    # changes the exit code, and silent unless the logic is verifiably behind.
    # warn_if_script_stale comes from lib/script-freshness.sh via the merge-gates.sh
    # source above; guarded so an older/partial checkout degrades rather than errors.
    if command -v warn_if_script_stale >/dev/null 2>&1; then
        case "$MODE" in
            list|blocking)
                warn_if_script_stale "postmortem-owed detector logic" \
                    "agents/scripts/core/postmortem-owed.sh" \
                    "agents/scripts/core/lib/script-freshness.sh"
                ;;
        esac
    fi
    case "$MODE" in
        list|blocking) echo "postmortem-owed: no gate escapes owed a postmortem (last $SCAN_N merges clean)." ;;
    esac
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === postmortem owed (${#owed[@]}) ==="
    echo "Gate escape(s) detected on recent develop merges. Each owes a blameless"
    echo "postmortem (the \`gate-escape-postmortem\` skill) whose mandatory"
    echo "\`### Preventing gate\` field files a new gate into the self-improvement loop:"
    for o in "${owed[@]}"; do echo "  - $o"; done
    exit 0
fi

# --list / --blocking share the plain-line output.
for o in "${owed[@]}"; do echo "postmortem owed: $o"; done

if [ "$MODE" = "blocking" ] && [ "${#owed[@]}" -gt "$BLOCKING_GRACE" ]; then
    echo "postmortem-owed: ${#owed[@]} escape(s) owed a postmortem exceeds grace ($BLOCKING_GRACE) — blocking." >&2
    exit 1
fi
exit 0
