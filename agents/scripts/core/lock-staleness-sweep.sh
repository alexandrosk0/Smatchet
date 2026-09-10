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
# Environment (the workflow provides all three; in a local gh-authed shell
# REPO + GH_TOKEN are OPTIONAL — both fall back to gh's ambient auth so the
# sweep is runnable from a normal session, e.g. git-janitor's pre-flight,
# without manual token plumbing):
#   REPO              — e.g. "alexandrosk0/Smatchet"; if unset, resolved via
#                       `gh repo view --json nameWithOwner` (current repo).
#   THRESHOLD_DAYS    — integer (default 14)
#   GH_TOKEN          — gh CLI auth (default GITHUB_TOKEN under Actions); if
#                       unset, falls back to gh's stored auth (`gh auth status`).
#   LOCK_REMOTE       — git remote holding refs/locks/* (default: origin); same
#                       knob the other lock-* scripts read.
#
# Exit codes:
#   0 — sweep finished (with or without findings)
#   1 — repo unresolvable or gh unauthenticated
#   2 — refs/locks/* could not be enumerated
#
# Side effects:
#   - May open or edit GitHub Issues titled `Stale plan-lock: <slug>`
#     with the `plan-lock-stale` label.
#   - May CLOSE such an Issue once its lock is no longer stale — the ref was
#     refreshed (bumped back under the threshold) or released. Without this the
#     Issue body's "will close automatically on the next sweep" promise was
#     false and every resolved lock left an Issue open forever.
#   - Never deletes refs.

set -euo pipefail

command -v gh >/dev/null 2>&1 || { echo "gh required" >&2; exit 2; }

# REPO: the workflow sets it explicitly; in a local shell fall back to the repo
# gh resolves from the current directory's remote (ambient auth) so the sweep
# isn't unrunnable without manual env plumbing (git-janitor's pre-flight already
# authenticates gh).
REPO="${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || true)}"
[ -n "$REPO" ] || { echo "lock-staleness-sweep: REPO unset and 'gh repo view' could not resolve it (run from the repo dir with gh authed, or set REPO=owner/name)" >&2; exit 1; }

: "${THRESHOLD_DAYS:=14}"

# GH_TOKEN: under Actions the workflow provides it; in a local shell gh uses its
# own stored credentials, so don't hard-require the env var — only fail when gh
# is not authenticated at all.
if [ -z "${GH_TOKEN:-}" ] && ! gh auth status >/dev/null 2>&1; then
    echo "lock-staleness-sweep: no GH_TOKEN and gh is not authenticated (run 'gh auth login' or set GH_TOKEN)" >&2
    exit 1
fi

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

# Confirm the local refs/locks/* namespace mirrors the remote before trusting
# it. The workflow's earlier "Fetch refs/locks/*" step already did this (and
# fails loud, deliberately), so in CI this is a cheap idempotent no-op. It
# matters on the LOCAL path (git-janitor's pre-flight): there, a never-fetched
# namespace also enumerates as zero refs, and the reconcile below cannot tell
# "every lock was released" from "I never looked" — it would close every open
# Issue. Only a confirmed-authoritative namespace earns the right to close.
# Non-fatal: flagging still runs against whatever refs are present, exactly as
# before; only the destructive half is withheld.
locks_authoritative=0
if git fetch --quiet --prune "${LOCK_REMOTE:-origin}" '+refs/locks/*:refs/locks/*' 2>/dev/null; then
    locks_authoritative=1
else
    echo "::warning::Could not refresh refs/locks/* from '${LOCK_REMOTE:-origin}'; will flag stale locks but skip Issue auto-close this run."
fi

# Enumerate the refs locally.
refs=$(git for-each-ref --format='%(refname)' refs/locks/ 2>/dev/null || true)
if [ -z "$refs" ]; then
    # NOT an early exit: zero refs is exactly the state left behind when every
    # lock has been released, and those releases are what the reconcile step at
    # the bottom has to close Issues for. The loop below is a no-op on an empty
    # `refs`, so just fall through.
    echo "::notice::No refs/locks/* present; nothing to sweep."
fi

stale_count=0
fresh_count=0
# Newline-delimited set of slugs flagged stale on THIS run. Any open
# `plan-lock-stale` Issue whose slug is absent from this set is resolved and
# gets closed by the reconcile step below.
stale_slugs=""

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
    stale_slugs="${stale_slugs}${slug}
"
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
        echo "2. **Slice is abandoned** — \`bash agents/scripts/core/lock-release.sh ${slug}\` to delete the ref. This Issue will close automatically on the next sweep."
        echo "3. **Slice has merged** — the PR was missing a \`lock-slug: ${slug}\` line in its body. \`bash agents/scripts/core/lock-release.sh ${slug}\` to delete the ref; this Issue will close automatically on the next sweep. Add the line to future PRs holding a lock."
        echo
        echo "No local ref-write access (e.g. a CI-scoped token)? Land a PR whose body carries a \`lock-slug: ${slug}\` line — \`.github/workflows/lock-cleanup.yml\` releases the ref when that PR closes."
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

# --- Reconcile: close Issues whose lock is no longer stale -------------------
#
# The three remedies the Issue body offers all converge here: a bumped lock is
# fresh again, and a released lock has no ref at all. Either way the slug is
# absent from `stale_slugs` and its Issue is obsolete, so close it.
#
# Best-effort by design: this is cleanup, not the sweep's primary job, so a gh
# failure warns and the sweep still exits 0. Flagging stale locks (above) has
# already happened and must not be undone by a listing hiccup.
closed_count=0
issue_listing=""
if [ "$locks_authoritative" -eq 1 ]; then
    # --limit well above the plausible number of concurrent locks; gh's default
    # of 30 would silently leave the overflow open forever.
    issue_listing=$(gh issue list \
        --repo "$REPO" \
        --state open \
        --label "plan-lock-stale" \
        --limit 200 \
        --json number,title \
        --jq '.[] | "\(.number)\t\(.title)"' 2>/dev/null || true)
fi

# Heredoc rather than a pipe so `closed_count` survives the loop (a pipeline
# would run the body in a subshell).
while IFS=$'\t' read -r issue_num issue_title; do
    [ -n "$issue_num" ] || continue

    # gh emits plain integers here, but this value becomes a `gh issue close`
    # argument — validate rather than trust the field's shape.
    case "$issue_num" in
        ''|*[!0-9]*) continue ;;
    esac

    # Only touch Issues this sweep owns the title format of. A human-filed
    # Issue that merely carries the label is left alone.
    case "$issue_title" in
        "Stale plan-lock: "*) ;;
        *) continue ;;
    esac
    issue_slug=${issue_title#"Stale plan-lock: "}

    # Same grammar lock-claim.sh enforces; anything else is not ours to close.
    printf '%s' "$issue_slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$' || continue

    # Still stale this run -> the Issue is current, leave it open.
    if printf '%s' "$stale_slugs" | grep -qFx "$issue_slug"; then
        continue
    fi

    echo "::notice::Closing Issue #${issue_num}: plan-lock '${issue_slug}' is no longer stale."
    if gh issue close "$issue_num" \
        --repo "$REPO" \
        --comment "Plan-lock \`${issue_slug}\` is no longer stale — the ref was refreshed or released. Closed automatically by \`.github/workflows/lock-staleness.yml\`." \
        >/dev/null 2>&1; then
        closed_count=$((closed_count + 1))
    else
        echo "::warning::Could not close Issue #${issue_num} for '${issue_slug}'; leaving it open."
    fi
done <<EOF
${issue_listing}
EOF

echo "::notice::Sweep finished. stale=${stale_count}, fresh=${fresh_count}, closed=${closed_count}."
