#!/usr/bin/env bats
# tests/bats/ui_driver_filters.bats
# ----------------------------------------------------------------------------
# Build-free gate over the bucket-E drivers (scripts/dev/test-ui-*.sh).
#
# Orphan-driver class (test 2026-08-04-orphaned-bucket-e-driver): a driver whose
# default `UI_TEST_FILTER` names a suite nobody registers can only ever fail
# ("ui_test.run matched 0 tests"), and it went unnoticed because test-all.sh SKIPs
# every bucket-E driver on a tree with no built exe — the common local state. Two
# shipped that way (AgentProposalStore, Omnibar). This suite needs no build and no
# exe: for every driver it reads the default filter and asserts each term appears
# as a substring of some C++ string literal in tests/ui/*.cpp. Literals are
# searched rather than IM_REGISTER_TEST call sites because names reach the engine
# through helpers (RegisterOneVariant, per-variant tables) and calls split across
# lines; comment lines are dropped first so a name that survives only in prose
# cannot satisfy the gate.
#
# selftest: asserts-failure — the fixture cases feed a reintroduced orphan, a
# comment-only name and a filter-less driver and assert the check goes red.
#
# Requires: bash, grep, sed, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    DEV="$REPO_ROOT/scripts/dev"
    UI_TESTS="$REPO_ROOT/tests/ui"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/dev" "$FIX/ui"
}

teardown() {
    [ -n "${FIX:-}" ] && rm -rf "$FIX"
}

# ui_drivers <dir> — the bucket-E drivers under <dir>, one path per line. A bats
# wrapper that happens to match the glob (test-ui-*-bats.sh) is not a driver.
ui_drivers() {
    local f
    for f in "$1"/test-ui-*.sh; do
        [ -e "$f" ] || continue
        case "$f" in *-bats.sh) continue ;; esac
        printf '%s\n' "$f"
    done
}

# driver_default_filter <driver> — X from the first `UI_TEST_FILTER:-X}` default
# (both the `FILTER="${UI_TEST_FILTER:-X}"` and the `export UI_TEST_FILTER=...`
# wrapper shapes). Empty when the driver declares none.
driver_default_filter() {
    grep -oE 'UI_TEST_FILTER:-[^}]*\}' "$1" | sed -nE '1{s/^UI_TEST_FILTER:-//;s/\}$//;p;}'
}

# ui_test_literals <dir> — every double-quoted C++ string literal in <dir>/*.cpp,
# skipping comment lines (`//`, `/*`, ` * `, `*/`).
ui_test_literals() {
    local f
    for f in "$1"/*.cpp; do
        [ -e "$f" ] || continue
        grep -vE '^[[:space:]]*(//|/\*|\*[[:space:]]|\*$|\*/)' "$f" | grep -oE '"([^"\\]|\\.)*"' || true
    done
}

# check_driver_filters <drivers-dir> <ui-tests-dir> — print one line per driver
# whose default filter is missing or names no registered test; return 1 if any.
# The engine filter is substring-match with optional ^ / $ anchors and
# comma-separated OR terms, so each term is checked on its own, anchors stripped.
check_driver_filters() {
    local drivers_dir="$1" tests_dir="$2" lits rc=0 d filter term n=0
    local -a terms
    lits="$(ui_test_literals "$tests_dir")"
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        n=$((n + 1))
        filter="$(driver_default_filter "$d")"
        if [ -z "$filter" ]; then
            echo "NO-FILTER: $(basename "$d") declares no UI_TEST_FILTER default"
            rc=1
            continue
        fi
        IFS=',' read -r -a terms <<<"$filter"
        for term in "${terms[@]}"; do
            term="${term#^}"
            term="${term%\$}"
            [ -n "$term" ] || continue
            if ! grep -qF -- "$term" <<<"$lits"; then
                echo "ORPHAN: $(basename "$d") default filter '$term' matches no string literal in $tests_dir/*.cpp"
                rc=1
            fi
        done
    done < <(ui_drivers "$drivers_dir")
    echo "checked $n driver(s)"
    return "$rc"
}

# _mk_driver <path> <filter> — a minimal driver carrying the canonical default.
_mk_driver() {
    # shellcheck disable=SC2016 # the ${UI_TEST_FILTER:-…} text is written verbatim
    printf '#!/usr/bin/env bash\nFILTER="${UI_TEST_FILTER:-%s}"\n' "$2" > "$1"
}

# ---------------------------------------------------------------------------
# Live tree
# ---------------------------------------------------------------------------

@test "every bucket-E driver's default filter resolves to a registered ui-test name" {
    run check_driver_filters "$DEV" "$UI_TESTS"
    echo "$output"
    [ "$status" -eq 0 ]
    # Non-vacuous: a glob that silently matched nothing would pass every driver.
    local n
    n="$(sed -nE 's/^checked ([0-9]+) driver\(s\)$/\1/p' <<<"$output")"
    [ "${n:-0}" -ge 10 ]
}

@test "a reintroduced orphan driver (a real driver repointed at a dead suite) fails the gate" {
    cp "$DEV/test-ui-grid-search-apply.sh" "$FIX/dev/test-ui-omnibar-search-apply.sh"
    sed -i 's/UI_TEST_FILTER:-GridSearch}/UI_TEST_FILTER:-Omnibar}/' "$FIX/dev/test-ui-omnibar-search-apply.sh"
    grep -q 'UI_TEST_FILTER:-Omnibar}' "$FIX/dev/test-ui-omnibar-search-apply.sh"
    cp "$DEV/test-ui-window-expand.sh" "$FIX/dev/"
    run check_driver_filters "$FIX/dev" "$UI_TESTS"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "ORPHAN: test-ui-omnibar-search-apply.sh default filter 'Omnibar'" <<<"$output"
    [[ "$output" != *"test-ui-window-expand.sh"* ]]
}

# ---------------------------------------------------------------------------
# Fixture semantics
# ---------------------------------------------------------------------------

@test "a name reached through a helper call split across lines resolves" {
    cat > "$FIX/ui/split.test.cpp" <<'CPP'
static void RegisterOneVariant(ImGuiTestEngine* e, const char* cat, const char* name);
void Register(ImGuiTestEngine* engine) {
    RegisterOneVariant(engine,
                       "SplitSuite",
                       "Case_A");
}
CPP
    _mk_driver "$FIX/dev/test-ui-split.sh" "SplitSuite"
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    echo "$output"
    [ "$status" -eq 0 ]
}

@test "a name that survives only in a comment does not satisfy the gate" {
    cat > "$FIX/ui/ghost.test.cpp" <<'CPP'
// IM_REGISTER_TEST(engine, "GhostSuite", "Removed") — the TU was deleted.
/* "GhostSuite" also mentioned in a block comment
 * "GhostSuite" continuation line
 */
void Register(ImGuiTestEngine* engine) { IM_REGISTER_TEST(engine, "LiveSuite", "Case"); }
CPP
    _mk_driver "$FIX/dev/test-ui-ghost.sh" "GhostSuite"
    _mk_driver "$FIX/dev/test-ui-live.sh" "LiveSuite"
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "ORPHAN: test-ui-ghost.sh" <<<"$output"
    [[ "$output" != *"test-ui-live.sh"* ]]
}

@test "every comma-separated term is checked, with ^ / \$ anchors stripped" {
    printf 'void R(E* e) { IM_REGISTER_TEST(e, "Alpha", "x"); }\n' > "$FIX/ui/a.test.cpp"
    _mk_driver "$FIX/dev/test-ui-ok.sh" '^Alpha$'
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    [ "$status" -eq 0 ]
    _mk_driver "$FIX/dev/test-ui-half.sh" 'Alpha,Beta'
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "ORPHAN: test-ui-half.sh default filter 'Beta'" <<<"$output"
}

@test "a driver with no UI_TEST_FILTER default fails (a new shape cannot slip past)" {
    printf 'void R(E* e) { IM_REGISTER_TEST(e, "Alpha", "x"); }\n' > "$FIX/ui/a.test.cpp"
    printf '#!/usr/bin/env bash\nFILTER="Alpha"\n' > "$FIX/dev/test-ui-hardcoded.sh"
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "NO-FILTER: test-ui-hardcoded.sh" <<<"$output"
}

@test "a bats wrapper matching the test-ui-* glob is not treated as a driver" {
    printf 'void R(E* e) { IM_REGISTER_TEST(e, "Alpha", "x"); }\n' > "$FIX/ui/a.test.cpp"
    printf '#!/usr/bin/env bash\nBATS_FILE="tests/bats/x.bats"\n' > "$FIX/dev/test-ui-something-bats.sh"
    _mk_driver "$FIX/dev/test-ui-alpha.sh" "Alpha"
    run check_driver_filters "$FIX/dev" "$FIX/ui"
    echo "$output"
    [ "$status" -eq 0 ]
    grep -q "checked 1 driver(s)" <<<"$output"
}
