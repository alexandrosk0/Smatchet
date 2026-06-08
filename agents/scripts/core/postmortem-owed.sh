#!/usr/bin/env bash
# postmortem-owed.sh — detect gate escapes on recent `develop` merges and nudge
# for a blameless postmortem (see docs/plans/shipped/gate-escape-postmortem.md +
# the gate-escape-postmortem skill). A gate escape = something shipped that a
# gate should have caught; the "gate, don't trust" response is a NEW gate, filed
# via the postmortem's mandatory `### Preventing gate` field.
#
# Triggers (an escape lacking a postmortems.md entry referencing its PR):
#   1. non-SUCCESS check on the merged head (PRIMARY) — a check (often a
#      NON-required CI job) that was red yet merged. The most common escape.
#   2. override label on the merged PR (project.config.json merge_gates.override_labels).
#   3. a `Revert` commit on develop (a post-merge undo).
#   (overdue SMATCHET_DEVIATION is covered by the strict `deviation-overdue` lint.)
#
# Modes:
#   --list   (default) plain "postmortem owed: PR #N — <trigger>" lines.
#   --nudge  SessionStart-formatted block (silent when nothing is owed).
#
# Mirrors memory-drain-nudge.sh: deterministic check -> nudge, no investigation.
# Advisory — never blocks. Exit 0 always (even without gh; degrades to a notice).

set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: postmortem-owed.sh [--list|--nudge]" >&2; exit 2 ;;
esac

REPO="${REPO:-alexandrosk0/Smatchet}"
LEDGER="docs/self-improvement/postmortems.md"
SCAN_N="${POSTMORTEM_SCAN_N:-20}"

# gh is required; without it, degrade to a quiet notice (advisory tool).
if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
    [ "$MODE" = "list" ] && echo "postmortem-owed: gh unavailable/unauthenticated — skipped (advisory)" >&2
    exit 0
fi

# Override-label set (config-sourced).
OVERRIDE_LABELS=""
# shellcheck source=scripts/dev/project-config.sh
if . scripts/dev/project-config.sh >/dev/null 2>&1; then
    OVERRIDE_LABELS="${PC_OVERRIDE_LABELS:-}"
fi

# Already has a postmortem? (ledger references "PR #<n>")
has_entry() {
    [ -f "$LEDGER" ] || return 1
    # Match either a "PR #N" reference or a "commit <sha>" reference, so the
    # commit-only revert path (which passes a sha) dedupes too.
    grep -qE "PR #$1([^0-9]|$)|commit $1([^0-9A-Fa-f]|$)" "$LEDGER" && return 0
    # Combined-PR postmortem: one blameless RCA can cover several PRs in a single
    # heading written `PR #A, #B, #C` — only the first carries the literal `PR #`
    # prefix; the rest are bare `, #N`. Match #N inside such a heading line (scoped
    # to `^#+ … PR #…` so a #N mention in prose body can't false-suppress a real owe).
    grep -qE "^#+ .*PR #[0-9].*[,[:space:]]#$1([^0-9]|$)" "$LEDGER"
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

owed=()   # "PR #N — <trigger>"

# --- Trigger 1 + 2: per merged PR (checks + labels), ONE batched gh call ------
# `gh pr list --json statusCheckRollup` returns every PR's checks in a single
# API call — fast enough for a SessionStart hook (no per-PR `gh pr view` loop).
# Each row: number <TAB> mergeCommit-oid <TAB> space-joined-labels <TAB>
# comma-joined-red-checks. The mergeCommit column lets us key the lossless
# snapshot-ledger lookup (snapshot_trigger) on pr+mergeCommit before falling
# back to the (lossy) live labels/statusCheckRollup columns.
# `gh pr list` has no mergedAt sort key, so over-fetch by its default
# createdAt-desc order, then re-sort by mergedAt and keep the most-recently
# MERGED SCAN_N. Without this, a long-lived branch created early but merged
# late falls outside a createdAt-ordered window and its escape is never seen.
FETCH_N="${POSTMORTEM_FETCH_N:-$((SCAN_N * 3))}"
# shellcheck disable=SC2016  # $c is a jq variable, not a shell expansion
JQ_ROWS='(sort_by(.mergedAt) | reverse | .[0:'"$SCAN_N"']) | .[] | [
    (.number|tostring),
    (.mergeCommit.oid // ""),
    ([.labels[].name] | join(" ")),
    ([.statusCheckRollup[]? | ((.conclusion // .state)) as $c
      | select($c != null and $c != "SUCCESS" and $c != "SKIPPED" and $c != "NEUTRAL")
      | (.name // .context)] | unique | join(", "))
  ] | @tsv'
while IFS=$'\t' read -r num mergecommit labels redchecks; do
    [ -z "$num" ] && continue
    has_entry "$num" && continue
    trigger=""
    # PRIMARY: lossless ledger first. If this pr+mergeCommit has a snapshot line,
    # the verdict at the merge instant is authoritative — derive the trigger from
    # it and do NOT consult the (lossy) live labels/statusCheckRollup columns.
    if [ -n "$mergecommit" ] && has_snapshot "$num" "$mergecommit"; then
        trigger="$(snapshot_trigger "$num" "$mergecommit")"
    else
        # FALLBACK (documented degraded path): no ledger entry (un-instrumented /
        # pre-ledger merge) → derive from the live query as before.
        [ -n "$redchecks" ] && trigger="red-check: ${redchecks}"
        if [ -n "$OVERRIDE_LABELS" ] && [ -n "$labels" ]; then
            for lbl in $OVERRIDE_LABELS; do
                case " $labels " in
                    *" $lbl "*) trigger="${trigger:+$trigger; }override: $lbl" ;;
                esac
            done
        fi
    fi
    [ -n "$trigger" ] && owed+=("PR #$num — $trigger")
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$FETCH_N" \
            --json number,labels,mergedAt,mergeCommit,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null || true)

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
        has_entry "$sha" && continue
        owed+=("commit $sha — revert (no PR ref)")
    fi
done < <(git log origin/develop --grep='^Revert' --since='30 days ago' \
            --format='%h %s' 2>/dev/null || true)

# --- Emit --------------------------------------------------------------------
if [ "${#owed[@]}" -eq 0 ]; then
    [ "$MODE" = "list" ] && echo "postmortem-owed: no gate escapes owed a postmortem (last $SCAN_N merges clean)."
    exit 0
fi

if [ "$MODE" = "nudge" ]; then
    echo "## === postmortem owed (${#owed[@]}) ==="
    echo "Gate escape(s) detected on recent develop merges. Each owes a blameless"
    echo "postmortem (the \`gate-escape-postmortem\` skill) whose mandatory"
    echo "\`### Preventing gate\` field files a new gate into the self-improvement loop:"
    for o in "${owed[@]}"; do echo "  - $o"; done
else
    for o in "${owed[@]}"; do echo "postmortem owed: $o"; done
fi
exit 0
