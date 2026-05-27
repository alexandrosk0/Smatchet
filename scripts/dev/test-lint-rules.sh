#!/usr/bin/env bash
# test-lint-rules.sh — mechanical enforcement of AGENTS.md logging + RAII rules.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

PASS=0; FAIL=0

check() {
    local desc="$1"; shift
    local hits
    hits=$(eval "$@" 2>/dev/null || true)
    if [ -z "$hits" ]; then
        echo "  PASS  $desc"
        PASS=$((PASS+1))
    else
        echo "  FAIL  $desc"
        echo "$hits" | sed 's/^/    /'
        FAIL=$((FAIL+1))
    fi
}

# Unexempted printf/fprintf/cerr/cout in Source_Core, Plugins, or Target_Standalone
check "no unexempted printf/fprintf/cerr in Source_Core+Plugins+Target_Standalone" \
    "grep -rn --include='*.cpp' --include='*.h' --include='*.hpp' \
        '\bprintf\b\|\bfprintf\b\|\bstd::cerr\b\|\bstd::cout\b' \
        Source_Core/ Plugins/ Target_Standalone/ \
    | grep -v '// CLI stdout\|// pre-logger-init\|// C-ABI\|// custom-deleter\|// pimpl\|ThirdParty/\|#include\|//.*printf'"

# Unexempted raw new in Source_Core, Plugins, or Target_Standalone:
# - exclude known-exempt comment markers
# - exclude lines where 'new' only appears inside a string literal or doc comment
# - exclude lines that are pure doc/comments (* prefix or // prefix)
# - exclude keyword/string arrays and string comparisons
check "no unexempted raw new in Source_Core+Plugins+Target_Standalone" \
    "grep -rn --include='*.cpp' --include='*.h' --include='*.hpp' \
        '\bnew\b' \
        Source_Core/ Plugins/ Target_Standalone/ \
    | grep -v '// C-ABI\|// custom-deleter\|// pimpl\|NOLINT\|ThirdParty/\|//.*new\|#\|_new\|new_\|renewed' \
    | grep -v '^\s*\*\|^\s*//' \
    | grep -v '\"[^\"]*new[^\"]*\"' \
    | grep 'new\s*[A-Z]'"

echo "==============================="
echo "Passed: $PASS  Failed: $FAIL"
echo "==============================="
[ "$FAIL" -eq 0 ]
