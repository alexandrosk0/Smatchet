#!/usr/bin/env bash
# assert-code-unchanged.sh — the safety gate for the reduce-source-comment-bloat sweep.
#
# For every first-party C++ file changed vs a base ref, compute the comment-stripped +
# whitespace-normalized "code-token residue" (via comment_lib.py --residue) of the BASE and the
# WORKING-TREE versions and require them to be byte-identical. A non-empty diff means the change
# touched executable code, not just comments — fail. This is what makes a comment sweep provably
# behaviour-preserving without an eyeball.
#
# Usage: assert-code-unchanged.sh [base-ref]      (default: origin/develop)
# Exit:  0 = only comments/whitespace changed; 1 = code residue differs; 2 = setup error.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$SCRIPT_DIR/comment_lib.py"
BASE="${1:-origin/develop}"
# Honor $PYTHON override; else probe for a python that actually runs (the Windows
# python3 Store-alias stub passes command -v but exits 49 when invoked).
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
    for _c in python3 python py; do
        _p="$(command -v "$_c" 2>/dev/null)" || continue
        if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
    done
fi

command -v "$PY" >/dev/null 2>&1 || { echo "assert-code-unchanged: $PY not found" >&2; exit 2; }
[ -f "$LIB" ] || { echo "assert-code-unchanged: $LIB missing" >&2; exit 2; }

# First-party C++ changed vs base (added/copied/modified/renamed; deletions can't change code residue).
mapfile -t FILES < <(git diff --name-only --diff-filter=ACMR "$BASE" -- \
    'Source/Core/***.cpp' 'Source/Core/***.h' 'Source/Core/***.hpp' \
    'Source/Plugins/***.cpp' 'Source/Plugins/***.h' 'Source/Plugins/***.hpp' \
    'Source/Standalone/***.cpp' 'Source/Standalone/***.h' 'Source/Standalone/***.hpp' \
    2>/dev/null | grep -v '/ThirdParty/' || true)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "[assert-code-unchanged] no first-party C++ changes vs $BASE — pass"
    exit 0
fi

fail=0
checked=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue   # deleted in working tree
    # A file absent at BASE is newly Added — not a comment-only edit of existing code; the build +
    # tests cover it. Skip (don't fail) those; the sweep never adds .cpp/.h.
    if ! base_src="$(git show "$BASE:$f" 2>/dev/null)"; then
        echo "[assert-code-unchanged] note: $f is new vs $BASE (no base) — skipping residue check" >&2
        continue
    fi
    # FAIL CLOSED on any residue-computation error: a suppressed crash on BOTH sides would yield
    # two empty strings that compare equal → a false PASS. Surface stderr and treat errors as failures.
    if ! base_res="$(printf '%s' "$base_src" | "$PY" "$LIB" --residue)"; then
        echo "CODE-RESIDUE ERROR (base) for $f — failing closed" >&2; fail=1; continue
    fi
    if ! head_res="$("$PY" "$LIB" --residue < "$f")"; then
        echo "CODE-RESIDUE ERROR (head) for $f — failing closed" >&2; fail=1; continue
    fi
    checked=$((checked + 1))
    if [ "$base_res" != "$head_res" ]; then
        fail=1
        echo "CODE-RESIDUE CHANGED (non-comment edit): $f" >&2
        diff <(printf '%s' "$base_res") <(printf '%s' "$head_res") | head -20 >&2 || true  # diff exits 1 on the known-differing residue (and head can SIGPIPE) — keep accumulating fail across files
        echo "---" >&2
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "[assert-code-unchanged] FAIL — a non-comment code change slipped in (see above)." >&2
    exit 1
fi
echo "[assert-code-unchanged] PASS — $checked changed file(s) differ only in comments/whitespace."
exit 0
