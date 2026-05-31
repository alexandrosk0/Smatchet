#!/usr/bin/env bash
# lock-staleness-sweep.sh — sweep refs/locks/* for stale claims and surface
# them as GitHub Issues. Invoked by .github/workflows/lock-staleness.yml.
#
# Phase 4 of git-ref-plan-locks. The action never deletes a ref — silent
# deletion would break coordination with the slice that claimed the lock.
# An Issue surfaces the stale ref so a human (or the plan owner) can
# decide between rescue / abandon / extend.
#
# Staleness is computed against `max(claim.started, claim.updated)` so
# locks that have been touched recently are not flagged even if the
# original claim is old.
#
# Hosted here (instead of inline in the workflow YAML) because the
# original inline-Python heredocs broke YAML block-scalar parsing — see
# fix/lock-staleness-yaml-parse and the Phase 4 deviations log in
# docs/plans/shipped/git-ref-plan-locks.md.
#
# Required environment (provided by the workflow):
#   REPO              — e.g. "alexandrosk0/Smatchet"
#   THRESHOLD_DAYS    — integer (default 14)
#   GH_TOKEN          — gh CLI auth (default GITHUB_TOKEN under Actions)
#
# Exit codes:
#   0 — sweep finished (with or without findings)
#   1 — required env var missing
#   2 — refs/locks/* could not be enumerated
#
# Side effects:
#   - May open or edit GitHub Issues titled `Stale plan-lock: <slug>`
#     with the `plan-lock-stale` label.
#   - Never deletes refs.

set -euo pipefail

command -v gh >/dev/null 2>&1 || { echo "gh required" >&2; exit 2; }

: "${REPO:?REPO env var required (e.g. alexandrosk0/Smatchet)}"
: "${THRESHOLD_DAYS:=14}"
: "${GH_TOKEN:?GH_TOKEN env var required}"

PYBIN="${PYBIN:-}"
if [ -z "$PYBIN" ]; then
    for candidate in python python3; do
        if command -v "$candidate" >/dev/null 2>&1 && \
           "$candidate" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PYBIN="$candidate"
            break
        fi
    done
fi
[ -n "$PYBIN" ] || { echo "lock-staleness-sweep: python3 (or python) is required" >&2; exit 1; }

HELPER="$(dirname "$0")/_lock-json.py"
[ -f "$HELPER" ] || { echo "lock-staleness-sweep: helper not found at $HELPER" >&2; exit 1; }

NOW_EPOCH=$(date -u +%s)
THRESHOLD_SECS=$((THRESHOLD_DAYS * 86400))

echo "::notice::Sweep starting; threshold=${THRESHOLD_DAYS} days (${THRESHOLD_SECS} s); repo=${REPO}."

# Enumerate the refs locally — workflow's earlier "Fetch refs/locks/*" step
# populated refs/locks/* in the local repo.
refs=$(git for-each-ref --format='%(refname)' refs/locks/ 2>/dev/null || true)
if [ -z "$refs" ]; then
    echo "::notice::No refs/locks/* present; nothing to sweep."
    exit 0
fi

stale_count=0
fresh_count=0

for ref in $refs; do
    slug=${ref#refs/locks/}

    # Read claim.json (tolerate parse errors).
    claim=$(git cat-file blob "${ref}:claim.json" 2>/dev/null || echo '{}')

    # max(started, updated) via the helper.
    latest_ts=$(printf '%s' "$claim" | "$PYBIN" "$HELPER" latest-ts 2>/dev/null || true)
    latest_ts=${latest_ts%$'\n'}

    if [ -z "$latest_ts" ]; then
        echo "::warning::${ref} claim.json has no started/updated timestamp; skipping."
        continue
    fi

    # ISO-8601 -> epoch via the helper (env-var passthrough, no source interp).
    ts_epoch=$(LATEST_TS="$latest_ts" "$PYBIN" "$HELPER" iso-to-epoch 2>/dev/null || true)
    ts_epoch=${ts_epoch%$'\n'}

    if [ -z "$ts_epoch" ]; then
        echo "::warning::${ref} timestamp '${latest_ts}' unparseable; skipping."
        continue
    fi

    age_secs=$((NOW_EPOCH - ts_epoch))
    age_days=$((age_secs / 86400))

    if [ "$age_secs" -lt "$THRESHOLD_SECS" ]; then
        fresh_count=$((fresh_count + 1))
        echo "fresh: ${slug} age=${age_days}d"
        continue
    fi

    stale_count=$((stale_count + 1))
    echo "::warning::STALE: ${slug} age=${age_days}d (latest=${latest_ts})"

    # Human-facing claim context.
    owner=$(printf '%s' "$claim" | "$PYBIN" "$HELPER" read-field owner)
    [ -n "$owner" ] || owner="?"
    branch=$(printf '%s' "$claim" | "$PYBIN" "$HELPER" read-field branch)
    [ -n "$branch" ] || branch="?"
    plan=$(printf '%s' "$claim" | "$PYBIN" "$HELPER" read-field originating_plan)

    # write_set length — read-field returns a JSON array; pipe through helper
    # to count. Simpler path: re-grep claim for path lines via Python directly.
    ws_count=$(printf '%s' "$claim" | "$PYBIN" -c '
import json, sys
try:
    print(len(json.loads(sys.stdin.read() or "{}").get("write_set") or []))
except Exception:
    print(0)
')

    title="Stale plan-lock: ${slug}"

    # Look up an existing open Issue with this exact title.
    existing=$(gh issue list \
        --repo "$REPO" \
        --state open \
        --search "in:title \"${title}\"" \
        --json number,title \
        --jq ".[] | select(.title == \"${title}\") | .number" \
        | head -n1 || true)

    body_file=$(mktemp)
    {
        echo "Plan-lock \`${slug}\` has been in-flight for **${age_days} days** without an \`updated\` timestamp newer than the staleness threshold (${THRESHOLD_DAYS} days)."
        echo
        echo "| Field | Value |"
        echo "|---|---|"
        echo "| Slug | \`${slug}\` |"
        echo "| Owner | \`${owner}\` |"
        echo "| Branch | \`${branch}\` |"
        if [ -n "$plan" ]; then
            echo "| Originating plan | [\`${plan}\`](../blob/develop/${plan}) |"
        fi
        echo "| Latest activity | \`${latest_ts}\` |"
        echo "| Age | ${age_days} days |"
        echo "| Write-set size | ${ws_count} paths |"
        echo
        echo "## What to do"
        echo
        echo "Pick one:"
        echo
        echo "1. **Slice is still active** — bump the lock with \`bash agents/scripts/core/lock-claim-update.sh ${slug} <write-set-file>\` to refresh \`updated\`. This Issue will close automatically on the next sweep."
        echo "2. **Slice is abandoned** — \`bash agents/scripts/core/lock-release.sh ${slug}\` to delete the ref. Close this Issue."
        echo "3. **Slice has merged** — the PR was missing a \`lock-slug: ${slug}\` line in its body. \`bash agents/scripts/core/lock-release.sh ${slug}\` and close this Issue. Add the line to future PRs holding a lock."
        echo
        echo "Live ref state: \`bash agents/scripts/core/locks-show.sh\`."
        echo "Plan: [\`docs/plans/shipped/git-ref-plan-locks.md\`](../blob/develop/docs/plans/shipped/git-ref-plan-locks.md)."
        echo
        echo "_Auto-generated by \`.github/workflows/lock-staleness.yml\` at $(date -u +%Y-%m-%dT%H:%M:%SZ)._"
    } > "$body_file"

    if [ -n "$existing" ]; then
        echo "::notice::Updating Issue #${existing} for ${slug}."
        gh issue edit "$existing" \
            --repo "$REPO" \
            --body-file "$body_file"
    else
        echo "::notice::Opening new Issue for ${slug}."
        gh issue create \
            --repo "$REPO" \
            --title "$title" \
            --body-file "$body_file" \
            --label "plan-lock-stale"
    fi

    rm -f "$body_file"
done

echo "::notice::Sweep finished. stale=${stale_count}, fresh=${fresh_count}."
