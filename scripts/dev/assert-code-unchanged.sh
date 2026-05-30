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
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$SCRIPT_DIR/comment_lib.py"
BASE="${1:-origin/develop}"
PY="${PYTHON:-python3}"

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
    base_res="$(git show "$BASE:$f" 2>/dev/null | "$PY" "$LIB" --residue 2>/dev/null)"
    head_res="$("$PY" "$LIB" --residue < "$f" 2>/dev/null)"
    checked=$((checked + 1))
    if [ "$base_res" != "$head_res" ]; then
        fail=1
        echo "CODE-RESIDUE CHANGED (non-comment edit): $f" >&2
        diff <(printf '%s' "$base_res") <(printf '%s' "$head_res") | head -20 >&2
        echo "---" >&2
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "[assert-code-unchanged] FAIL — a non-comment code change slipped in (see above)." >&2
    exit 1
fi
echo "[assert-code-unchanged] PASS — $checked changed file(s) differ only in comments/whitespace."
exit 0
