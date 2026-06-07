#!/usr/bin/env bash
# historical-review-survivors.sh — emit ONLY the lines a past PR/commit
# introduced that are STILL ALIVE and UNTOUCHED at HEAD.
#
# Why: re-reviewing an old PR should not re-flag code a NEWER PR already
# changed/fixed. develop squash-merges, so each PR is exactly ONE commit on
# develop; `git blame HEAD` attributes a line to the commit that LAST touched
# it. A line still blamed to the PR's squash commit therefore = "introduced by
# this PR and never touched since" — exactly the review surface we want. A line
# a later PR rewrote is re-attributed to that later commit and drops out for
# free. No diff math, no manual exclusion list.
#
# Usage:
#   historical-review-survivors.sh --pr <N>          # resolve PR's merge commit
#   historical-review-survivors.sh <commit-ish>      # any commit on HEAD's history
#   historical-review-survivors.sh --pr 948 --context 2   # N context lines (default 0)
#
# Output: a review DIGEST on stdout — per surviving file, the surviving line
# ranges with their current HEAD line numbers + code. Feeds the
# `historical-code-review` skill. Exit 0 even when nothing survives (prints a
# "fully superseded" banner). Exit 2 on bad input.
#
# Read-only. No git mutations.
set -euo pipefail

# Resolve a working Python (Windows ships `python`; bare `python3` may hit the
# Microsoft Store alias stub). Prefer an explicit $PYTHON, then python3/python/py.
PY=""
for cand in "${PYTHON:-}" python3 python "py -3"; do
    [ -z "$cand" ] && continue
    if $cand -c "import sys" >/dev/null 2>&1; then PY="$cand"; break; fi
done
if [ -z "$PY" ]; then
    echo "historical-review-survivors: no working Python found (set \$PYTHON)" >&2
    exit 2
fi

CONTEXT=0
PR=""
COMMITISH=""
while [ $# -gt 0 ]; do
    case "$1" in
        --pr) PR="${2:-}"; shift 2 ;;
        --context) CONTEXT="${2:-0}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        --*) echo "historical-review-survivors: unknown flag $1" >&2; exit 2 ;;
        *) COMMITISH="$1"; shift ;;
    esac
done

# --- Resolve the target commit SHA --------------------------------------------
if [ -n "$PR" ]; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "historical-review-survivors: --pr needs gh (unauthenticated/absent)" >&2
        exit 2
    fi
    SHA="$(gh pr view "$PR" --json mergeCommit --jq '.mergeCommit.oid' 2>/dev/null || true)"
    if [ -z "$SHA" ] || [ "$SHA" = "null" ]; then
        echo "historical-review-survivors: PR #$PR has no merge commit (open/closed-unmerged?)" >&2
        exit 2
    fi
    LABEL="PR #$PR"
elif [ -n "$COMMITISH" ]; then
    SHA="$(git rev-parse --verify "${COMMITISH}^{commit}" 2>/dev/null || true)"
    if [ -z "$SHA" ]; then
        echo "historical-review-survivors: '$COMMITISH' is not a commit" >&2
        exit 2
    fi
    LABEL="commit ${SHA:0:8}"
else
    echo "historical-review-survivors: need --pr <N> or a <commit-ish>" >&2
    exit 2
fi

# Must be an ancestor of HEAD (else "alive at HEAD" is meaningless).
if ! git merge-base --is-ancestor "$SHA" HEAD 2>/dev/null; then
    echo "historical-review-survivors: $LABEL ($SHA) is not an ancestor of HEAD — nothing to review against this tree" >&2
    exit 2
fi

SHORT="${SHA:0:8}"
SUBJECT="$(git log -1 --format='%s' "$SHA")"

# Files this commit changed (vs its parent = the PR's net diff under squash).
# --root so an initial commit lists its files instead of emitting nothing.
mapfile -t FILES < <(git diff-tree --no-commit-id --name-only -r --root "$SHA")

emit_header() {
    echo "# Historical-review survivors — $LABEL ($SHORT)"
    echo "# Subject: $SUBJECT"
    echo "# Lines this change introduced that are STILL ALIVE + UNTOUCHED at HEAD."
    echo "# Lines a later PR modified/fixed are EXCLUDED by construction (git blame"
    echo "# re-attributes them). Review ONLY the lines below; do not re-flag what is gone."
    echo "#"
}

TMP_BODY="$(mktemp)"
PARSER="$(mktemp --suffix=.py 2>/dev/null || mktemp)"
trap 'rm -f "$TMP_BODY" "$PARSER"' EXIT

# Porcelain parser lives in its own file so the blame stream (stdin via pipe) is
# NOT shadowed by a heredoc — `python - <<EOF` would make the heredoc, not the
# pipe, become stdin. argv: <target-sha> <context> <path>; reads blame on stdin.
cat > "$PARSER" <<'PY'
import sys, subprocess
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Windows cp1252 stdout safety
except Exception:
    pass
target, ctx, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
cur = None; final = None
rows = {}            # final_lineno -> code (lines still blamed to target)
for line in sys.stdin.buffer.read().split(b"\n"):
    if line[:1] == b"\t":
        if cur == target and final is not None:
            rows[final] = line[1:].decode("utf-8", "replace")
        continue
    parts = line.split(b" ")
    if len(parts) >= 3 and len(parts[0]) == 40:
        try:
            int(parts[0], 16)
            cur = parts[0].decode(); final = int(parts[2]); continue
        except ValueError:
            pass
nums = sorted(rows)
if not nums:
    sys.exit(0)
full = []
if ctx > 0:
    full = subprocess.run(["git", "show", f"HEAD:{path}"],
                          capture_output=True).stdout.decode("utf-8", "replace").split("\n")
ranges = []
start = prev = nums[0]
for n in nums[1:]:
    if n - prev <= max(1, ctx):
        prev = n
    else:
        ranges.append((start, prev)); start = prev = n
ranges.append((start, prev))
print(f"__ALIVE__ {len(nums)}")
for a, b in ranges:
    lo = max(1, a - ctx); hi = b + ctx
    print(f"@@ lines {lo}-{hi} @@")
    for ln in range(lo, hi + 1):
        code = full[ln - 1] if (ctx > 0 and 1 <= ln <= len(full)) else rows.get(ln, "")
        mark = " " if ln in rows else "~"   # "~" = context line (added/changed by another commit)
        print(f"{ln:>6}:{mark}{code}")
    print()
PY

total_introduced=0
total_alive=0
files_with_survivors=0

for f in "${FILES[@]}"; do
    # Skip files that no longer exist at HEAD (deleted/renamed = "touched").
    git cat-file -e "HEAD:$f" 2>/dev/null || continue

    # Net lines this commit added to this file (for the supersede ratio).
    added="$(git log -1 --format= --numstat "$SHA" -- "$f" | awk 'NF>=3 && $1!="-"{s+=$1} END{print s+0}')"
    total_introduced=$((total_introduced + added))

    # Surviving lines: blame HEAD, keep lines still attributed to $SHA.
    surv="$(git blame --line-porcelain HEAD -- "$f" 2>/dev/null | $PY "$PARSER" "$SHA" "$CONTEXT" "$f")"

    [ -z "$surv" ] && continue
    alive="$(printf '%s\n' "$surv" | sed -n 's/^__ALIVE__ //p')"
    total_alive=$((total_alive + alive))
    files_with_survivors=$((files_with_survivors + 1))
    {
        echo "=== $f ===  ($alive surviving line(s))"
        printf '%s\n' "$surv" | grep -v '^__ALIVE__ '
    } >> "$TMP_BODY"
done

emit_header
superseded=$((total_introduced - total_alive))
[ "$superseded" -lt 0 ] && superseded=0
echo "# Summary: $files_with_survivors file(s) with survivors; ~$total_alive/$total_introduced introduced line(s) still alive (~$superseded superseded)."
echo "#"
if [ "$files_with_survivors" -eq 0 ]; then
    echo "# FULLY SUPERSEDED — every line this change introduced has since been"
    echo "# modified or removed. Nothing to review."
    exit 0
fi
echo
cat "$TMP_BODY"
