#!/usr/bin/env bash
# test-pre-push-stage-neutralisers.sh — hermeticity lint for pre-push self-tests
# (test/2026-06-24-pre-push-guard-test-not-hermetic, the #1552 recurrence guard).
#
# A unit test that drives the WHOLE scripts/git-hooks/pre-push through a
# stripped `env -i` sandbox must neutralise every independent stage it is NOT
# testing, or a sibling stage gaining a new environment dependency silently
# breaks the test on the merge-ref checkout (green locally, red only in CI).
#
# Rule: extract the SMATCHET_* escape vars guarding each independent hook stage
# (the `${SMATCHET_X:-0}` overrides), then require every `env -i … $HOOK`
# invocation in agents/scripts/core/test-pre-push-*.sh to set EACH of them —
# as a neutraliser or as the var under test. Backslash-continued invocations
# are joined before matching.
#
# Seams (selftest fixtures): PRE_PUSH_HOOK (hook path), PRE_PUSH_TEST_DIR
# (dir scanned for test-pre-push-*.sh).
#
# Modes:
#   (no args) | --check   scan the real tree; FAIL (1) listing each missing pair.
#   --selftest            synth a hook + a test omitting one neutraliser ->
#                         assert FAIL; add it -> assert PASS.
#                         # selftest: asserts-failure
#
# Exit: 0 clean · 1 missing neutraliser(s) · 2 infra error.

set -uo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || pwd)" || exit 2

SELF_BASE="test-pre-push-stage-neutralisers.sh"

escape_vars() {
    grep -oE '\$\{SMATCHET_[A-Z_]+:-0\}' "$1" | grep -oE 'SMATCHET_[A-Z_]+' | sort -u
}

# Print $1 with backslash-continued lines joined, so a multi-line `env -i`
# block is matched as one invocation.
joined() {
    sed -e ':a' -e '/\\$/{N;s/\\\n//;ba}' "$1"
}

# check_tree <hook> <testdir> — 0 clean, 1 violation(s) (printed), 2 infra.
check_tree() {
    local hook="$1" testdir="$2" vars f line v rc=0 found_any=0
    [ -f "$hook" ] || { echo "$SELF_BASE: hook not found: $hook" >&2; return 2; }
    vars="$(escape_vars "$hook")"
    if [ -z "$vars" ]; then
        echo "$SELF_BASE: no \${SMATCHET_*:-0} escape vars found in $hook — extraction broken (fail-closed)" >&2
        return 2
    fi
    for f in "$testdir"/test-pre-push-*.sh; do
        [ -f "$f" ] || continue
        [ "$(basename "$f")" = "$SELF_BASE" ] && continue
        while IFS= read -r line; do
            found_any=1
            for v in $vars; do
                case "$line" in
                    *"$v"*) ;;
                    *)
                        echo "  - $f: env -i hook invocation omits $v (set it: neutraliser for a stage this test does not exercise)"
                        rc=1
                        ;;
                esac
            done
        done < <(joined "$f" | grep -E 'env -i' | grep -E '\$HOOK|pre-push' || true)
    done
    [ "$found_any" -eq 0 ] && echo "$SELF_BASE: note — no env -i hook invocations under $testdir (nothing to lint)"
    return "$rc"
}

selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/tests"
    cat > "$tmp/hook" <<'EOF'
[ "${SMATCHET_FOO_GATE:-0}" = "1" ] && exit 0
[ "${SMATCHET_BAR_GATE:-0}" = "1" ] && exit 0
EOF
    cat > "$tmp/tests/test-pre-push-fixture.sh" <<'EOF'
HOOK=hook
out=$(env -i \
    PATH="$PATH" \
    SMATCHET_FOO_GATE=1 \
    bash "$HOOK")
EOF
    # Missing SMATCHET_BAR_GATE -> must FAIL.
    # selftest: asserts-failure
    if check_tree "$tmp/hook" "$tmp/tests" >/dev/null 2>&1; then
        echo "selftest FAIL: missing neutraliser not flagged" >&2
        rc=1
    fi
    printf 'out=$(env -i SMATCHET_FOO_GATE=1 SMATCHET_BAR_GATE=1 bash "$HOOK")\n' \
        > "$tmp/tests/test-pre-push-fixture.sh"
    if ! check_tree "$tmp/hook" "$tmp/tests" >/dev/null 2>&1; then
        echo "selftest FAIL: fully-neutralised invocation flagged" >&2
        rc=1
    fi
    rm -rf "$tmp"
    [ "$rc" -eq 0 ] && echo "$SELF_BASE: selftest OK"
    return "$rc"
}

MODE="${1:---check}"
case "$MODE" in
    --selftest)
        if selftest; then echo "Passed: 1  Failed: 0"; exit 0; fi
        echo "Passed: 0  Failed: 1"; exit 1 ;;
    --check)
        ;;
    *)
        echo "usage: $SELF_BASE [--check|--selftest]" >&2; exit 2 ;;
esac

HOOK_FILE="${PRE_PUSH_HOOK:-scripts/git-hooks/pre-push}"
TEST_DIR="${PRE_PUSH_TEST_DIR:-agents/scripts/core}"

# Dogfood the selftest first (gate-selftests convention), then scan the tree.
if ! selftest >/dev/null 2>&1; then
    echo "$SELF_BASE: selftest failed — refusing to certify the tree" >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

check_tree "$HOOK_FILE" "$TEST_DIR"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "$SELF_BASE: PASS — every env -i pre-push invocation sets all stage escape vars."
    echo "Passed: 1  Failed: 0"
    exit 0
elif [ "$rc" -eq 1 ]; then
    echo "$SELF_BASE: FAIL — env -i pre-push invocation(s) missing stage neutralisers (see above)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
echo "Passed: 0  Failed: 1"
exit 2
