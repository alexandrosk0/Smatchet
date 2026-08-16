#!/usr/bin/env bash
# unwatched-pr-nudge.sh — SessionStart nudge for an OPEN agent-branch PR that
# nothing appears to be driving any more.
#
# Why this exists (infra 2026-08-16, observed on PR #2028): the autonomous
# ship-loop drives each PR to merge via self-scheduled `send_later` check-ins.
# A check-in that fires while the remote container is SUSPENDED is silently
# lost — the scheduler records `last_fired_at` + `ended_reason:
# run_once_fired`, so from its side the message was delivered, but no work ran.
# #2028's 04:23Z check-in was supposed to post the review trigger once the
# CodeRabbit quota reopened; it never did, and the PR then sat 11.5 hours with
# all CI green and no review requested.
#
# Nothing surfaced it, and that is structural rather than an oversight: the loop
# deliberately re-arms SILENTLY when a tick finds nothing actionable (correct —
# it keeps quiet holds out of the transcript), so "ran and found nothing" and
# "never ran at all" look identical from outside. Detection therefore cannot key
# on the loop reporting in; it has to key on the PR going quiet.
#
# Hence ABSENCE-AWARE, and stateless by design. An earlier shape considered for
# this entry had the check-in stamp a heartbeat file — but the heartbeat would be
# written by the very process that dies, so a suspended container writes nothing
# and a fresh checkout has no file at all; both are indistinguishable from
# "never armed". Asking GitHub what is open needs no cooperation from the thing
# that failed. Sibling of merge-watcher-stuck-nudge.sh, which applies the same
# absence-aware reasoning to the merge-watcher daemon.
#
# Modes:
#   --nudge (default)  SILENT unless an open agent-branch PR has been quiet
#                      longer than the staleness threshold; then print a short
#                      block naming each one. Never blocks (exit 0 always).
#   --list             plain `stale: PR #N — <branch> quiet <H>h` lines.
#   --selftest         fixture-driven test of the pure detector; exit 0/1.
#
# Knobs:
#   SMATCHET_UNWATCHED_PR_STALE_SECONDS  quiet-time before a PR is reported
#                                        (default 7200 = 2h; 0 disables).
#   SMATCHET_UNWATCHED_PR_BRANCH_RE      egrep-style branch filter
#                                        (default '^(claude|agent)/').
#   SMATCHET_UNWATCHED_PR_FIXTURE        file of `<num>\t<branch>\t<updated_at>\t<isDraft>`
#                                        rows — the injectable data layer, used
#                                        by --selftest and the bats suite.
#   SMATCHET_UNWATCHED_PR_NOW            RFC3339 override for "now".
#
# gh / network / auth absent → SILENT exit 0. This is advisory; it must never
# wedge SessionStart on a machine that cannot reach GitHub.
#
# selftest: asserts-failure
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$SCRIPT_DIR/lib/resolve-py.sh"

MODE="nudge"
case "${1:-}" in
    --nudge|"") MODE="nudge" ;;
    --list)     MODE="list" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: unwatched-pr-nudge.sh [--nudge|--list|--selftest]" >&2; exit 2 ;;
esac

PY="$(resolve_py 2>/dev/null || true)"
[ -n "$PY" ] || exit 0

# Fetch `<num>\t<branch>\t<updated_at>\t<isDraft>` rows for OPEN PRs. The
# fixture short-circuits the network entirely (selftest + bats). A gh failure
# yields NO rows, which the detector reads as "nothing to say" — never as
# "everything is stale".
fetch_rows() {
    if [ -n "${SMATCHET_UNWATCHED_PR_FIXTURE:-}" ]; then
        cat "$SMATCHET_UNWATCHED_PR_FIXTURE" 2>/dev/null || true
        return 0
    fi
    command -v gh >/dev/null 2>&1 || return 0
    # Bounded: SessionStart is bounded by the harness (Claude 10s, Codex 60s),
    # but a direct `--list` / CI / cron caller has no such ceiling, and a stalled
    # network makes `gh` hang rather than fail. `timeout` is not universal
    # (absent on some macOS setups), so it is used only when present.
    local tmo="${SMATCHET_UNWATCHED_PR_GH_TIMEOUT:-8}"
    local runner=()
    command -v timeout >/dev/null 2>&1 && runner=(timeout "$tmo")
    "${runner[@]}" gh pr list --state open --limit 100 \
        --json number,headRefName,updatedAt,isDraft \
        --jq '.[] | [(.number|tostring), .headRefName, .updatedAt, (.isDraft|tostring)] | @tsv' \
        2>/dev/null || true
}

# Pure detector. $1: a FILE of TSV rows. stdout: `num<TAB>branch<TAB>quiet_seconds`
# for each OPEN, NON-DRAFT PR on an agent branch quiet longer than the threshold.
# Draft PRs are excluded: a draft is parked on purpose, so reporting it would be
# the false positive that teaches people to skip this nudge.
#
# Rows arrive as a FILE, not on stdin: the program itself is fed to python on
# stdin via the heredoc, so a `sys.stdin` read here would consume the source
# text instead of the data (the --selftest caught exactly that).
detect() {
    "$PY" - "$1" <<'PY'
import os, re, sys, time
from datetime import datetime, timezone

thr = os.environ.get("SMATCHET_UNWATCHED_PR_STALE_SECONDS", "7200").strip()
try: thr = int(thr)
except ValueError: thr = 7200
if thr <= 0:
    sys.exit(0)  # explicitly disabled

pat = os.environ.get("SMATCHET_UNWATCHED_PR_BRANCH_RE", "").strip() or r"^(claude|agent)/"
try: rx = re.compile(pat)
except re.error: sys.exit(0)

now_s = os.environ.get("SMATCHET_UNWATCHED_PR_NOW", "").strip()
def parse(ts):
    # An offset-less value parses to a NAIVE datetime, and .timestamp() then
    # reads it as LOCAL time — skewing quiet-time by the machine's UTC offset.
    # gh always emits `Z`, so the production path never hits this; a
    # hand-set SMATCHET_UNWATCHED_PR_NOW like `2026-08-16T12:00:00` does.
    ts = (ts or "").strip().replace("Z", "+00:00")
    try: dt = datetime.fromisoformat(ts)
    except ValueError: return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()
now = parse(now_s) if now_s else time.time()
if now is None: sys.exit(0)

try:
    rows = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
except (OSError, IndexError):
    sys.exit(0)

for line in rows:
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 4: continue
    num, branch, updated, is_draft = parts[0], parts[1], parts[2], parts[3]
    if not num.strip().isdigit(): continue
    if is_draft.strip().lower() == "true": continue
    if not rx.search(branch): continue
    u = parse(updated)
    if u is None: continue          # unparseable timestamp: cannot judge -> silent
    quiet = int(now - u)
    if quiet > thr:
        print("%s\t%s\t%d" % (num.strip(), branch, quiet))
PY
}

run_selftest() {
    local tmp rc=0 out
    tmp="$(mktemp 2>/dev/null)" || return 0
    [ -n "$tmp" ] || return 0
    trap 'rm -f "$tmp"' RETURN
    export SMATCHET_UNWATCHED_PR_NOW="2026-08-16T12:00:00Z"
    export SMATCHET_UNWATCHED_PR_STALE_SECONDS=7200
    unset SMATCHET_UNWATCHED_PR_BRANCH_RE

    # selftest: asserts-failure — a quiet agent PR is the whole point.
    printf '2028\tclaude/backlog-review-w9h2n7\t2026-08-16T00:30:00Z\tfalse\n' > "$tmp"
    out="$(detect "$tmp")"
    if ! printf '%s' "$out" | grep -q '^2028\b'; then
        echo "unwatched-pr-nudge --selftest: FAIL — an 11.5h-quiet agent PR was not reported" >&2; rc=1
    fi

    # Fresh activity must stay silent, or every active PR nags.
    printf '2100\tclaude/x\t2026-08-16T11:59:00Z\tfalse\n' > "$tmp"
    if [ -n "$(detect "$tmp")" ]; then
        echo "unwatched-pr-nudge --selftest: FAIL — a PR touched 1 min ago was reported stale" >&2; rc=1
    fi

    # A DRAFT is parked deliberately — reporting it is the false positive that
    # trains people to ignore the nudge.
    printf '2101\tclaude/x\t2026-08-15T00:00:00Z\ttrue\n' > "$tmp"
    if [ -n "$(detect "$tmp")" ]; then
        echo "unwatched-pr-nudge --selftest: FAIL — a draft PR was reported" >&2; rc=1
    fi

    # A human branch is not this loop's business.
    printf '2102\tfeature/hand-written\t2026-08-15T00:00:00Z\tfalse\n' > "$tmp"
    if [ -n "$(detect "$tmp")" ]; then
        echo "unwatched-pr-nudge --selftest: FAIL — a non-agent branch was reported" >&2; rc=1
    fi

    # 0 disables outright.
    printf '2103\tclaude/x\t2026-08-01T00:00:00Z\tfalse\n' > "$tmp"
    if [ -n "$(SMATCHET_UNWATCHED_PR_STALE_SECONDS=0 detect "$tmp")" ]; then
        echo "unwatched-pr-nudge --selftest: FAIL — threshold 0 did not disable the nudge" >&2; rc=1
    fi

    # No rows (gh missing / unauthenticated / no open PRs) must be SILENT, never
    # "everything is stale" — this runs on every SessionStart.
    : > "$tmp"
    if [ -n "$(detect "$tmp")" ]; then
        echo "unwatched-pr-nudge --selftest: FAIL — empty input produced output" >&2; rc=1
    fi

    [ "$rc" -eq 0 ] && echo "unwatched-pr-nudge --selftest: PASS"
    return "$rc"
}

if [ "$MODE" = "selftest" ]; then
    run_selftest; exit $?
fi

# An unchecked mktemp leaves rows_file empty, and the redirection below then
# fails LOUDLY to stderr — straight into SessionStart output, which is exactly
# the silence this script promises. Exit quietly instead.
rows_file="$(mktemp 2>/dev/null)" || exit 0
[ -n "$rows_file" ] || exit 0
trap 'rm -f "$rows_file"' EXIT
fetch_rows > "$rows_file"
[ -s "$rows_file" ] || exit 0
stale="$(detect "$rows_file")" || exit 0
[ -n "$stale" ] || exit 0

# The threshold is operator-configurable, so integer hours would render every
# sub-hour report as `quiet 0h` — losing the one number the line exists to carry.
fmt_quiet() {
    if [ "$1" -ge 3600 ]; then printf '%dh' "$(( $1 / 3600 ))"; else printf '%dm' "$(( $1 / 60 ))"; fi
}

if [ "$MODE" = "list" ]; then
    while IFS=$'\t' read -r num branch quiet; do
        [ -n "$num" ] || continue
        printf 'stale: PR #%s — %s quiet %s\n' "$num" "$branch" "$(fmt_quiet "$quiet")"
    done <<< "$stale"
    exit 0
fi

echo "🔔 === open agent PR with no recent activity ==="
while IFS=$'\t' read -r num branch quiet; do
    [ -n "$num" ] || continue
    printf '  PR #%s (%s) has been quiet for %s.\n' "$num" "$branch" "$(fmt_quiet "$quiet")"
done <<< "$stale"
cat <<'EOS'
  A scheduled check-in that fires while the container is suspended is recorded
  as delivered but does no work, and the loop re-arms silently — so a lost tick
  looks exactly like a quiet one. Re-poll the PR's gates and re-arm before
  assuming anything is still driving it; do NOT read a green checks page as
  "ready" without confirming the review gate has real evidence on the head.
EOS
exit 0
