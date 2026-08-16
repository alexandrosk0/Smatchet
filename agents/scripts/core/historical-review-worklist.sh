#!/usr/bin/env bash
# historical-review-worklist.sh — build a historical-review sweep work-list and
# FAIL LOUDLY when the two PR enumerators disagree.
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (tooling 2026-08-16-historical-review-worklist-misses-merge-commit-prs)
#   A sweep's work-list used to be built by scraping `(#N)` off develop squash
#   subjects. That is only correct under the repo's squash-merge invariant, and
#   the invariant does not always hold: a PR merged with a MERGE COMMIT carries
#   the subject `Merge pull request #N from <branch>` (no trailing `(#N)`), and
#   its constituent commits carry no PR reference at all. Such a PR does not get
#   "skipped with a warning" — it never enters the work-list, so it cannot appear
#   in the batch's reviewed/clean/superseded counts and the batch reports a
#   complete frontier it never covered.
#
#   Measured: for #1878-#1940 GitHub reports 60 merged PRs, the scrape 53. The 7
#   invisible ones (#1883/#1919/#1920/#1921/#1923/#1927/#1932) were all merge
#   commits, and all release-publishing code that had never been survivor-
#   reviewed. Batch 20 nonetheless claimed "#1-#1940 contiguous". The same class
#   hit Batch 13 (#1439/#1577/#1593/#1597) and was caught then only because that
#   run happened to hand-cross-validate. Nothing enforced it, so honesty about
#   coverage depended on whether a given run remembered.
#
#   Second-order defect this also fixes: `git blame` attributes lines to a merge
#   commit's CONSTITUENTS, never to the merge commit itself. Feeding a merge sha
#   to historical-review-survivors.sh yields a falsely-clean `FULLY SUPERSEDED`
#   (an empty review surface that looks like "nothing to review"). Merge shas are
#   therefore expanded to one unit per constituent — promoting the Batch 16 #1593
#   "per-constituent special" from prose into the default path.
#
# THE CORE PROPERTY
#   The develop-log scrape is a CANDIDATE set, never an authority. This script
#   refuses to emit a work-list from the scrape alone: an authoritative merged-PR
#   set must be supplied (gh, or --merged-list for gh-less environments such as
#   every remote session). No authority -> exit 2, never a quiet partial list.
#
# USAGE
#   historical-review-worklist.sh --range <lo> <hi> [--merged-list <file>]
#   historical-review-worklist.sh --range=<lo>-<hi> [--merged-list=<file>]
#   historical-review-worklist.sh --selftest
#
#   --range <lo> <hi>     inclusive PR-number range to build units for.
#   --range=<lo>-<hi>     same, as one token (`,` also accepted as the separator).
#   --merged-list <file>  authoritative merged-PR numbers, whitespace/newline
#                         separated (a JSON array of numbers also parses). Use in
#                         gh-less environments; produce it from the GitHub API /
#                         MCP `list_pull_requests(base=develop, state=closed)`
#                         filtered on a non-null merged_at.
#   --against <ref>       tree to enumerate (default origin/develop).
#
# OUTPUT (stdout) — a JSON array of units, ready for the sweep workflow's args:
#   [{"pr":1883,"sha":"e5aa8d11","note":"merge-PR constituent 1/2"}, ...]
# A coverage triple + any disagreement report goes to STDERR, so a caller can
# quote a COMPUTED coverage number instead of asserting one.
#
# EXIT: 0 = work-list emitted and enumerators agree. 2 = usage / no authority /
# unresolvable PR / enumerator disagreement that could not be repaired.
#
# Sibling of historical-review-survivors.sh (the per-unit extractor) and
# historical-review-ledger-reconcile.sh (the staleness probe).
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(dirname "$0")/lib/resolve-py.sh"

MODE="range"
LO=""; HI=""; MERGED_LIST=""; AGAINST="origin/develop"
while [ $# -gt 0 ]; do
    case "$1" in
        --range) LO="${2:-}"; HI="${3:-}"; shift 3 ;;
        # `--range=<lo>-<hi>` / `--range=<lo>,<hi>` — the single-token twin of the
        # two-arg form above (shell-lint FLAG_PARITY requires a `=` twin for any
        # value-taking flag; it is also the form that survives being pasted into a
        # CI `run:` line without arg-splitting surprises).
        --range=*) LO="${1#*=}"; HI="${LO#*[-,]}"; LO="${LO%%[-,]*}"; shift ;;
        --merged-list) MERGED_LIST="${2:-}"; shift 2 ;;
        --merged-list=*) MERGED_LIST="${1#*=}"; shift ;;
        --against) AGAINST="${2:-}"; shift 2 ;;
        --against=*) AGAINST="${1#*=}"; shift ;;
        --selftest) MODE="selftest"; shift ;;
        -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "historical-review-worklist: unknown arg $1" >&2; exit 2 ;;
    esac
done

# --- pure helpers (testable without gh) --------------------------------------

# scrape_candidates <ref> <lo> <hi> — "<pr> <sha>" per line from `(#N)` subjects.
scrape_candidates() {
    git log "$1" --format='%h|%s' 2>/dev/null | awk -F'|' -v lo="$2" -v hi="$3" '
        {
            s=$0; sub(/^[^|]*\|/,"",s)
            if (match(s, /\(#[0-9]+\)$/)) {
                n = substr(s, RSTART+2, RLENGTH-3) + 0
                if (n >= lo && n <= hi) print n, $1
            }
        }' | sort -n -u
}

# read_merged_list <file> — bare PR numbers from a plain or JSON-array file.
read_merged_list() {
    tr -c '0-9' ' ' < "$1" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n -u
}

# merge_commit_for <pr> <ref> — the `Merge pull request #<pr>` commit, if any.
merge_commit_for() {
    git log "$2" --format='%h|%s' 2>/dev/null \
        | awk -F'|' -v pr="$1" '{
              s=$0; sub(/^[^|]*\|/,"",s)
              if (s ~ ("^Merge pull request #" pr "( |$)")) { print $1; exit }
          }'
}

# constituents_of <sha> — non-merge commits unique to a merge commit's topic side.
constituents_of() {
    local first
    first="$(git rev-list --parents -n1 "$1" 2>/dev/null | awk '{print $2}')"
    [ -n "$first" ] || return 1
    git log --format='%h' --no-merges "${first}..$1" 2>/dev/null
}

# is_merge_commit <sha>
is_merge_commit() {
    [ "$(git rev-list --parents -n1 "$1" 2>/dev/null | wc -w)" -ge 3 ]
}

if [ "$MODE" = "range" ]; then
    case "$LO$HI" in ''|*[!0-9]*) echo "historical-review-worklist: --range <lo> <hi> required (integers)" >&2; exit 2 ;; esac
    if ! git rev-parse --verify -q "${AGAINST}^{commit}" >/dev/null 2>&1; then
        echo "historical-review-worklist: '$AGAINST' does not resolve — fetch first" >&2; exit 2
    fi

    # --- the authority. The scrape alone is NEVER acceptable. ----------------
    AUTH_SRC=""
    AUTH_FILE="$(mktemp)"; trap 'rm -f "$AUTH_FILE"' EXIT
    if [ -n "$MERGED_LIST" ]; then
        [ -r "$MERGED_LIST" ] || { echo "historical-review-worklist: --merged-list '$MERGED_LIST' unreadable" >&2; exit 2; }
        read_merged_list "$MERGED_LIST" | awk -v lo="$LO" -v hi="$HI" '$1>=lo && $1<=hi' > "$AUTH_FILE"
        AUTH_SRC="--merged-list $MERGED_LIST"
    elif command -v gh >/dev/null 2>&1; then
        gh pr list --state merged --base develop --limit 900 --json number \
           --jq '.[].number' 2>/dev/null \
          | awk -v lo="$LO" -v hi="$HI" '$1>=lo && $1<=hi' | sort -n -u > "$AUTH_FILE"
        AUTH_SRC="gh pr list"
        [ -s "$AUTH_FILE" ] || { echo "historical-review-worklist: gh returned no merged PRs in [$LO,$HI] — refusing to emit a scrape-only work-list" >&2; exit 2; }
    else
        cat >&2 <<'EOF'
historical-review-worklist: NO AUTHORITATIVE MERGED-PR SET AVAILABLE.
  `gh` is absent and --merged-list was not supplied. The develop-log `(#N)`
  scrape is a CANDIDATE set only: it cannot see a PR merged with a merge
  commit, so emitting from it alone silently understates coverage (the
  #1878-#1940 case: 60 merged, 53 scraped). Supply the authority:
    --merged-list <file>   # numbers from list_pull_requests(base=develop,
                           # state=closed) filtered on non-null merged_at
EOF
        exit 2
    fi

    CAND_FILE="$(mktemp)"; SCRAPE_NUMS="$(mktemp)"
    trap 'rm -f "$AUTH_FILE" "$CAND_FILE" "$SCRAPE_NUMS"' EXIT
    scrape_candidates "$AGAINST" "$LO" "$HI" > "$CAND_FILE"
    awk '{print $1}' "$CAND_FILE" | sort -n -u > "$SCRAPE_NUMS"

    MISSING="$(comm -23 "$AUTH_FILE" "$SCRAPE_NUMS" | tr '\n' ' ')"
    EXTRA="$(comm -13 "$AUTH_FILE" "$SCRAPE_NUMS" | tr '\n' ' ')"

    # EXTRA = in the log but not in the merged list. Never silently dropped:
    # it means the authority is stale or the subject was hand-edited.
    if [ -n "${EXTRA// /}" ]; then
        echo "historical-review-worklist: WARN — in the develop log but NOT in the authoritative merged set: $EXTRA" >&2
        echo "  (stale authority, or an edited squash subject — verify before trusting this batch's coverage)" >&2
    fi

    UNITS=""; RESOLVED=0; UNRESOLVED=""
    emit() { UNITS="${UNITS}${UNITS:+$'\n'}$1 $2 $3"; RESOLVED=$((RESOLVED+1)); }

    while read -r pr sha; do
        [ -n "$pr" ] || continue
        if is_merge_commit "$sha"; then
            # A squash-subject match that is nonetheless a merge commit.
            idx=0; total=$(constituents_of "$sha" | wc -l)
            while read -r c; do
                [ -n "$c" ] || continue
                idx=$((idx+1)); emit "$pr" "$c" "merge-PR constituent $idx/$total"
            done < <(constituents_of "$sha")
        else
            emit "$pr" "$sha" ""
        fi
    done < "$CAND_FILE"

    # Repair the misses the scrape structurally cannot see.
    for pr in $MISSING; do
        mc="$(merge_commit_for "$pr" "$AGAINST")"
        if [ -z "$mc" ]; then
            UNRESOLVED="${UNRESOLVED}${UNRESOLVED:+ }#$pr"
            continue
        fi
        total=$(constituents_of "$mc" | wc -l)
        if [ "$total" -eq 0 ]; then
            UNRESOLVED="${UNRESOLVED}${UNRESOLVED:+ }#$pr(no-constituents)"
            continue
        fi
        idx=0
        while read -r c; do
            [ -n "$c" ] || continue
            idx=$((idx+1)); emit "$pr" "$c" "merge-PR constituent $idx/$total"
        done < <(constituents_of "$mc")
    done

    AUTH_N=$(wc -l < "$AUTH_FILE" | tr -d ' ')
    SCRAPE_N=$(wc -l < "$SCRAPE_NUMS" | tr -d ' ')
    COVERED=$(printf '%s\n' "$UNITS" | awk 'NF{print $1}' | sort -u | wc -l | tr -d ' ')

    {
        echo "historical-review-worklist: range [$LO,$HI] against $AGAINST (authority: $AUTH_SRC)"
        echo "  authoritative merged PRs : $AUTH_N"
        echo "  develop-log scrape       : $SCRAPE_N"
        if [ -n "${MISSING// /}" ]; then
            echo "  scrape MISSED (repaired) : $MISSING"
        else
            echo "  scrape MISSED            : none"
        fi
        echo "  coverage                 : $COVERED/$AUTH_N PRs -> $RESOLVED review unit(s)"
    } >&2

    if [ -n "$UNRESOLVED" ]; then
        echo "historical-review-worklist: FAIL — merged PR(s) with no resolvable commit on $AGAINST: $UNRESOLVED" >&2
        echo "  Refusing to emit a work-list that silently omits them." >&2
        exit 2
    fi

    PY="$(resolve_py)" || { echo "historical-review-worklist: python required to emit JSON" >&2; exit 2; }
    printf '%s\n' "$UNITS" | "$PY" -c '
import json,sys
out=[]
for line in sys.stdin:
    line=line.rstrip("\n")
    if not line.strip(): continue
    parts=line.split(" ",2)
    u={"pr":int(parts[0]),"sha":parts[1]}
    if len(parts)>2 and parts[2].strip(): u["note"]=parts[2].strip()
    out.append(u)
json.dump(out,sys.stdout)
sys.stdout.write("\n")'
    exit 0
fi

# --- selftest ----------------------------------------------------------------
# Replays the MOTIVATING BUG against a real git fixture: a range holding one
# squash-merged PR and one TRUE MERGE-COMMIT PR. The scrape sees only the squash
# one; the gate must notice, repair it to constituents, and never emit a
# scrape-only list.
fail=0
self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

# Fixture A — the motivating bug. Merge-commit PR must be detected + expanded.
repoA="$(mktemp -d)"
(
    cd "$repoA" || exit 99
    git init -q -b develop && git config user.email t@t && git config user.name t
    mkdir -p agents/scripts/core/lib
    cp "$self" agents/scripts/core/historical-review-worklist.sh
    cp "$(dirname "$self")/lib/resolve-py.sh" agents/scripts/core/lib/resolve-py.sh
    echo a > a.txt && git add -A && git commit -qm "base"
    echo b > b.txt && git add -A && git commit -qm "feat: squashed thing (#100)"
    # a true merge-commit PR (#101) with two constituents
    git checkout -q -b topic
    echo c > c.txt && git add -A && git commit -qm "first half"
    echo d > d.txt && git add -A && git commit -qm "second half"
    git checkout -q develop
    git merge -q --no-ff topic -m "Merge pull request #101 from o/topic"
    printf '100\n101\n' > /tmp/auth.$$
    bash agents/scripts/core/historical-review-worklist.sh \
         --range 100 101 --merged-list /tmp/auth.$$ --against develop >/dev/null 2>/tmp/err.$$
    rc=$?
    out="$(bash agents/scripts/core/historical-review-worklist.sh \
             --range 100 101 --merged-list /tmp/auth.$$ --against develop 2>/dev/null)"
    err="$(cat /tmp/err.$$)"; rm -f /tmp/auth.$$ /tmp/err.$$
    [ "$rc" = "0" ] || { echo "FAIL(A): exit $rc, want 0"; exit 1; }
    case "$err" in *"scrape MISSED (repaired) : 101"*) ;; *) echo "FAIL(A): #101 not reported as missed; stderr: $err"; exit 1 ;; esac
    case "$out" in *'"pr": 101'*|*'"pr":101'*) ;; *) echo "FAIL(A): #101 absent from work-list: $out"; exit 1 ;; esac
    n=$(printf '%s' "$out" | grep -o '"pr"' | wc -l)
    [ "$n" = "3" ] || { echo "FAIL(A): want 3 units (1 squash + 2 constituents), got $n: $out"; exit 1; }
    case "$out" in *"constituent 1/2"*) ;; *) echo "FAIL(A): constituents not expanded: $out"; exit 1 ;; esac
    # The `--range=<lo>-<hi>` twin must produce a byte-identical work-list to the
    # two-arg form (else the FLAG_PARITY twin is decorative).
    printf '100\n101\n' > /tmp/auth2.$$
    out2="$(bash agents/scripts/core/historical-review-worklist.sh \
              --range=100-101 --merged-list=/tmp/auth2.$$ --against develop 2>/dev/null)"
    rm -f /tmp/auth2.$$
    [ "$out2" = "$out" ] || { echo "FAIL(A): --range=lo-hi differs from --range lo hi"; echo "  two-arg: $out"; echo "  =form  : $out2"; exit 1; }
) || fail=1
rm -rf "$repoA"

# Fixture B — NO authority (no gh, no --merged-list) must exit 2 rather than emit
# a scrape-only list. This is the property the whole gate exists for.
# selftest: asserts-failure — a work-list with no authoritative merged set must be refused (exit 2).
repoB="$(mktemp -d)"
(
    cd "$repoB" || exit 99
    git init -q -b develop && git config user.email t@t && git config user.name t
    mkdir -p agents/scripts/core/lib
    cp "$self" agents/scripts/core/historical-review-worklist.sh
    cp "$(dirname "$self")/lib/resolve-py.sh" agents/scripts/core/lib/resolve-py.sh
    echo a > a.txt && git add -A && git commit -qm "feat: thing (#100)"
    PATH="/usr/bin:/bin" bash agents/scripts/core/historical-review-worklist.sh \
        --range 100 100 --against develop >/dev/null 2>&1
    rc=$?
    # gh may genuinely exist on a dev box; only assert the no-gh contract when absent.
    if command -v gh >/dev/null 2>&1; then exit 0; fi
    [ "$rc" = "2" ] || { echo "FAIL(B): no-authority exit $rc, want 2 (scrape-only list must be refused)"; exit 1; }
) || fail=1
rm -rf "$repoB"

# Fixture C — an authoritative PR with no resolvable commit must fail loudly, not
# vanish silently from the work-list.
# selftest: asserts-failure — a merged PR with no resolvable commit must exit 2.
repoC="$(mktemp -d)"
(
    cd "$repoC" || exit 99
    git init -q -b develop && git config user.email t@t && git config user.name t
    mkdir -p agents/scripts/core/lib
    cp "$self" agents/scripts/core/historical-review-worklist.sh
    cp "$(dirname "$self")/lib/resolve-py.sh" agents/scripts/core/lib/resolve-py.sh
    echo a > a.txt && git add -A && git commit -qm "feat: thing (#100)"
    printf '100\n102\n' > /tmp/authC.$$
    bash agents/scripts/core/historical-review-worklist.sh \
        --range 100 102 --merged-list /tmp/authC.$$ --against develop >/dev/null 2>/tmp/errC.$$
    rc=$?; err="$(cat /tmp/errC.$$)"; rm -f /tmp/authC.$$ /tmp/errC.$$
    [ "$rc" = "2" ] || { echo "FAIL(C): unresolvable PR exit $rc, want 2"; exit 1; }
    case "$err" in *"no resolvable commit"*) ;; *) echo "FAIL(C): missing loud message; stderr: $err"; exit 1 ;; esac
) || fail=1
rm -rf "$repoC"

if [ "$fail" = "0" ]; then echo "historical-review-worklist --selftest: PASS (3 e2e fixtures)"; exit 0; fi
echo "historical-review-worklist --selftest: FAIL"; exit 1
