#!/usr/bin/env bash
# sync-settings-hooks.sh — heal a provisioned .claude/settings.json by bringing
# any hook present in the TEMPLATE but MISSING from the deployed file into the
# matching event/matcher group, WITHOUT touching existing hooks, their order,
# or the user's `permissions` block.
#
# Why: setup-harness.sh's copy_template() refuses to overwrite a user-modified
# settings.json (all-or-nothing skip), and the SessionStart sync copies only
# hook *scripts*, never settings.json — so a NEW governance hook added to the
# template (e.g. postmortem-owed.sh --nudge) silently never reaches an already-
# provisioned adapter. This is the additive, non-destructive heal for that gap.
#
# Additive-only by design: it never removes a hook the template dropped, never
# reorders, never edits a hook whose command changed — those would risk
# clobbering a per-machine customisation (the exact failure copy_template's skip
# avoids). Template renames/removals still need a manual re-provision.
#
# The additive-only design has one blind spot (hooks-session-lifecycle-03): a hook
# the template REMOVED or RENAMED lingers in the deployed file forever — the sync
# never surfaces it. `--check` is the read-only audit for that: it lists every
# deployed hook command with no counterpart in the template (a candidate stale /
# renamed hook), so an operator can decide whether to re-provision. It never
# writes.
#
# Usage:  sync-settings-hooks.sh <template.json> <deployed.json>       (additive sync)
#         sync-settings-hooks.sh --check <template.json> <deployed.json> (read-only audit)
# Exit 0 always (advisory; jq absent / jq error -> WARN + leave file unchanged).

set -euo pipefail

MODE="sync"
if [ "${1:-}" = "--check" ]; then
    MODE="check"
    shift
fi

tmpl="${1:-}"
dst="${2:-}"
[ -n "$tmpl" ] && [ -n "$dst" ] || { echo "usage: sync-settings-hooks.sh [--check] <template> <deployed>" >&2; exit 2; }
[ -f "$tmpl" ] && [ -f "$dst" ] || exit 0  # nothing to sync

if ! command -v jq >/dev/null 2>&1; then
    echo "  WARN  jq not found — cannot sync settings hooks ($dst); install jq + re-run setup-harness" >&2
    exit 0
fi

# --check: report deployed hook commands absent from the template (removed /
# renamed hooks the additive sync can't propagate), then exit. Read-only.
if [ "$MODE" = "check" ]; then
    absent="$(jq -rn --slurpfile dd "$dst" --slurpfile tt "$tmpl" '
        ($tt[0].hooks // {} | [.[][]?.hooks[]?.command]) as $tmplcmds
        | ($dd[0].hooks // {} | to_entries[]
           | .key as $ev | (.value[]? | (.matcher // "*") as $m | .hooks[]?
             | select((.command) as $c | ($tmplcmds | index($c)) == null)
             | "  \($ev)/\($m): \(.command)"))
    ' 2>/dev/null || true)"
    if [ -n "$absent" ]; then
        echo "sync-settings-hooks --check: deployed hook(s) with NO template counterpart (removed/renamed — re-provision to drop):"
        printf '%s\n' "$absent"
    else
        echo "sync-settings-hooks --check: OK — no deployed hooks absent from the template ($dst)."
    fi
    exit 0
fi

# Merge program: for each event→group in the template, either append a missing
# matcher-group whole, or (matcher already present) append only the template
# hooks whose `.command` is not already in that group. Keyed on command string
# so re-runs are idempotent; only `.hooks[<event>]` is ever assigned, so the
# top-level `permissions` block and key order survive untouched.
tmp="$(mktemp "${dst}.XXXXXX")" || exit 0
if jq -n --slurpfile dd "$dst" --slurpfile tt "$tmpl" '
      $dd[0] as $D | $tt[0] as $T
      | $D
      | reduce ($T.hooks | to_entries[]) as $ev (.;
          reduce $ev.value[] as $g (.;
            ($g.matcher // "") as $m
            | (.hooks[$ev.key] // []) as $groups
            | if ($groups | map(select((.matcher // "") == $m)) | length) == 0
              then .hooks[$ev.key] = ($groups + [$g])
              else .hooks[$ev.key] = ($groups | map(
                     if (.matcher // "") == $m
                     then (.hooks | map(.command)) as $have
                          | .hooks += ($g.hooks | map(select((.command) as $c | ($have | index($c)) == null)))
                     else . end))
              end))
    ' >"$tmp" 2>/dev/null && [ -s "$tmp" ]; then
    if cmp -s "$tmp" "$dst"; then
        rm -f "$tmp"          # already in sync — no write
    else
        mv "$tmp" "$dst"      # atomic replace (same dir)
        echo "  sync  $dst (added missing template hooks)"
    fi
else
    rm -f "$tmp"
    echo "  WARN  settings-hook sync failed (jq error) — $dst left unchanged" >&2
fi
exit 0
