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

# Git-for-Windows / MSYS arg-mangling note. MSYS rewrites any argument that
# looks like a POSIX path-list — notably the "<rev>:<path>" form — turning ':'
# into ';' and '/' into '\', so e.g. `git cat-file -e "origin/develop:.github/
# workflows/x.yml"` reached git as the bogus object name "origin\develop;
# .github\workflows\x.yml" (fatal=128) and the existence guard SILENTLY dropped
# live survivor files under .github/ etc. The fix is STRUCTURAL — the existence
# guard below uses `git ls-tree <ref> -- <path>` (separate args, no colon) so
# nothing trips the heuristic. The parser temp file (an MSYS "/tmp/…py" path) is
# handed to native python.exe as a cygpath-normalised Windows path (see PARSER_PY
# below), so it resolves whether or not MSYS path-conversion is on — an operator
# may safely export MSYS2_ARG_CONV_EXCL / MSYS_NO_PATHCONV globally without breaking
# the python handoff. (Before that hardening, a global export fed python.exe a raw
# "/tmp/…" it read as "C:\tmp\…" and could not open.)

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
AGAINST=""
while [ $# -gt 0 ]; do
    case "$1" in
        --pr) PR="${2:-}"; shift 2 ;;
        --pr=*) PR="${1#*=}"; shift ;;
        --context) CONTEXT="${2:-0}"; shift 2 ;;
        --context=*) CONTEXT="${1#*=}"; shift ;;
        --against) AGAINST="${2:-}"; shift 2 ;;
        --against=*) AGAINST="${1#*=}"; shift ;;
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

# Resolve the "alive" reference. CRITICAL: review against the CANONICAL LATEST,
# not a possibly-stale local HEAD — else a line a newer PR already fixed (absent
# from a behind-by-N local checkout) reappears as a false survivor and gets
# re-flagged, defeating the whole point. Default: fetch + use origin's default
# branch (origin/develop). --against <ref> overrides; falls back to HEAD when no
# remote exists (e.g. test sandboxes).
REVIEW_REF="$AGAINST"
STALE_NOTE=""
if [ -z "$REVIEW_REF" ]; then
    git fetch -q origin 2>/dev/null || true
    # Take the first candidate that actually RESOLVES. `git rev-parse
    # --abbrev-ref origin/HEAD` ECHOES its unresolvable argument on stdout
    # ("origin/HEAD") while exiting 128, so a non-empty result proves nothing —
    # it has to be verified before use. Guarding the origin/develop fallback on
    # `[ -z "$DEFREF" ]` (as this did) meant that echoed string suppressed the
    # fallback and the whole block fell through to local HEAD: in any clone
    # without origin/HEAD set, every run silently reviewed against whatever
    # branch was checked out — the stale baseline this block exists to avoid,
    # and the one that makes already-fixed lines reappear as false survivors.
    DEFREF=""
    for _cand in "$(git rev-parse --abbrev-ref origin/HEAD 2>/dev/null || true)" origin/develop; do
        [ -n "$_cand" ] || continue
        if git rev-parse --verify -q "${_cand}^{commit}" >/dev/null 2>&1; then
            DEFREF="$_cand"
            break
        fi
    done
    if [ -n "$DEFREF" ]; then
        REVIEW_REF="$DEFREF"
    else
        # No origin ref resolved (test sandbox, or a remote-less checkout).
        # Reviewing against local HEAD is still the only option, but it is NOT
        # the canonical latest — say so rather than degrade quietly.
        REVIEW_REF="HEAD"
        STALE_NOTE="# NOTE: no origin ref resolved; survivors computed against LOCAL HEAD, not the canonical latest — lines a newer commit already fixed may reappear here."
    fi
fi
if ! git rev-parse --verify -q "${REVIEW_REF}^{commit}" >/dev/null 2>&1; then
    echo "historical-review-survivors: review ref '$REVIEW_REF' does not resolve" >&2
    exit 2
fi
# Warn if local HEAD trails the review ref — the operator's tree is behind the
# baseline the survivors are computed against (informational; results are still
# correct because we blame REVIEW_REF, not HEAD).
if [ "$REVIEW_REF" != "HEAD" ]; then
    behind="$(git rev-list --count "HEAD..${REVIEW_REF}" 2>/dev/null || echo 0)"
    [ "${behind:-0}" -gt 0 ] && STALE_NOTE="# NOTE: local HEAD is $behind commit(s) behind $REVIEW_REF; survivors computed against $REVIEW_REF (canonical latest)."
fi

# Must be an ancestor of the review ref (else "alive at $REVIEW_REF" is meaningless).
if ! git merge-base --is-ancestor "$SHA" "$REVIEW_REF" 2>/dev/null; then
    echo "historical-review-survivors: $LABEL ($SHA) is not an ancestor of $REVIEW_REF — nothing to review against that tree" >&2
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
    echo "# Reviewed against: $REVIEW_REF (the canonical-latest tree)."
    [ -n "$STALE_NOTE" ] && echo "$STALE_NOTE"
    echo "# Lines this change introduced that are STILL ALIVE + UNTOUCHED at $REVIEW_REF."
    echo "# Lines a later PR modified/fixed are EXCLUDED by construction (git blame"
    echo "# re-attributes them). Review ONLY the lines below; do not re-flag what is gone."
    echo "#"
}

TMP_BODY="$(mktemp)"
PARSER="$(mktemp --suffix=.py 2>/dev/null || mktemp)"
trap 'rm -f "$TMP_BODY" "$PARSER"' EXIT

# Path to hand the Python interpreter. On Git-for-Windows, mktemp emits a POSIX
# path (/tmp/tmp.XXXX.py) but $PY is usually a NATIVE Windows python, which reads
# /tmp/.. as the drive-relative C:\tmp\.. — a different, nonexistent location. MSYS
# auto-converts the arg only while path-conversion is ON; an operator who globally
# exports MSYS_NO_PATHCONV / MSYS2_ARG_CONV_EXCL (e.g. to keep <rev>:<path> git args
# intact) disables it and breaks the handoff. cygpath -w gives a path both MSYS and
# native python resolve identically regardless of conversion state; absent (Linux/
# macOS) it's a plain no-op. $PARSER itself stays POSIX so the trap's rm still finds it.
PARSER_PY="$PARSER"
if command -v cygpath >/dev/null 2>&1; then
    PARSER_PY="$(cygpath -w "$PARSER")"
fi

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
ref = sys.argv[4] if len(sys.argv) > 4 else "HEAD"
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
    full = subprocess.run(["git", "show", f"{ref}:{path}"],
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
    # Existence guard against the review ref. Use `ls-tree` (separate <ref> and
    # `-- <path>` args, no colon) NOT `cat-file -e "<ref>:<path>"`: the colon
    # form trips MSYS arg-mangling (see top-of-file note) AND can't tell
    # "absent" from "errored" by exit code — both are fatal=128. ls-tree avoids
    # the colon and gives a clean three-way so a future mangling-class bug
    # surfaces LOUDLY instead of silently dropping a live file:
    #   nonzero rc      -> the check itself errored: warn, do NOT skip
    #   rc 0 + empty    -> genuinely absent at the ref (deleted/renamed): skip
    #   rc 0 + nonempty -> present at the ref: review it
    if ! present="$(git ls-tree --name-only "$REVIEW_REF" -- "$f" 2>/dev/null)"; then
        echo "historical-review-survivors: WARNING — existence check for '$f' at '$REVIEW_REF' errored; NOT skipping it (possible path-mangling regression — review '$f' manually)." >&2
    elif [ -z "$present" ]; then
        continue
    fi

    # Net lines this commit added to this file (for the supersede ratio).
    numstat="$(git log -1 --format= --numstat "$SHA" -- "$f")"

    # A BINARY path carries no reviewable lines. numstat reports it as "-  -",
    # so it already contributed 0 to `added` — but `git blame` still emits a
    # "line" per chunk of bytes, so the file landed in the digest with a large
    # survivor count anyway. That is how a golden PNG scored 1173 surviving
    # "lines" and outranked every source file, and why the totals could read
    # alive > introduced. Skip it: there is nothing a reviewer can act on.
    # Flag, not `exit 0` in the rule: awk still runs END after a rule-level
    # exit, and END's own exit status wins — so `{exit 0} END{exit 1}` is
    # always 1 and the branch never fires.
    if printf '%s\n' "$numstat" | awk 'NF>=3 && $1=="-"{found=1} END{exit !found}'; then
        continue
    fi

    added="$(printf '%s\n' "$numstat" | awk 'NF>=3 && $1!="-"{s+=$1} END{print s+0}')"
    total_introduced=$((total_introduced + added))

    # Surviving lines: blame the review ref, keep lines still attributed to $SHA.
    surv="$(git blame --line-porcelain "$REVIEW_REF" -- "$f" 2>/dev/null | $PY "$PARSER_PY" "$SHA" "$CONTEXT" "$f" "$REVIEW_REF")"

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
