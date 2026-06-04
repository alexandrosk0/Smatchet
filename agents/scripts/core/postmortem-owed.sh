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
    grep -qE "PR #$1([^0-9]|$)|commit $1([^0-9A-Fa-f]|$)" "$LEDGER"
}

owed=()   # "PR #N — <trigger>"

# --- Trigger 1 + 2: per merged PR (checks + labels), ONE batched gh call ------
# `gh pr list --json statusCheckRollup` returns every PR's checks in a single
# API call — fast enough for a SessionStart hook (no per-PR `gh pr view` loop).
# Each row: number <TAB> space-joined-labels <TAB> comma-joined-red-checks.
# shellcheck disable=SC2016  # $c is a jq variable, not a shell expansion
JQ_ROWS='.[] | [
    (.number|tostring),
    ([.labels[].name] | join(" ")),
    ([.statusCheckRollup[]? | ((.conclusion // .state)) as $c
      | select($c != null and $c != "SUCCESS" and $c != "SKIPPED" and $c != "NEUTRAL")
      | (.name // .context)] | unique | join(", "))
  ] | @tsv'
while IFS=$'\t' read -r num labels redchecks; do
    [ -z "$num" ] && continue
    has_entry "$num" && continue
    trigger=""
    [ -n "$redchecks" ] && trigger="red-check: ${redchecks}"
    if [ -n "$OVERRIDE_LABELS" ] && [ -n "$labels" ]; then
        for lbl in $OVERRIDE_LABELS; do
            case " $labels " in
                *" $lbl "*) trigger="${trigger:+$trigger; }override: $lbl" ;;
            esac
        done
    fi
    [ -n "$trigger" ] && owed+=("PR #$num — $trigger")
done < <(gh pr list --repo "$REPO" --base develop --state merged --limit "$SCAN_N" \
            --json number,labels,statusCheckRollup --jq "$JQ_ROWS" 2>/dev/null || true)

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
