#!/usr/bin/env bats
# tests/bats/small_helper_audit.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/small_helper_audit.py — the
# gate-blind-spot-sweep Slice 2 sub-floor helper-clone census. ADVISORY (WARN-first,
# never blocking) per ADR-0015's graduation path, so these tests pin the DETECTION
# semantics rather than an exit code.
#
# Covers: --selftest; and, via throwaway git repos, the three narrowings that make a
# sub-MIN_CLONE_TOKENS band readable at all —
#   * a copy-then-RENAME across 3 TUs IS reported (the whole point);
#   * the same body in only 2 TUs is NOT (a pair is not a pattern);
#   * a genuinely different body does NOT group with it;
#   * a body at or above dup_audit's floor is left to the DRY gate;
#   * an `if (...) {` / a call statement is never extracted as a function;
#   * the baseline grandfathers a group so --check stops WARNing about it.
#
# The band CEILING is imported from dup_audit.py rather than copied — the selftest
# asserts that, so a future MIN_CLONE_TOKENS retune cannot silently open a gap
# between the two audits.
#
# There is no bats on the Windows CI runner, so these run at the pre-push gate and in
# the ubuntu "Agentic self-tests (bats)" lane (match dup_audit.bats structure).
#
# Requires: bash, bats, a working python interpreter (python3/python/py), git.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/small_helper_audit.py"
    export GATE="$REPO_ROOT/agents/scripts/core/test-small-helper-audit.sh"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
    TMP="$(mktemp -d)"
    export TMP
}

teardown() {
    [ -n "${TMP:-}" ] && rm -rf "$TMP"
    return 0
}

_mk_repo() {
    mkdir -p "$1/Source/Core/src"
    git init -q "$1"
    git -C "$1" config user.email t@t.t
    git -C "$1" config user.name t
}

# Write a ~54-token ASCII-lower helper named $2 into TU $1 — squarely inside the band.
_lower_helper() {
    cat > "$1" <<EOF
#include <algorithm>
#include <string>
std::string $2(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
EOF
}

_list() {
    (cd "$1" && "$PY" "$AUD" --list 2>&1)
}

# ---------- selftest ----------

@test "selftest passes (extractor + banding + spread invariants in sync)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"invariants hold"* ]]
}

@test "the shell gate's --selftest passes" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"Failed: 0"* ]]
}

@test "the band ceiling is dup_audit's MIN_CLONE_TOKENS, not a copied constant" {
    run "$PY" -c "
import sys; sys.path.insert(0, '$REPO_ROOT/agents/scripts/core')
import small_helper_audit as s, dup_audit as d
assert s.HELPER_MAX_TOKENS is d.MIN_CLONE_TOKENS or s.HELPER_MAX_TOKENS == d.MIN_CLONE_TOKENS
print('ceiling', s.HELPER_MAX_TOKENS)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ceiling"* ]]
}

# ---------- the three narrowings ----------

@test "a copy-then-RENAME across 3 TUs IS reported" {
    _mk_repo "$TMP/r"
    _lower_helper "$TMP/r/Source/Core/src/A.cpp" ToLowerAscii
    _lower_helper "$TMP/r/Source/Core/src/B.cpp" LowerAscii
    _lower_helper "$TMP/r/Source/Core/src/C.cpp" AsciiLowerCopy
    git -C "$TMP/r" add -A
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ToLowerAscii"* ]]
    [[ "$output" == *"LowerAscii"* ]]
    [[ "$output" == *"AsciiLowerCopy"* ]]
}

@test "the same body in only 2 TUs is NOT reported (a pair is not a pattern)" {
    _mk_repo "$TMP/r"
    _lower_helper "$TMP/r/Source/Core/src/A.cpp" ToLowerAscii
    _lower_helper "$TMP/r/Source/Core/src/B.cpp" LowerAscii
    git -C "$TMP/r" add -A
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "a genuinely different body does NOT group with the std::tolower family" {
    _mk_repo "$TMP/r"
    _lower_helper "$TMP/r/Source/Core/src/A.cpp" ToLowerAscii
    _lower_helper "$TMP/r/Source/Core/src/B.cpp" LowerAscii
    cat > "$TMP/r/Source/Core/src/C.cpp" <<'EOF'
#include <algorithm>
#include <string>
std::string BranchyLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; });
    return s;
}
EOF
    git -C "$TMP/r" add -A
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    # Only 2 TUs share the std::tolower body once the branchy one is excluded -> no group at all.
    [ -z "$output" ]
}

@test "a body at or above dup_audit's floor is left to the DRY gate" {
    _mk_repo "$TMP/r"
    local big
    big="$(printf 'Step(a); %.0s' $(seq 1 40))"
    for tu in A B C; do
        printf 'void Step(int);\nvoid Big%s(int a) { %s }\n' "$tu" "$big" \
            > "$TMP/r/Source/Core/src/$tu.cpp"
    done
    git -C "$TMP/r" add -A
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"Big"* ]]
}

@test "an if-statement head is never extracted as a function definition" {
    _mk_repo "$TMP/r"
    for tu in A B C; do
        cat > "$TMP/r/Source/Core/src/$tu.cpp" <<'EOF'
void Work(int);
void Outer(bool ready, int a) {
    if (ready) {
        Work(a); Work(a); Work(a); Work(a); Work(a); Work(a); Work(a); Work(a); Work(a);
    }
}
EOF
    done
    git -C "$TMP/r" add -A
    run _list "$TMP/r"
    [ "$status" -eq 0 ]
    [[ "$output" != *"	if"* ]]
}

# ---------- baseline grandfathering ----------

@test "--check WARNs on a new group and stops once it is baselined" {
    _mk_repo "$TMP/r"
    _lower_helper "$TMP/r/Source/Core/src/A.cpp" ToLowerAscii
    _lower_helper "$TMP/r/Source/Core/src/B.cpp" LowerAscii
    _lower_helper "$TMP/r/Source/Core/src/C.cpp" AsciiLowerCopy
    git -C "$TMP/r" add -A

    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]

    mkdir -p "$TMP/r/docs/high-integrity"
    (cd "$TMP/r" && "$PY" "$AUD" --baseline-md) > "$TMP/r/docs/high-integrity/small-helper-baseline.md"
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN"* ]]
    [[ "$output" == *"PASS"* ]]
}

@test "--check NOTEs a baselined group that is no longer duplicated (burn-down ratchet)" {
    _mk_repo "$TMP/r"
    mkdir -p "$TMP/r/docs/high-integrity"
    _lower_helper "$TMP/r/Source/Core/src/A.cpp" ToLowerAscii
    _lower_helper "$TMP/r/Source/Core/src/B.cpp" LowerAscii
    _lower_helper "$TMP/r/Source/Core/src/C.cpp" AsciiLowerCopy
    git -C "$TMP/r" add -A
    (cd "$TMP/r" && "$PY" "$AUD" --baseline-md) > "$TMP/r/docs/high-integrity/small-helper-baseline.md"

    # Migrate two of the three onto the survivor — the group drops below the spread floor.
    printf '#include <string>\nstd::string LowerAscii(std::string);\n' > "$TMP/r/Source/Core/src/B.cpp"
    printf '#include <string>\nstd::string AsciiLowerCopy(std::string);\n' > "$TMP/r/Source/Core/src/C.cpp"
    git -C "$TMP/r" add -A
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 0 ]
    [[ "$output" == *"NOTE"* ]]
}

# ---------- fail-closed ----------

@test "a tree with no first-party C++ is an infra error, not a silent pass" {
    _mk_repo "$TMP/r"
    rm -rf "$TMP/r/Source"
    printf 'placeholder\n' > "$TMP/r/README.md"
    git -C "$TMP/r" add -A
    run bash -c "cd '$TMP/r' && '$PY' '$AUD' --check"
    [ "$status" -eq 2 ]
    [[ "$output" == *"fail closed"* ]]
}
