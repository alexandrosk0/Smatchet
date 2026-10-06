#!/usr/bin/env bats
# tests/bats/coverage_gate.bats
# ----------------------------------------------------------------------------
# Bats coverage for the two coverage-gate scripts' --selftest surfaces:
#   * scripts/dev/coverage.sh            — the test-binary-vs-OpenCppCoverage-tooling
#                                          exit split (quarantine-safe capture fix).
#   * scripts/dev/coverage-delta-gate.sh — the test-light exemption classifier,
#                                          incl. the multi-line wrapped LOG_* join.
#
# These scripts each ship a hermetic `--selftest` (no build dir / exe / network
# needed); this suite just runs them under bats so a regression reds a discoverable
# test-*.sh wrapper (test-all.sh + test-orphan-bats.sh), not only a hand-run flag.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export COVERAGE="$REPO_ROOT/scripts/dev/coverage.sh"
    export DELTA_GATE="$REPO_ROOT/scripts/dev/coverage-delta-gate.sh"
}

# ---------- coverage.sh: test-binary vs tooling exit split ----------

@test "coverage.sh --selftest passes (test-binary vs OpenCppCoverage tooling exit split)" {
    run bash "$COVERAGE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"coverage.sh --selftest: PASS"* ]]
}

# ---------- coverage.sh: quarantine-exclude wired into BOTH captures ----------
# The quarantine-safe capture fix (ci-falsepositive-hardening Cluster-C /
# coverage-quarantine-safe) passes `--test-case-exclude=*[quarantined:*]*` to the
# doctest child on every capture so a known-flaky quarantined case cannot flake
# the Coverage lane and masquerade as a coverage regression. That flag living in
# the script is the entire fix — if it is silently dropped the false-red returns.
# This pins it: the flag must be defined AND handed to both the SmatchetTests and
# SmatchetLuaTests capture invocations (a full OpenCppCoverage integration run
# needs the Windows toolchain, so this is a static regression-pin, not a capture).
@test "coverage.sh: quarantine-exclude flag defined and passed to both captures" {
    # Defined once as the shared flag var.
    run grep -qF "DOCTEST_EXCLUDE_QUARANTINED='--test-case-exclude=*[quarantined:*]*'" "$COVERAGE"
    [ "$status" -eq 0 ]
    # Handed to the doctest child (after `--`) on exactly the two capture lines —
    # SmatchetTests + SmatchetLuaTests. Two references beyond the definition.
    run bash -c "grep -c '\"\$DOCTEST_EXCLUDE_QUARANTINED\"' '$COVERAGE'"
    [ "$status" -eq 0 ]
    [ "$output" -ge 2 ]
}

# ---------- coverage.sh: infra-crash vs test/threshold verdicts (stubbed end-to-end) ----------
# Backlog infra.md 2026-06-14 ci-infra-flake-reds-masquerade-as-real-breakage: an
# OpenCppCoverage crash used to surface as a `0%` threshold red. These cases drive the real
# script against a stub OpenCppCoverage (the OPENCPPCOVERAGE_EXE seam) and dummy test exes,
# pinning the exit contract without the Windows toolchain:
#   3 + COVERAGE-INFRA-CRASH — no coverage data even after the one retry (capture or merge);
#   1 — a test binary failed under capture;
#   4 — a genuine threshold miss (the only code coverage-out-of-band may waive);
#   0 — clean, or a transient tooling crash rescued by the retry.

# cov_stub_setup — dummy build dir + stub OpenCppCoverage under $COVDIR. The stub reads
# COV_CAPTURE_PLAN (one word per capture call: ok | crash | test) and COV_MERGE_PLAN (one
# word per merge call: ok | empty | fail), writes COV_RATE as the Cobertura line-rate, and
# counts its calls in $COVDIR/{capture,merge}.count.
cov_stub_setup() {
    COVDIR="$BATS_TEST_TMPDIR/cov"
    mkdir -p "$COVDIR/build/tests/Lua" "$COVDIR/bin" "$COVDIR/out"
    : > "$COVDIR/build/tests/SmatchetTests.exe"
    : > "$COVDIR/build/tests/Lua/SmatchetLuaTests.exe"
    echo 0 > "$COVDIR/capture.count"
    echo 0 > "$COVDIR/merge.count"
    # coverage.sh parses the XML with `python`; shim it where only python3 exists.
    if ! command -v python >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
        printf '#!/bin/sh\nexec python3 "$@"\n' > "$COVDIR/bin/python"
        chmod +x "$COVDIR/bin/python"
    fi
    cat > "$COVDIR/bin/occ" <<'STUB'
#!/usr/bin/env bash
kind=capture; out=""; prev=""
for a in "$@"; do
    [ "$a" = "--input_coverage" ] && kind=merge
    if [ "$prev" = "--export_type" ]; then
        case "$a" in binary:*) out="${a#binary:}" ;; cobertura:*) out="${a#cobertura:}" ;; esac
    fi
    prev="$a"
done
n=$(( $(cat "$COVDIR/$kind.count") + 1 )); echo "$n" > "$COVDIR/$kind.count"
if [ "$kind" = capture ]; then read -r -a plan <<< "$COV_CAPTURE_PLAN"; else read -r -a plan <<< "$COV_MERGE_PLAN"; fi
step="${plan[$((n - 1))]:-ok}"
case "$kind:$step" in
    capture:crash|merge:fail) exit 1 ;;
    capture:test) printf 'cov' > "$out"; exit 3 ;;
    capture:*) printf 'cov' > "$out"; exit 0 ;;
    merge:empty) : > "$out"; exit 0 ;;
    *) printf '<?xml version="1.0"?>\n<coverage line-rate="%s" version="1.9">\n</coverage>\n' "$COV_RATE" > "$out"; exit 0 ;;
esac
STUB
    chmod +x "$COVDIR/bin/occ"
    export COVDIR
}

# run_cov <capture-plan> <merge-plan> <rate> — run coverage.sh the way coverage.yml does.
run_cov() {
    run env PATH="$COVDIR/bin:$PATH" OPENCPPCOVERAGE_EXE="$COVDIR/bin/occ" \
        SMATCHET_COVERAGE_BUILD_DIR="$COVDIR/build" SMATCHET_COVERAGE_OUTPUT_DIR="$COVDIR/out" \
        COV_CAPTURE_PLAN="$1" COV_MERGE_PLAN="$2" COV_RATE="$3" \
        bash "$COVERAGE" --xml-only --threshold 70
}

@test "coverage.sh: clean capture + merge above threshold exits 0 with no infra marker" {
    cov_stub_setup
    run_cov "ok ok" "ok" "0.80"
    [ "$status" -eq 0 ]
    [[ "$output" == *"line coverage: 80%"* ]]
    [[ "$output" != *"COVERAGE-INFRA-CRASH"* ]]
}

@test "coverage.sh: a transient capture crash is retried once and recovers (exit 0)" {
    cov_stub_setup
    run_cov "crash ok ok" "ok" "0.80"
    [ "$status" -eq 0 ]
    [[ "$output" == *"retrying the capture once"* ]]
    [ "$(cat "$COVDIR/capture.count")" -eq 3 ]
    [[ "$output" != *"COVERAGE-INFRA-CRASH"* ]]
}

@test "coverage.sh: a persistent capture crash is INFRA -  exit 3 + COVERAGE-INFRA-CRASH, never a 0% red" {
    cov_stub_setup
    run_cov "crash crash ok" "ok" "0.80"
    [ "$status" -eq 3 ]
    [[ "$output" == *"::error title=COVERAGE-INFRA-CRASH::"*"SmatchetTests"* ]]
    [[ "$output" != *"line coverage:"* ]]
    [ "$(cat "$COVDIR/merge.count")" -eq 0 ]
}

@test "coverage.sh: a test-binary failure is exit 1, never retried, no infra marker" {
    cov_stub_setup
    run_cov "test ok" "ok" "0.80"
    [ "$status" -eq 1 ]
    [ "$(cat "$COVDIR/capture.count")" -eq 2 ]
    [[ "$output" == *"real test failure"* ]]
    [[ "$output" != *"COVERAGE-INFRA-CRASH"* ]]
}

@test "coverage.sh: a merge that keeps writing an empty coverage.xml is INFRA (exit 3), not 0%" {
    cov_stub_setup
    run_cov "ok ok" "empty empty" "0.80"
    [ "$status" -eq 3 ]
    [[ "$output" == *"::error title=COVERAGE-INFRA-CRASH::"* ]]
    [[ "$output" != *"line coverage:"* ]]
    [ "$(cat "$COVDIR/merge.count")" -eq 2 ]
}

@test "coverage.sh: a failed merge is retried once and recovers (exit 0)" {
    cov_stub_setup
    run_cov "ok ok" "fail ok" "0.80"
    [ "$status" -eq 0 ]
    [[ "$output" == *"line coverage: 80%"* ]]
    [ "$(cat "$COVDIR/merge.count")" -eq 2 ]
}

@test "coverage.sh: a genuine threshold miss is exit 4 (its own code) with no infra marker" {
    cov_stub_setup
    run_cov "ok ok" "ok" "0.50"
    [ "$status" -eq 4 ]
    [[ "$output" == *"line coverage 50% < threshold 70%"* ]]
    [[ "$output" != *"COVERAGE-INFRA-CRASH"* ]]
}

# ---------- coverage.yml: coverage-out-of-band waives ONLY a threshold miss ----------
# Runs the workflow step's own `run:` block (extracted from the YAML, under the
# Actions bash flags -e -o pipefail) against a stub coverage.sh exiting STUB_RC.

# cov_step <stub-rc> <labels-json> — run the Capture-coverage step body.
cov_step() {
    local d="$BATS_TEST_TMPDIR/step"
    mkdir -p "$d/scripts/dev" "$d/bin"
    printf '#!/usr/bin/env bash\nexit %s\n' "$1" > "$d/scripts/dev/coverage.sh"
    if ! command -v python >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
        printf '#!/bin/sh\nexec python3 "$@"\n' > "$d/bin/python"
        chmod +x "$d/bin/python"
    fi
    awk '/- name: Capture coverage/ { f = 1 }
         f && /^        run: \|/ { r = 1; next }
         r { if ($0 == "" || $0 ~ /^          /) { sub(/^          /, ""); print } else exit }' \
        "$REPO_ROOT/.github/workflows/coverage.yml" > "$d/step.sh"
    grep -q 'coverage.sh --xml-only --threshold 70' "$d/step.sh"
    run env PATH="$d/bin:$PATH" PR_LABELS="$2" bash -c 'cd "$0" && exec bash -e -o pipefail step.sh' "$d"
}

@test "coverage.yml: coverage-out-of-band downgrades a threshold miss (exit 4) to a WARN" {
    cov_step 4 '[{"name":"coverage-out-of-band"}]'
    [ "$status" -eq 0 ]
    [[ "$output" == *"::warning::line coverage below threshold"* ]]
}

@test "coverage.yml: coverage-out-of-band does NOT downgrade a test failure, missing binary or infra crash" {
    for rc in 1 2 3; do
        cov_step "$rc" '[{"name":"coverage-out-of-band"}]'
        [ "$status" -eq "$rc" ]
        [[ "$output" == *"::error::coverage.sh exited $rc"* ]]
        [[ "$output" != *"::warning::"* ]]
    done
}

@test "coverage.yml: without the label a threshold miss is red; a clean run is green either way" {
    cov_step 4 '[]'
    [ "$status" -eq 4 ]
    cov_step 0 '[{"name":"coverage-out-of-band"}]'
    [ "$status" -eq 0 ]
    [[ "$output" != *"::warning::"* ]]
}

# ---------- coverage-delta-gate.sh: classifier incl. wrapped LOG_* join ----------

@test "coverage-delta-gate.sh --selftest passes (classifier + multi-line LOG_ join)" {
    run bash "$DELTA_GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"coverage-delta-gate --selftest: PASS"* ]]
    [[ "$output" == *"2-line wrapped LOG_ERROR"* ]]
    [[ "$output" == *"3-line wrapped LOG_ERROR"* ]]
    [[ "$output" == *"reworded /* */ block opener + untested statement far below"* ]]
    [[ "$output" == *"#if defined(__ANDROID__) inside a raw string literal is not a directive"* ]]
    [[ "$output" == *"#if stack unbalanced at end of file (untrusted) falls through"* ]]
}

# ---------- coverage-delta-gate.sh: TEST_CHANGES recognition ----------
# The gate cds to its own ../.. — copying it into a fixture repo makes that the
# fixture root, so the whole diff pipeline runs hermetically against fixture git.

# make_fixture_repo — a base commit with one real prod TU, on branch main.
make_fixture_repo() {
    FIXREPO="$(mktemp -d)"
    git -C "$FIXREPO" init -q -b main
    git -C "$FIXREPO" config user.email t@t && git -C "$FIXREPO" config user.name t
    mkdir -p "$FIXREPO/scripts/dev" "$FIXREPO/Source/Core/src"
    cp "$DELTA_GATE" "$FIXREPO/scripts/dev/"
    printf 'int foo() { return 1; }\n' > "$FIXREPO/Source/Core/src/a.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm base
    git -C "$FIXREPO" checkout -qb head
    printf 'int foo() { return 2; }\nint bar() { return 3; }\n' > "$FIXREPO/Source/Core/src/a.cpp"
}

@test "coverage-delta-gate.sh: NEW test dir's *.test.cpp earns gate credit (tests/monkey)" {
    make_fixture_repo
    mkdir -p "$FIXREPO/tests/monkey"
    printf 'int t() { return 0; }\n' > "$FIXREPO/tests/monkey/m.test.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm head
    run env SMATCHET_COVERAGE_GATE_BASE=main bash "$FIXREPO/scripts/dev/coverage-delta-gate.sh"
    rm -rf "$FIXREPO"
    [ "$status" -eq 0 ]
    [[ "$output" == *"production + test files both changed"* ]]
}

@test "coverage-delta-gate.sh: tests/support *.test.cpp earns NO credit (dismissable helper)" {
    make_fixture_repo
    mkdir -p "$FIXREPO/tests/support"
    printf 'int t() { return 0; }\n' > "$FIXREPO/tests/support/h.test.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm head
    run env SMATCHET_COVERAGE_GATE_BASE=main bash "$FIXREPO/scripts/dev/coverage-delta-gate.sh"
    rm -rf "$FIXREPO"
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL"* ]]
}

# ==========================================================================
# coverage-delta-gate.sh: full-context exemptions (off-target platform arm +
# header->cpp body relocation). Separate block: real `git diff --unified=100000`
# fixtures, both directions, through the gate's normal (non-selftest) path.
# ==========================================================================

# _wph_repo — a base commit carrying a platform-guarded TU and a header with an
# inline definition, on branch main; leaves the work tree on branch `head`.
_wph_repo() {
    FIXREPO="$(mktemp -d)"
    git -C "$FIXREPO" init -q -b main
    git -C "$FIXREPO" config user.email t@t && git -C "$FIXREPO" config user.name t
    mkdir -p "$FIXREPO/scripts/dev" "$FIXREPO/Source/Core/src" "$FIXREPO/Source/Core/include"
    cp "$DELTA_GATE" "$FIXREPO/scripts/dev/"
    printf '%s\n' '#include "p.h"' '#ifdef _WIN32' 'int Read() { return 1; }' \
        '#elif defined(__ANDROID__)' 'int Read() { return 2; }' '#else' \
        'int Read() { return 3; }' '#endif' > "$FIXREPO/Source/Core/src/p.cpp"
    printf '%s\n' '#pragma once' 'namespace ui {' 'inline int Hook(int x) {' \
        '    if (x > 0) {' '        return x * 2;' '    }' '    return 0;' '}' \
        '}  // namespace ui' > "$FIXREPO/Source/Core/include/h.h"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm base
    git -C "$FIXREPO" checkout -qb head
}

# _wph_insert_after <file> <line-no> <text> — insert one line after line N.
_wph_insert_after() {
    awk -v n="$2" -v t="$3" '{ print } NR == n { print t }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}

# _wph_gate — commit the head edits, run the gate, drop the fixture repo.
_wph_gate() {
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm head
    run env SMATCHET_COVERAGE_GATE_BASE=main bash "$FIXREPO/scripts/dev/coverage-delta-gate.sh"
    rm -rf "$FIXREPO"
}

@test "coverage-delta-gate.sh: statement in an __ANDROID__ elif arm is exempt (no test delta)" {
    _wph_repo
    _wph_insert_after "$FIXREPO/Source/Core/src/p.cpp" 5 'static int g_android = Read();'
    _wph_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"test-light exemption"* ]]
}

@test "coverage-delta-gate.sh: statement in the non-WIN32 else arm still FAILs (Linux CI builds it)" {
    _wph_repo
    _wph_insert_after "$FIXREPO/Source/Core/src/p.cpp" 7 'static int g_posix = Read();'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL: Source/Core/ changes without test deltas"* ]]
}

@test "coverage-delta-gate.sh: statement in an __APPLE__ arm still FAILs (no Apple CI job)" {
    _wph_repo
    printf '%s\n' '#include "p.h"' '#ifdef __APPLE__' 'int Bundle() { return 4; }' '#endif' \
        >> "$FIXREPO/Source/Core/src/p.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm 'apple arm (base)'
    git -C "$FIXREPO" branch -f main HEAD
    _wph_insert_after "$FIXREPO/Source/Core/src/p.cpp" 11 'static int g_apple = Bundle();'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL: Source/Core/ changes without test deltas"* ]]
}

@test "coverage-delta-gate.sh: statement on the _WIN32 if side still FAILs" {
    _wph_repo
    _wph_insert_after "$FIXREPO/Source/Core/src/p.cpp" 3 'static int g_win = Read();'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL"* ]]
}

@test "coverage-delta-gate.sh: inline header body relocated byte-identical to a new .cpp is exempt" {
    _wph_repo
    printf '%s\n' '#pragma once' 'namespace ui {' '// Defined out-of-line in hook.cpp.' \
        'int Hook(int x);' '}  // namespace ui' > "$FIXREPO/Source/Core/include/h.h"
    printf '%s\n' '#include "h.h"' '' 'namespace ui {' '' 'int Hook(int x) {' \
        '    if (x > 0) {' '        return x * 2;' '    }' '    return 0;' '}' '' \
        '}  // namespace ui' > "$FIXREPO/Source/Core/src/hook.cpp"
    _wph_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"test-light exemption"* ]]
}

@test "coverage-delta-gate.sh: relocated body with one edited line still FAILs" {
    _wph_repo
    printf '%s\n' '#pragma once' 'namespace ui {' 'int Hook(int x);' '}  // namespace ui' \
        > "$FIXREPO/Source/Core/include/h.h"
    printf '%s\n' '#include "h.h"' 'namespace ui {' 'int Hook(int x) {' \
        '    if (x >= 0) {' '        return x * 2;' '    }' '    return 0;' '}' \
        '}  // namespace ui' > "$FIXREPO/Source/Core/src/hook.cpp"
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL"* ]]
}

# ==========================================================================
# coverage-delta-gate.sh: classifier state across a full-context diff. The
# --unified=100000 diff is ONE hunk per file, so state the classifier enters on
# a reworded first line of an existing /* */ block or wrapped LOG_*( call must
# not swallow a real statement far below (the default-context diff reset at
# each @@). Also: a `#if defined(__ANDROID__)` that is comment text is not a
# directive (it used to open the #if stack and drop every later '+' line).
# ==========================================================================

# _ctx_repo <first-block-lines...> — a base TU: the given lines, 40 filler
# statements, then `void g(int x) { (void)x; }`; leaves the tree on `head`.
_ctx_repo() {
    FIXREPO="$(mktemp -d)"
    git -C "$FIXREPO" init -q -b main
    git -C "$FIXREPO" config user.email t@t && git -C "$FIXREPO" config user.name t
    mkdir -p "$FIXREPO/scripts/dev" "$FIXREPO/Source/Core/src"
    cp "$DELTA_GATE" "$FIXREPO/scripts/dev/"
    {
        printf '%s\n' '#include "a.h"' "$@"
        for i in $(seq 1 40); do printf 'int v%d = %d;\n' "$i" "$i"; done
        printf '%s\n' 'void g(int x) {' '    (void)x;' '}'
    } > "$FIXREPO/Source/Core/src/a.cpp"
    git -C "$FIXREPO" add -A && git -C "$FIXREPO" commit -qm base
    git -C "$FIXREPO" checkout -qb head
}

# _ctx_edit <sed-expr> — apply one edit plus the untested `launchMissiles(x);`.
_ctx_edit() {
    sed -i -e "$1" -e 's|^    (void)x;|    launchMissiles(x);|' "$FIXREPO/Source/Core/src/a.cpp"
}

@test "coverage-delta-gate.sh: reworded block-comment opener does not exempt a statement far below" {
    _ctx_repo '/* Old first line of the block.' ' * second line' ' */'
    _ctx_edit 's|^/\* Old first line|/* New first line|'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL: Source/Core/ changes without test deltas"* ]]
}

@test "coverage-delta-gate.sh: reworded wrapped LOG_INFO opener does not exempt a statement far below" {
    _ctx_repo 'void f(int x) {' '    LOG_INFO("old {}",' '             x);' '}'
    _ctx_edit 's|LOG_INFO("old {}",|LOG_INFO("new {}",|'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL: Source/Core/ changes without test deltas"* ]]
}

@test "coverage-delta-gate.sh: an #if defined(__ANDROID__) inside a block comment is not a directive" {
    _ctx_repo '/* Usage note:' '#if defined(__ANDROID__)' '   (illustrative only)' ' */'
    _ctx_edit 's|^   (illustrative only)|   (illustrative only, see below)|'
    _wph_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"FAIL: Source/Core/ changes without test deltas"* ]]
}

@test "coverage-delta-gate.sh: comment-only edits inside an existing block comment stay exempt" {
    _ctx_repo '/** Old summary.' ' * unchanged detail' ' * old note' ' */'
    sed -i -e 's|^/\*\* Old summary.|/** New summary.|' -e 's|^ \* old note| * new note|' \
        "$FIXREPO/Source/Core/src/a.cpp"
    _wph_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"test-light exemption"* ]]
}
