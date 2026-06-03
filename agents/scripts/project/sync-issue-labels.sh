#!/usr/bin/env bash
# sync-issue-labels.sh — reconcile the GitHub Issue label set from a checked-in
# manifest (agents/scripts/project/issue-labels.manifest), so the label set is
# reproducible and the area:<subsystem> labels stay in parity with the
# coderabbit-triage path→agent map.
#
# Issue-triage protocol (ADR-0014): docs/agent-rules/issue-triage.md § Labels.
# Plan: docs/plans/active/issue-triage-protocol.md § C.
#
# Usage:
#   bash agents/scripts/project/sync-issue-labels.sh                # dry-run (default): print the plan
#   bash agents/scripts/project/sync-issue-labels.sh --apply        # create/update labels via gh
#   bash agents/scripts/project/sync-issue-labels.sh --check-parity # area:* ↔ coderabbit-triage map
#
# Env:
#   REPO   — owner/repo (default: derived from `gh repo view`, else alexandrosk0/Smatchet)
#
# Exit codes (test-author convention):
#   0 — dry-run printed / apply succeeded / parity OK
#   1 — parity drift (an area:* has no matching agent + isn't a generic area)
#   2 — bad args / missing manifest / gh missing

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

# Paths are env-overridable so the bats harness can point --check-parity at fixtures.
MANIFEST="${ISSUE_LABELS_MANIFEST:-agents/scripts/project/issue-labels.manifest}"
TRIAGE_MAP="${ISSUE_TRIAGE_MAP:-agents/core/coderabbit-triage.md}"
# Generic area:<x> labels that intentionally have no 1:1 agent in the triage map.
GENERIC_AREAS=" ui build "

MODE="dry-run"
case "${1:-}" in
    --apply)        MODE="apply" ;;
    --check-parity) MODE="parity" ;;
    "")             MODE="dry-run" ;;
    *) echo "usage: $0 [--apply|--check-parity]" >&2; exit 2 ;;
esac

[ -f "$MANIFEST" ] || { echo "sync-issue-labels: missing $MANIFEST" >&2; exit 2; }

# Read the manifest into parallel arrays (name / color / desc), skipping # + blanks.
names=(); colors=(); descs=()
while IFS='|' read -r name color desc; do
    case "$name" in ''|\#*) continue ;; esac
    name="$(printf '%s' "$name" | sed 's/[[:space:]]*$//')"
    [ -n "$name" ] || continue
    names+=("$name"); colors+=("$color"); descs+=("$desc")
done < "$MANIFEST"

# --- parity check: every area:<x> resolves to an agent (or a generic area) ---
if [ "$MODE" = "parity" ]; then
    [ -f "$TRIAGE_MAP" ] || { echo "sync-issue-labels: missing $TRIAGE_MAP" >&2; exit 2; }
    miss=0
    for n in "${names[@]}"; do
        case "$n" in area:*) ;; *) continue ;; esac
        sub="${n#area:}"
        case "$GENERIC_AREAS" in *" $sub "*) continue ;; esac
        # The map cites the agent as `\`<agent>\`` in the right column.
        if ! grep -qF "\`$sub\`" "$TRIAGE_MAP"; then
            echo "PARITY FAIL: $n has no matching agent in $TRIAGE_MAP (add the agent, the label, or list '$sub' in GENERIC_AREAS)" >&2
            miss=1
        fi
    done
    if [ "$miss" -eq 0 ]; then
        echo "sync-issue-labels: parity OK — every area:* maps to an agent or a generic area (${#names[@]} labels)"
        exit 0
    fi
    exit 1
fi

command -v gh >/dev/null 2>&1 || { echo "sync-issue-labels: gh required" >&2; exit 2; }
REPO="${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || echo alexandrosk0/Smatchet)}"

# Snapshot existing labels (name<TAB>color<TAB>description).
existing="$(gh label list --repo "$REPO" --limit 200 \
    --json name,color,description \
    --jq '.[] | [.name, .color, .description] | @tsv' 2>/dev/null || true)"

creates=0; updates=0; nochange=0
for i in "${!names[@]}"; do
    n="${names[$i]}"; c="${colors[$i]}"; d="${descs[$i]}"
    line="$(printf '%s\n' "$existing" | awk -F'\t' -v n="$n" '$1==n {print; exit}')"
    if [ -z "$line" ]; then
        echo "  CREATE  $n  (#$c)  $d"
        creates=$((creates+1))
        if [ "$MODE" = "apply" ]; then
            gh label create "$n" --repo "$REPO" --color "$c" --description "$d" --force >/dev/null \
                || echo "sync-issue-labels: WARN create '$n' failed" >&2
        fi
    else
        cur_color="$(printf '%s' "$line" | cut -f2)"
        cur_desc="$(printf '%s' "$line" | cut -f3)"
        if [ "$cur_color" != "$c" ] || [ "$cur_desc" != "$d" ]; then
            echo "  UPDATE  $n  (#$cur_color→#$c)"
            updates=$((updates+1))
            if [ "$MODE" = "apply" ]; then
                gh label edit "$n" --repo "$REPO" --color "$c" --description "$d" >/dev/null \
                    || echo "sync-issue-labels: WARN edit '$n' failed" >&2
            fi
        else
            nochange=$((nochange+1))
        fi
    fi
done

verb=$([ "$MODE" = "apply" ] && echo "applied" || echo "would apply (dry-run; pass --apply)")
echo "sync-issue-labels: $verb — $creates create, $updates update, $nochange unchanged (repo $REPO)"
exit 0
