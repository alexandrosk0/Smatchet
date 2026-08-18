#!/usr/bin/env bash
# cr-oob-review-backfill.sh — drip a retroactive CodeRabbit review onto every PR
# that merged carrying `cr-out-of-band`.
# ----------------------------------------------------------------------------
# WHY THIS EXISTS
#   `cr-out-of-band` downgrades the CodeRabbit merge-gate block to WARN
#   (docs/agent-rules/merge-gates.md § Per-PR overrides). It waives the GATE,
#   never the REVIEW: 90 PRs merged under it, each leaving a diff on develop
#   that CR either never looked at or looked at and had findings on. This drains
#   that debt.
#
#   Two measurements set the shape (both on #1977, 2026-08-16):
#     1. CR accepts a review request on a MERGED PR — it answers `Full
#        retroactive review requested for #N` and reviews the merged head. The
#        debt is collectable in place; no reconstruction onto a fresh PR.
#     2. The plan carries ONE included review at a time on a rolling window. A
#        second request inside the window answers `Review rate limited … next
#        included review will be available in N minutes` and is DROPPED, not
#        queued — #1977 still had zero reviews 39h after its rate-limited
#        request. A bounced request must be retried, never assumed pending.
#
#   So a bulk sweep buys one review and 89 rate-limit replies. This fires at
#   most ONE request per invocation and holds when the window is closed.
#
# STATELESS BY DESIGN
#   The first cut of this backfill ran as a session-scoped 15-minute cron that
#   updated a status column in a committed ledger. The session was reclaimed and
#   the drip made ZERO progress in 39 hours — the same class
#   unwatched-pr-nudge.sh was written for (a check-in that was supposed to post
#   a review trigger once CR's quota reopened, and never did). A ledger written
#   by the process that dies cannot report that the process died.
#
#   So no mutable state is kept here. Every question is asked of GitHub:
#     - "has PR N been requested?"  -> N carries a `cr-oob-backfill:` marker
#     - "did the request land?"     -> CR's reply under that marker
#     - "does N still owe a review?"-> N's merged head carries no CR review
#   A run therefore recomputes from scratch and is safe to interrupt, re-run,
#   or run concurrently with itself. It also needs no repo write access —
#   `pull-requests: write` to comment is the whole permission surface.
#
# MODES
#   --poll (default)  One tick: fire a single review request iff the quota
#                     window is open and something is owed. Silent on a hold.
#   --list            Queue status: requested / owed / skipped counts.
#   --selftest        Fixture-driven test of the pure decision core; exit 0/1.
#
# KNOBS
#   SMATCHET_CR_BACKFILL_WINDOW_SECONDS  quota window (default 2700 = 45min).
#   SMATCHET_CR_BACKFILL_QUEUE           queue file (default the repo ledger).
#   SMATCHET_CR_BACKFILL_FIXTURE         decision-input file — the injectable
#                                        data layer used by --selftest + bats.
#   SMATCHET_CR_BACKFILL_NOW             epoch-seconds override for "now".
#   SMATCHET_CR_BACKFILL_CONFIRM_GRACE_SECONDS
#                                        how long an unanswered request may sit
#                                        before the drip halts (default 1800).
#   SMATCHET_CR_BACKFILL_DRY_RUN         1 = print the request, never post it.
#
# gh / network / auth absent -> SILENT exit 0. This is a drip, not a gate; it
# must never wedge a caller that cannot reach GitHub.
#
# selftest: asserts-failure
set -uo pipefail

REPO_SLUG="alexandrosk0/Smatchet"
MARKER_PREFIX="cr-oob-backfill"
WINDOW_SECONDS="${SMATCHET_CR_BACKFILL_WINDOW_SECONDS:-2700}"
# How long a request may go unanswered before the drip stops rather than
# marching on. Shorter than the window so the stall surfaces on the same tick
# the window would otherwise reopen.
CONFIRM_GRACE_SECONDS="${SMATCHET_CR_BACKFILL_CONFIRM_GRACE_SECONDS:-1800}"
DRY_RUN="${SMATCHET_CR_BACKFILL_DRY_RUN:-0}"

# ── pure decision core ──────────────────────────────────────────────────────
# Reads the decision input on stdin and prints exactly one verdict line:
#   FIRE <pr>        — the window is open and <pr> is the next PR owed a review
#   HOLD <reason>    — nothing to do this tick
#
# Input lines (tab-separated), order-independent:
#   now       <epoch>              "now", in epoch seconds
#   queue     <pr>                 a PR owed a review, newest merge first
#   requested <pr>  <epoch>        a request already posted, and when
#   cooldown  <epoch>              CR named a rate-limit cooldown ending here
#   confirmed <pr>                 a request CR visibly answered (reply or review)
#
# The verdict is a pure function of that input — no clock, no network. Keeping
# it pure is what makes the rate-limit arithmetic testable, which matters
# because getting it wrong is silent: firing too early burns the request
# (dropped, not queued) and firing too late just stalls the drain.
cr_backfill_decide() {
    awk -F'\t' -v window="$WINDOW_SECONDS" -v grace="$CONFIRM_GRACE_SECONDS" '
        $1 == "now"       { now = $2 + 0; next }
        $1 == "queue"     { queue[++qn] = $2 + 0; next }
        $1 == "requested" {
            req[$2 + 0] = 1
            if (($3 + 0) > last_req) { last_req = $3 + 0; last_pr = $2 + 0; last_pr_at = $3 + 0 }
            next
        }
        $1 == "cooldown"  { if (($2 + 0) > cooldown) cooldown = $2 + 0; next }
        $1 == "confirmed" { conf[$2 + 0] = 1; next }
        END {
            if (cooldown > 0 && now < cooldown) {
                printf "HOLD rate-limited for %d more second(s)\n", cooldown - now
                exit 0
            }
            if (last_req > 0 && (now - last_req) < window) {
                printf "HOLD window closed for %d more second(s)\n", window - (now - last_req)
                exit 0
            }
            # FAIL-SAFE: never march past a request CR never answered. On a
            # merged PR a request can silently no-op, and "requested" is not
            # "reviewed" — without this the drip would post 90 comments that
            # collect nothing and call the debt paid. Hold instead, loudly.
            if (last_pr > 0 && !(last_pr in conf) && (now - last_pr_at) > grace) {
                printf "HOLD unconfirmed — #%d requested %ds ago, no CR response\n", last_pr, now - last_pr_at
                exit 0
            }
            for (i = 1; i <= qn; i++) {
                if (!(queue[i] in req)) { printf "FIRE %d\n", queue[i]; exit 0 }
            }
            print "HOLD queue drained"
        }
    '
}

# ── selftest ────────────────────────────────────────────────────────────────
run_selftest() {
    local fails=0 got

    _case() { # <name> <expected> <input>
        got="$(printf '%b' "$3" | cr_backfill_decide)"
        if [ "$got" = "$2" ]; then
            echo "ok   — $1"
        else
            echo "FAIL — $1: expected '$2', got '$got'" >&2
            fails=$((fails + 1))
        fi
    }

    # An open window with an untouched queue fires the newest PR.
    _case "fires the newest owed PR" "FIRE 1995" \
        "now\t1000000\nqueue\t1995\nqueue\t1977\n"

    # Already-requested PRs are skipped in queue order.
    _case "skips an already-requested PR" "FIRE 1977" \
        "now\t1000000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t900000\nconfirmed\t1995\n"

    # ASSERTS-FAILURE: a request inside the window must HOLD. Firing here would
    # spend a request CR drops on the floor — the #1977 bounce, which is the
    # whole reason this core exists.
    _case "holds inside the quota window" "HOLD window closed for 1700 more second(s)" \
        "now\t1001000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t1000000\n"

    # A cooldown CR named itself outranks the local window arithmetic.
    _case "honours a CR-named cooldown" "HOLD rate-limited for 600 more second(s)" \
        "now\t1000000\nqueue\t1995\ncooldown\t1000600\n"

    # An elapsed cooldown does not hold.
    _case "releases an elapsed cooldown" "FIRE 1995" \
        "now\t1000000\nqueue\t1995\ncooldown\t999000\n"

    # ASSERTS-FAILURE: an unanswered request halts the drip. Marching on would
    # post a comment per queued PR and collect nothing — the failure mode that
    # turns a backfill into a spam machine.
    _case "halts on an unanswered request" "HOLD unconfirmed — #1995 requested 100000s ago, no CR response" \
        "now\t1000000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t900000\n"

    # Inside the grace period an unanswered request is simply too early to judge.
    _case "gives an unanswered request its grace period" "HOLD window closed for 1700 more second(s)" \
        "now\t1001000\nqueue\t1995\nqueue\t1977\nrequested\t1995\t1000000\n"

    # Nothing left to do.
    _case "reports a drained queue" "HOLD queue drained" \
        "now\t1000000\nqueue\t1995\nrequested\t1995\t100\nconfirmed\t1995\n"

    if [ "$fails" -eq 0 ]; then
        echo "cr-oob-review-backfill --selftest: all cases passed"
        return 0
    fi
    echo "cr-oob-review-backfill --selftest: $fails case(s) failed" >&2
    return 1
}

# ── data layer ──────────────────────────────────────────────────────────────
have_gh() { command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; }

# Resolve a WORKING interpreter, not merely a named one: on Windows `python3` is
# the Store App Execution Alias stub, which satisfies `command -v` and then exits
# non-zero. Empty when none works — every call site degrades to a silent no-op.
PY_BIN=""
for _c in python3 python py; do
    if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then PY_BIN="$_c"; break; fi
done

queue_file() {
    local root
    root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
    echo "${SMATCHET_CR_BACKFILL_QUEUE:-$root/docs/self-improvement/cr-oob-review-backfill.jsonl}"
}

now_epoch() { echo "${SMATCHET_CR_BACKFILL_NOW:-$(date -u +%s)}"; }

# Build the decision input from GitHub. One search call resolves the whole
# "already requested" set — asking per-PR would be 90 calls a tick.
build_decision_input() {
    local qf; qf="$(queue_file)"
    printf 'now\t%s\n' "$(now_epoch)"
    [ -f "$qf" ] && grep -oE '"pr":[0-9]+' "$qf" | cut -d: -f2 | while read -r pr; do
        printf 'queue\t%s\n' "$pr"
    done

    # Marker comments are the request record. `in:comments` indexes comment
    # bodies, so one query returns every PR this backfill has already touched.
    gh search issues --repo "$REPO_SLUG" "$MARKER_PREFIX in:comments" \
        --json number,updatedAt --limit 100 2>/dev/null \
    | ${PY_BIN:-:} -c '
import json,sys,calendar,time
try: rows=json.load(sys.stdin)
except Exception: rows=[]
for r in rows:
    t=r.get("updatedAt") or ""
    try: e=calendar.timegm(time.strptime(t,"%Y-%m-%dT%H:%M:%SZ"))
    except Exception: e=0
    print("requested\t%d\t%d" % (r.get("number",0), e))
' 2>/dev/null

    # Confirm the newest request (see emit_confirmation).
    local newest newest_at
    newest="$(gh search issues --repo "$REPO_SLUG" "$MARKER_PREFIX in:comments" \
        --json number,updatedAt --limit 100 2>/dev/null \
        | ${PY_BIN:-:} -c '
import json,sys
try: rows=json.load(sys.stdin)
except Exception: rows=[]
rows=[r for r in rows if r.get("updatedAt")]
if rows:
    r=max(rows,key=lambda r:r["updatedAt"]); print("%s %s" % (r["number"], r["updatedAt"]))
' 2>/dev/null)"
    if [ -n "$newest" ]; then
        newest_at="${newest#* }"; newest="${newest%% *}"
        emit_confirmation "$newest" "$newest_at"
    fi

    # CR states its own cooldown in the reply to our marker. Honour it verbatim
    # rather than re-deriving it — the server knows the rolling window, we do not.
    gh api "repos/$REPO_SLUG/issues/comments?per_page=100&sort=created&direction=desc" \
        --jq '.[] | select(.user.login=="coderabbitai[bot]") | select(.body|test("available in [0-9]+ minutes")) | "\(.created_at)\t\(.body)"' \
        2>/dev/null | head -1 \
    | ${PY_BIN:-:} -c '
import sys,re,calendar,time
line=sys.stdin.read().strip()
if line:
    ts,_,body=line.partition("\t")
    m=re.search(r"available in (\d+) minutes", body)
    try: e=calendar.timegm(time.strptime(ts,"%Y-%m-%dT%H:%M:%SZ"))
    except Exception: e=0
    if m and e: print("cooldown\t%d" % (e + int(m.group(1))*60))
' 2>/dev/null
}

# Confirm the NEWEST request only — it is the sole input the fail-safe reads, and
# confirming all 90 would cost a call per PR per tick. "Confirmed" is deliberately
# generous: any CR comment after our marker counts, including a rate-limit reply.
# The question it answers is "is CR responding to us at all", not "did the review
# pass" — a silent CR is the failure this guards, and a talking CR is not stuck.
emit_confirmation() {
    local pr="$1" marker_at="$2"
    local answered
    answered="$(gh api "repos/$REPO_SLUG/issues/$pr/comments?per_page=100" \
        --jq "[.[]|select(.user.login==\"coderabbitai[bot]\")|select(.created_at > \"$marker_at\")]|length" 2>/dev/null)"
    if [ "${answered:-0}" -gt 0 ]; then printf 'confirmed\t%s\n' "$pr"; return; fi
    answered="$(gh api "repos/$REPO_SLUG/pulls/$pr/reviews" \
        --jq '[.[]|select(.user.login=="coderabbitai[bot]")]|length' 2>/dev/null)"
    [ "${answered:-0}" -gt 0 ] && printf 'confirmed\t%s\n' "$pr"
}

# A merged head that already carries a completed CR review owes nothing — the
# label waived the FINDINGS, not the review, and re-requesting spends a slot the
# queue needs.
owes_review() {
    local pr="$1" n
    n="$(gh api "repos/$REPO_SLUG/pulls/$pr/reviews" --jq '[.[]|select(.user.login=="coderabbitai[bot]")]|length' 2>/dev/null)"
    [ "${n:-0}" -eq 0 ]
}

post_request() {
    local pr="$1"
    local body
    body="$(cat <<EOF
@coderabbitai full review

<!-- ${MARKER_PREFIX}:${pr} -->
_Retroactive review backfill: this PR merged carrying \`cr-out-of-band\`, so the CodeRabbit merge gate was waived and CR never completed a review of its merged head. Requesting that review now._

---
_Generated by [Claude Code](https://claude.ai/code)_
EOF
)"
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY-RUN would request review on #$pr"
        return 0
    fi
    gh pr comment "$pr" --repo "$REPO_SLUG" --body "$body" >/dev/null 2>&1 \
        && echo "requested review on #$pr" \
        || { echo "cr-oob-review-backfill: failed to comment on #$pr" >&2; return 1; }
}

decision_input() {
    if [ -n "${SMATCHET_CR_BACKFILL_FIXTURE:-}" ]; then
        cat "$SMATCHET_CR_BACKFILL_FIXTURE"
    else
        build_decision_input
    fi
}

run_poll() {
    if [ -z "${SMATCHET_CR_BACKFILL_FIXTURE:-}" ] && ! have_gh; then
        return 0   # advisory: no gh/auth is a silent no-op, never a failure
    fi
    local verdict pr
    verdict="$(decision_input | cr_backfill_decide)"
    case "$verdict" in
        FIRE\ *)
            pr="${verdict#FIRE }"
            # Recheck at fire time: the queue is static, the review state is not.
            if [ -z "${SMATCHET_CR_BACKFILL_FIXTURE:-}" ] && ! owes_review "$pr"; then
                echo "skipped #$pr — merged head already carries a CR review"
                return 0
            fi
            post_request "$pr"
            ;;
        *)  [ -n "${VERBOSE:-}" ] && echo "$verdict"
            return 0
            ;;
    esac
}

run_list() {
    decision_input | awk -F'\t' '
        $1 == "queue"     { q[++n] = $2; next }
        $1 == "requested" { req[$2] = 1; next }
        END {
            for (i = 1; i <= n; i++) if (q[i] in req) r++;
            printf "queue %d — requested %d, owed %d\n", n, r, n - r
        }'
}

case "${1:---poll}" in
    --selftest) run_selftest ;;
    --list)     run_list ;;
    --poll)     run_poll ;;
    *) echo "usage: cr-oob-review-backfill.sh [--poll|--list|--selftest]" >&2; exit 2 ;;
esac
