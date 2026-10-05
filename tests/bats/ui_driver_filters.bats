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
# Second half — the preamble ratchet (tooling
# 2026-08-07-bucket-e-runners-need-ephemeral-home): every driver that launches
# the exe sources scripts/dev/lib/ui-test-driver.sh and calls its staleness
# guard, and every driver isolates its profile through it or carries a reasoned
# `# ui-test-home: opt-out — <reason>` line, so the hardening cannot drift back
# out one script at a time.
#
# selftest: asserts-failure — the fixture cases feed a reintroduced orphan, a
# comment-only name, a filter-less driver and preamble-less drivers and assert
# the check goes red.
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

# ===========================================================================
# Preamble ratchet: every driver uses the shared helper or says why not
# ===========================================================================
# tooling 2026-08-07-bucket-e-runners-need-ephemeral-home: the profile isolation
# drifted back out one driver at a time (7 of 30 had it). Every driver that
# launches the exe must source scripts/dev/lib/ui-test-driver.sh and call its
# staleness guard, and every driver must either isolate its profile through it
# or carry a reasoned `# ui-test-home: opt-out — <reason>` line.

# Drivers held by another session's plan-lock when the ratchet landed, so they
# could not be migrated in the same change. The list only shrinks: a listed
# driver that now complies (or no longer exists) fails the suite until removed.
RATCHET_PENDING=(test-ui-tracker-first-run-setup.sh)

# _noncomment <file> — the file minus whole-line comments.
_noncomment() { grep -vE '^[[:space:]]*#' "$1" || true; }

# driver_preamble_problems <driver> — one line per missing piece; empty = compliant.
driver_preamble_problems() {
    local d="$1" name body
    name="$(basename "$d")"
    body="$(_noncomment "$d")"
    # shellcheck disable=SC2016 # the literal text "$EXE" marks a driver that launches the exe
    if grep -qF '"$EXE"' <<<"$body"; then
        grep -qE '^[[:space:]]*(\.|source)[[:space:]].*lib/ui-test-driver\.sh' <<<"$body" \
            || echo "NO-HELPER: $name launches the exe but does not source scripts/dev/lib/ui-test-driver.sh"
        grep -qE '^[[:space:]]*ui_test_require_fresh_exe[[:space:]]' <<<"$body" \
            || echo "NO-STALE-GUARD: $name launches the exe without ui_test_require_fresh_exe"
    fi
    if ! grep -qE '^[[:space:]]*ui_test_isolate_home([[:space:]]|$)' <<<"$body" \
        && ! grep -qE '^[[:space:]]*# ui-test-home: opt-out — .{20,}' "$d"; then
        echo "NO-ISOLATION: $name neither calls ui_test_isolate_home nor carries a reasoned '# ui-test-home: opt-out — <reason>' line"
    fi
}

# check_driver_preambles <drivers-dir> [pending-basename...] — return 1 on any problem.
check_driver_preambles() {
    local dir="$1" d name p rc=0 problems
    shift
    while IFS= read -r d; do
        name="$(basename "$d")"
        for p in "$@"; do [ "$p" = "$name" ] && continue 2; done
        problems="$(driver_preamble_problems "$d")"
        if [ -n "$problems" ]; then
            echo "$problems"
            rc=1
        fi
    done < <(ui_drivers "$dir")
    return "$rc"
}

@test "every bucket-E driver sources the shared preamble and isolates its profile (or opts out with a reason)" {
    run check_driver_preambles "$DEV" "${RATCHET_PENDING[@]}"
    echo "$output"
    [ "$status" -eq 0 ]
}

@test "the ratchet's pending list only names drivers that still need migrating" {
    local p
    for p in "${RATCHET_PENDING[@]}"; do
        [ -f "$DEV/$p" ] || { echo "$p no longer exists — drop it from RATCHET_PENDING"; return 1; }
        [ -n "$(driver_preamble_problems "$DEV/$p")" ] || {
            echo "$p now complies — drop it from RATCHET_PENDING so the ratchet holds it"
            return 1
        }
    done
}

# _mk_raw_driver <path> <line>... — a fixture driver whose body is the given lines verbatim.
_mk_raw_driver() {
    local out="$1"
    shift
    { echo '#!/usr/bin/env bash'; printf '%s\n' "$@"; } > "$out"
}

# Fixture-driver lines, single-quoted on purpose: they are written verbatim.
# shellcheck disable=SC2016
L_SRC='. "$(dirname "$0")/lib/ui-test-driver.sh"'
# shellcheck disable=SC2016
L_STALE='ui_test_require_fresh_exe "$EXE" || exit 2'
# shellcheck disable=SC2016
L_RUN='RAW="$("$EXE" cmd ui_test.run)"'

@test "the preamble ratchet reds on a driver that skips the helper, the stale guard, or the isolation" {
    _mk_raw_driver "$FIX/dev/test-ui-bare.sh" "$L_RUN"
    _mk_raw_driver "$FIX/dev/test-ui-nostale.sh" "$L_SRC" 'ui_test_isolate_home --seed' "$L_RUN"
    _mk_raw_driver "$FIX/dev/test-ui-thin-reason.sh" "$L_SRC" "$L_STALE" '# ui-test-home: opt-out — tbd' "$L_RUN"
    run check_driver_preambles "$FIX/dev"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "NO-HELPER: test-ui-bare.sh" <<<"$output"
    grep -q "NO-STALE-GUARD: test-ui-bare.sh" <<<"$output"
    grep -q "NO-ISOLATION: test-ui-bare.sh" <<<"$output"
    grep -q "NO-STALE-GUARD: test-ui-nostale.sh" <<<"$output"
    [[ "$output" != *"NO-ISOLATION: test-ui-nostale.sh"* ]]
    grep -q "NO-ISOLATION: test-ui-thin-reason.sh" <<<"$output"
}

@test "the preamble ratchet passes a migrated driver, a reasoned opt-out and an exe-less wrapper" {
    _mk_raw_driver "$FIX/dev/test-ui-good.sh" "$L_SRC" "$L_STALE" 'ui_test_isolate_home' "$L_RUN"
    _mk_raw_driver "$FIX/dev/test-ui-optout.sh" "source ${L_SRC#. }" "$L_STALE" \
        '# ui-test-home: opt-out — needs the real profile for a reason spelled out here' "$L_RUN"
    _mk_raw_driver "$FIX/dev/test-ui-wrapper.sh" \
        '# ui-test-home: opt-out — launches no exe; the exec-ed driver isolates the profile' 'exec bash other.sh'
    run check_driver_preambles "$FIX/dev"
    echo "$output"
    [ "$status" -eq 0 ]
}

@test "a commented-out helper call does not satisfy the ratchet" {
    _mk_raw_driver "$FIX/dev/test-ui-commented.sh" "# $L_SRC" "# $L_STALE" '# ui_test_isolate_home' "$L_RUN"
    run check_driver_preambles "$FIX/dev"
    echo "$output"
    [ "$status" -eq 1 ]
    grep -q "NO-HELPER: test-ui-commented.sh" <<<"$output"
    grep -q "NO-STALE-GUARD: test-ui-commented.sh" <<<"$output"
    grep -q "NO-ISOLATION: test-ui-commented.sh" <<<"$output"
}
