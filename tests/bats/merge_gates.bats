#!/usr/bin/env bats
# tests/bats/merge_gates.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/merge-gates.sh — poll_merge_gates + helpers.
#
# Stubs `gh` on PATH; reads fixtures from tests/fixtures/merge_gates_*.json;
# mutates fixtures in-flight via jq (`fixture_override` helper) — same
# dependency as the poller-under-test.
#
# Requires: bash, jq (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export FIXTURES_DIR="$REPO_ROOT/tests/fixtures"
    export SCRIPTS_DIR="$REPO_ROOT/agents/scripts/core"

    # Standard test env
    export ORCH_USER="alexkonstantonis"
    export MERGE_GATES_POLL_INTERVAL=0
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_TIMEOUT_SECONDS=3600

    # Required-context cross-check default: EMPTY (detector inert). The existing
    # fixtures use synthetic context names (build / ci/standalone / …) that do
    # NOT match the live project.config.json required_contexts, so without this
    # override every legacy pass test would newly block on `required-missing`.
    # Setting the override to "" (set-but-empty) bypasses the config read and
    # keeps the required-absent detector inert for legacy tests. The new
    # required-absent tests opt into a fixture-matching set explicitly.
    export MERGE_GATES_REQUIRED_CONTEXTS=""

    # Freshness guard OFF for the suite. The production default is "warn"
    # (gate-tooling-run-from-stale-session-branch — a human-invoked poll must never
    # print a BLOCK without a staleness caveat), but the guard's data layer is a real
    # `git fetch origin develop` + blob compare against the CHECKOUT UNDER TEST. Left
    # at the default, every one of the ~170 tests below would attempt a network fetch
    # and then legitimately WARN that the working tree differs from origin/develop —
    # which is true of any branch mid-development, including the one adding these
    # tests. That is unrelated I/O and unrelated output in every unrelated test.
    # The dedicated freshness tests drive the guard explicitly via
    # MERGE_GATES_FRESHNESS + the MERGE_GATES_FRESH_*_BLOB injection seams.
    export MERGE_GATES_FRESHNESS=off

    # Stub gh on PATH
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
# Stub gh — bats-controlled via env vars.
#   MERGE_GATES_STUB_GH_FAIL       — when set, exit 1 with the value to stderr
#   MERGE_GATES_TEST_FIXTURE       — path to fixture for `gh api graphql`
#   MERGE_GATES_STUB_READY_STDERR  — stderr for `gh pr ready`
#   MERGE_GATES_STUB_READY_EXIT    — exit code for `gh pr ready` (default 0)
#   MERGE_GATES_STUB_VIEW_ISDRAFT  — value returned for `gh pr view --json isDraft`
#                                   (default: false; "true" / "false" / "" )
#   MERGE_GATES_STUB_VIEW_EXIT     — exit code for `gh pr view` (default 0)
case "$1" in
    api)
        if [ "$2" = "graphql" ]; then
            if [ -n "${MERGE_GATES_STUB_GH_FAIL:-}" ]; then
                echo "$MERGE_GATES_STUB_GH_FAIL" >&2
                exit 1
            fi
            fixture="${MERGE_GATES_TEST_FIXTURE:?stub gh: fixture not set}"
            # Emulate `gh api graphql --jq <filter>`: real gh applies the filter
            # with its bundled jq. The test harness has standalone jq on PATH, so
            # use it to reproduce gh's --jq behaviour (raw output, comma-stream =
            # one line per result). Falls back to raw cat when no --jq passed.
            _filter=""; _prev=""
            for _a in "$@"; do
                if [ "$_prev" = "--jq" ]; then _filter="$_a"; break; fi
                _prev="$_a"
            done
            if [ -n "$_filter" ]; then
                jq -r "$_filter" "$fixture"; exit $?
            fi
            cat "$fixture"
            exit 0
        fi
        # `gh api repos/.../contents/<path>` — used by poll_merge_gates' H12
        # cr_installed probe. Default: simulate 404 (no .coderabbit.yaml/.yml
        # in the test repo). Override via env for the specific H12 + grace
        # tests:
        #   MERGE_GATES_STUB_CR_CONFIG=200       → file present
        #   MERGE_GATES_STUB_CR_CONFIG=404       → confirmed absent (default)
        #   MERGE_GATES_STUB_CR_CONFIG=401       → auth failure (other error)
        #   MERGE_GATES_STUB_CR_CONFIG=transient → network-style error
        case "$2" in
            */contents/.coderabbit.yaml|*/contents/.coderabbit.yml)
                case "${MERGE_GATES_STUB_CR_CONFIG:-404}" in
                    200) exit 0 ;;
                    404) echo "gh: Not Found (HTTP 404)" >&2; exit 1 ;;
                    401) echo "gh: HTTP 401: Bad credentials" >&2; exit 1 ;;
                    *)   echo "gh: stub transient error (no HTTP code)" >&2; exit 1 ;;
                esac
                ;;
            */actions/runs)
                # Two probes share this path; the cancelled-pending probe is
                # the one that passes a head_sha=<sha> field.
                _has_head_sha=""
                for _a in "$@"; do
                    case "$_a" in head_sha=*) _has_head_sha=1 ;; esac
                done
                if [ -n "$_has_head_sha" ]; then
                    # Cancelled-pending probe: gh api .../actions/runs -X GET
                    #   -f head_sha=<sha> -f per_page=100 --jq '... @tsv'
                    #   MERGE_GATES_STUB_HEAD_RUNS — final TSV output, one run
                    #     per line: id<TAB>status<TAB>conclusion<TAB>name
                    #     (unset/empty → no runs on head); "fail" → probe error
                    case "${MERGE_GATES_STUB_HEAD_RUNS:-}" in
                        fail) echo "gh: stub head-runs error" >&2; exit 1 ;;
                        "")   exit 0 ;;
                        *)    printf '%s\n' "$MERGE_GATES_STUB_HEAD_RUNS"; exit 0 ;;
                    esac
                fi
                # Outage probe: gh api repos/<o>/<r>/actions/runs -X GET
                #   -f "created=>=<iso>" -f per_page=1 --jq '.total_count'
                #   MERGE_GATES_STUB_RUNS_CREATED — total_count to report
                #                                   (default 1 = Actions alive)
                #                                   "fail" → probe error
                case "${MERGE_GATES_STUB_RUNS_CREATED:-1}" in
                    fail) echo "gh: stub actions/runs error" >&2; exit 1 ;;
                    *)    echo "${MERGE_GATES_STUB_RUNS_CREATED:-1}"; exit 0 ;;
                esac
                ;;
            */pulls/*)
                # Silent-BLOCKED live probe step 1: gh api repos/<o>/<r>/pulls/<n>
                #   --jq .base.ref (the PR's ACTUAL base for the protection read).
                #   MERGE_GATES_STUB_BASE_REF — base ref to emit; unset → error
                #   (forces the config fallback, so only opted-in tests take
                #   the live path).
                if [ -n "${MERGE_GATES_STUB_BASE_REF:-}" ]; then
                    echo "$MERGE_GATES_STUB_BASE_REF"; exit 0
                fi
                echo "gh: stub pulls error" >&2; exit 1
                ;;
            */branches/*/protection)
                # Step 2: protection read for the base from step 1. Appends the
                # requested path to a file so a test can assert WHICH branch
                # was queried (stdout/stderr are captured by the SUT).
                #   MERGE_GATES_STUB_PROTECTION — "true" / "false"; unset → error
                #   MERGE_GATES_STUB_PROTECTION_PATH_FILE — record "$2" here
                if [ -n "${MERGE_GATES_STUB_PROTECTION_PATH_FILE:-}" ]; then
                    echo "$2" >> "$MERGE_GATES_STUB_PROTECTION_PATH_FILE"
                fi
                if [ -n "${MERGE_GATES_STUB_PROTECTION:-}" ]; then
                    echo "$MERGE_GATES_STUB_PROTECTION"; exit 0
                fi
                echo "gh: stub protection error" >&2; exit 1
                ;;
        esac
        ;;
    pr)
        if [ "$2" = "ready" ]; then
            if [ -n "${MERGE_GATES_STUB_READY_MARKER:-}" ]; then
                # Marker file — caller's subshell may swallow our stderr, but
                # the file persists so a test can assert invocation.
                : > "$MERGE_GATES_STUB_READY_MARKER"
            fi
            if [ -n "${MERGE_GATES_STUB_READY_STDERR:-}" ]; then
                echo "$MERGE_GATES_STUB_READY_STDERR" >&2
            fi
            exit "${MERGE_GATES_STUB_READY_EXIT:-0}"
        fi
        if [ "$2" = "view" ]; then
            # gh pr view <pr> --json isDraft --jq .isDraft
            # Stub only emits the `--jq .isDraft` path — emit the raw value.
            if [ "${MERGE_GATES_STUB_VIEW_EXIT:-0}" -ne 0 ]; then
                echo "stub-gh: pr view failure" >&2
                exit "${MERGE_GATES_STUB_VIEW_EXIT}"
            fi
            echo "${MERGE_GATES_STUB_VIEW_ISDRAFT:-false}"
            exit 0
        fi
        if [ "$2" = "comment" ]; then
            # gh pr comment <pr> --body "@coderabbitai review" — the auto-nudge.
            # Append one line per invocation to a counter file so a test can
            # assert the exact call count (the poller redirects our stdout/stderr
            # to /dev/null, so a file is the only observable side-effect).
            if [ -n "${MERGE_GATES_STUB_COMMENT_COUNTER:-}" ]; then
                echo "$*" >> "$MERGE_GATES_STUB_COMMENT_COUNTER"
            fi
            exit "${MERGE_GATES_STUB_COMMENT_EXIT:-0}"
        fi
        ;;
    user)
        # `gh api user --jq .login` mock (only invoked if ORCH_USER missing).
        echo '{"login":"alexkonstantonis"}'
        exit 0
        ;;
esac
echo "stub-gh: unhandled args: $*" >&2
exit 99
STUB
    chmod +x "$STUB_BIN_DIR/gh"
    export PATH="$STUB_BIN_DIR:$PATH"

    # Source SUT
    # shellcheck source=../../agents/scripts/core/merge-gates.sh
    source "$SCRIPTS_DIR/merge-gates.sh"
}

teardown() {
    rm -rf "$STUB_BIN_DIR"
    unset MERGE_GATES_TEST_FIXTURE MERGE_GATES_STUB_GH_FAIL MERGE_GATES_STUB_READY_STDERR MERGE_GATES_STUB_READY_EXIT
    unset MERGE_GATES_STUB_VIEW_ISDRAFT MERGE_GATES_STUB_VIEW_EXIT
    unset MERGE_GATES_TEST_ANSWER MERGE_GATES_STUB_CR_CONFIG MERGE_GATES_STUB_READY_MARKER
    unset MERGE_GATES_STUB_COMMENT_COUNTER MERGE_GATES_STUB_COMMENT_EXIT
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS MERGE_GATES_STALE_REREVIEW_POLLS
    unset MERGE_GATES_NONE_NUDGE_POLLS MERGE_GATES_PRIOR_NONE_STREAK MERGE_GATES_PRIOR_NONE_HEAD
    unset MERGE_GATES_REQUIRED_CONTEXTS MERGE_GATES_CONFIG_FILE MERGE_GATES_IGNORE_MERGESTATE
    unset MERGE_GATES_FRESHNESS MERGE_GATES_FRESH_RUN_BLOB MERGE_GATES_FRESH_DEV_BLOB
    unset MERGE_GATES_OUTAGE_POLLS MERGE_GATES_STUB_RUNS_CREATED MERGE_GATES_STUB_HEAD_RUNS
    unset MERGE_GATES_PRIOR_OUTAGE_HEAD MERGE_GATES_PRIOR_OUTAGE_STREAK MERGE_GATES_PRIOR_OUTAGE_SINCE
}

# ---------- helpers ----------

fixture_override() {
    # Usage: fixture_override <fixture> <dot-path> <json-value>
    # Returns path to temp fixture with the override applied.
    # Numeric path segments are converted to array indices.
    local base="$1" path="$2" value="$3"
    local out
    out="$(mktemp)"
    jq --argjson v "$value" --arg p "$path" '
        setpath(($p | split(".") | map(if test("^[0-9]+$") then tonumber else . end)); $v)
    ' "$base" > "$out"
    echo "$out"
}

set_fixture() {
    export MERGE_GATES_TEST_FIXTURE="$1"
}

# ---------- Pass / block by gate ----------

@test "all gates pass -> return 0 + GATES_PASSED" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    # Clean (no-override) pass: GATE_SNAPSHOT carries nothing to bypass, so the
    # merge actor writes redChecks=[] and postmortem-owed never double-flags a
    # clean merge (mandatory-merge-snapshot-on-override-merge).
    [[ "$output" == *"GATE_SNAPSHOT cr_override=0 downgraded="* ]]
    # No CI check name and no cr_override=1 leaked into the line.
    [[ "$output" != *"GATE_SNAPSHOT cr_override=1"* ]]
}

@test "CI conclusion FAILURE -> return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_fail.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
}

# ---------- Gate-logic self-freshness guard (#1428) ----------
# The watcher runs merge-gates.sh from its host checkout; if that tree is parked
# behind origin/develop it enforces STALE gate logic. MERGE_GATES_FRESHNESS=block
# refuses GATES_PASSED when this script's blob != origin/develop's blob. The
# MERGE_GATES_FRESH_RUN_BLOB / _DEV_BLOB test overrides bypass git so the guard is
# exercised deterministically (no git/network dependency in the bats sandbox).

@test "freshness WARN by default -> divergent blobs warn but do NOT block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    # Unset the suite-wide off (setup()) to exercise the PRODUCTION default, which is
    # "warn" (gate-tooling-run-from-stale-session-branch, process P1): a human-invoked
    # poll out of a long-lived session tree must not be able to print a verdict without
    # the staleness caveat. Verdict is unchanged — warn never sets self_stale.
    unset MERGE_GATES_FRESHNESS
    export MERGE_GATES_FRESH_RUN_BLOB="aaaaaaaaaaaa1111"
    export MERGE_GATES_FRESH_DEV_BLOB="bbbbbbbbbbbb2222"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"differs from origin/develop"* ]]
    [[ "$output" != *"Refusing GATES_PASSED"* ]]
}

@test "freshness explicit OFF -> divergent blobs are fully inert (no warn, no block)" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FRESHNESS=off
    export MERGE_GATES_FRESH_RUN_BLOB="aaaaaaaaaaaa1111"
    export MERGE_GATES_FRESH_DEV_BLOB="bbbbbbbbbbbb2222"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" != *"differs from origin/develop"* ]]
}

@test "freshness BLOCK + stale (run != develop) -> refuses GATES_PASSED (#1428)" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FRESHNESS=block
    export MERGE_GATES_FRESH_RUN_BLOB="aaaaaaaaaaaa1111"
    export MERGE_GATES_FRESH_DEV_BLOB="bbbbbbbbbbbb2222"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    # Bare `echo GATES_PASSED` emit must be ABSENT — whole-line match so the BLOCK
    # message's own "Refusing GATES_PASSED" text doesn't false-trip this.
    ! grep -qx 'GATES_PASSED' <<<"$output"
    [[ "$output" == *"differs from origin/develop"* ]]
    [[ "$output" == *"Refusing GATES_PASSED"* ]]
}

@test "freshness BLOCK + fresh (run == develop) -> passes" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FRESHNESS=block
    export MERGE_GATES_FRESH_RUN_BLOB="cccccccccccc3333"
    export MERGE_GATES_FRESH_DEV_BLOB="cccccccccccc3333"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" != *"differs from origin/develop"* ]]
}

@test "freshness WARN + divergent blobs -> warns but still passes" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FRESHNESS=warn
    export MERGE_GATES_FRESH_RUN_BLOB="aaaaaaaaaaaa1111"
    export MERGE_GATES_FRESH_DEV_BLOB="bbbbbbbbbbbb2222"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"WARN: merge-gates.sh or a merge-gates.d/ gate module differs from origin/develop"* ]]
}

@test "freshness guard fingerprints EVERY merge-gates.d/ module (no silent escape) (#1428 split)" {
    # Regression guard for the merge-gates.d/ split: the block allow-list and the
    # GATE_FILTER moved into modules, so a stale/tampered module must invalidate
    # the freshness check too. Every *.sh under merge-gates.d/ must appear in BOTH
    # the shell guard's _fresh_relpaths AND merge-watcher.py's _GATE_LOGIC_RELPATHS,
    # or a future module would enforce out-of-date gate logic undetected.
    local core="$REPO_ROOT/agents/scripts/core"
    local mod rel missing=0
    for mod in "$core"/merge-gates.d/*.sh; do
        rel="agents/scripts/core/merge-gates.d/$(basename "$mod")"
        grep -qF "$rel" "$core/merge-gates.sh" || { echo "MISSING in merge-gates.sh _fresh_relpaths: $rel"; missing=1; }
        grep -qF "$rel" "$core/merge-watcher.py" || { echo "MISSING in merge-watcher.py _GATE_LOGIC_RELPATHS: $rel"; missing=1; }
    done
    [ "$missing" -eq 0 ]
}

@test "freshness BLOCK + unverifiable (no develop blob) -> fail-closed block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FRESHNESS=block
    export MERGE_GATES_FRESH_RUN_BLOB="aaaaaaaaaaaa1111"
    export MERGE_GATES_FRESH_DEV_BLOB=""
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    # Whole-line match — the BLOCK message contains "refusing GATES_PASSED".
    ! grep -qx 'GATES_PASSED' <<<"$output"
    [[ "$output" == *"freshness unverifiable"* ]]
}

@test "freshness INVALID value -> return 3, never passes (reject typo) (#1428 CR)" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    # A typo'd mode previously fell through the "!= off" gate into warn-only,
    # silently weakening enforcement; it must now fail loud instead.
    export MERGE_GATES_FRESHNESS=blcok
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
    ! grep -qx 'GATES_PASSED' <<<"$output"
    [[ "$output" == *"must be one of off|warn|block"* ]]
}

@test "CI StatusContext state ERROR -> return 1" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_ci_fail.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"StatusContext","context":"ci/lint","state":"ERROR","isRequired":true}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required allow-listed Coverage FAILURE blocks (gate-escape #923 fix)" {
    # A non-required check whose name matches the meant-to-block allow-list
    # (Coverage / Sanitizer / Bucket-*) must BLOCK even though it is not in
    # branch_protection.required_contexts. (The "all gates pass" test above keeps
    # the converse contract: a non-required check NOT on the allow-list still
    # passes.) Required check kept green so coverage is the sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Coverage (windows-2022 + OpenCppCoverage)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required Perf PR-fast FAILURE blocks (perf-gate-revival step 6a)" {
    # "Perf PR-fast" joined the meant-to-block allow-list once the
    # ci-windows-latest baselines landed (perf-gate-revival step 6a). A red
    # perf check must block the poller even though it is not a
    # branch-protection-required context. The perf-out-of-band downgrade
    # (covered by its own test below) remains the override hatch.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Perf PR-fast (windows-2022)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required Android security gate FAILURE blocks (mobile WS1 #1067/#1068)" {
    # "Android security gate" joined the meant-to-block allow-list (mobile-mvp-
    # completion WS1). The advisory mobile jobs do NOT catch the manifest-
    # allowBackup or OpenSSL-fail-fast regressions, so this gate is routed onto
    # the blocking path: a red "Android security gate" must block even though it
    # is not a branch-protection-required context. Required check kept green so
    # the security gate is the sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Android security gate (manifest + TLS fail-fast)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required Fuzz smoke FAILURE blocks (#1301 compile-break promotion 2026-06-16)" {
    # "Fuzz smoke" joined the meant-to-block allow-list 2026-06-16: #1301 merged
    # past a RED fuzz-smoke whose libFuzzer driver FAILED TO COMPILE (a real
    # broken develop build) because the check is non-required. A red "Fuzz smoke
    # (Linux libFuzzer)" must now block. Safe to promote because the workflow's
    # stochastic time-boxed fuzz STEP is continue-on-error on PRs, so only the
    # deterministic build/ctest reds the check. Required check kept green so the
    # fuzz check is the sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Fuzz smoke (Linux libFuzzer)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required Fuzz smoke IN_PROGRESS blocks as pending (allow-list pending parity)" {
    # Same $blocking-pending contract as the Sanitizer #1237 fix: an in-flight
    # allow-listed "Fuzz smoke" must count as pending so the poller WAITS for it
    # to go terminal instead of waving the merge through. Required check kept
    # green so the in-progress fuzz check is the sole non-terminal context.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Fuzz smoke (Linux libFuzzer)","status":"IN_PROGRESS","conclusion":null,"isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
    rm -f "$f"
}

@test "non-required Bucket-E FAILURE blocks (block-on-any-red flip)" {
    # HISTORY: `Bucket-` was dropped from the curated allow-list 2026-06-15
    # (Mesa lanes could not boot the CI exe — stochastic-flake jam) and this test
    # pinned "a red bucket-C/E must NOT block". The all-gates-blocking flip
    # inverts that contract: the lanes are genuinely green (Mesa boot fixed;
    # #1370 fixed the --spawn teardown exit-code), the blocking regex now
    # matches every name, and a red Bucket-E is a real UI-test regression that
    # must BLOCK like any other check. The only non-blocking reds left are
    # checks whose NAME carries the "advisory" token (see the advisory-name
    # escape tests). Required check kept green so the bucket failure is the
    # sole non-required red.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Bucket-E UI tests (Mesa headless GL)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "non-required Bucket launch-smoke (Mesa GL) FAILURE blocks (dead-harness graduation 2026-06-18)" {
    # The dedicated, hard-fail `Bucket launch-smoke (Mesa GL)` job graduated onto
    # the meant-to-block allow-list 2026-06-18 (infra.md `bucket-mesa-exe-boot` P1):
    # it boots the provisioned exe once under Mesa software-GL and is now reliably
    # green after #1370 fixed the `--spawn` teardown exit-code. A red launch-smoke
    # means the provisioned exe cannot boot (dead harness) and must BLOCK even
    # though it is not a branch-protection-required context. Mirrors the
    # Coverage/Sanitizer block tests above. This is the BLOCKING counterpart to the
    # Bucket-E-does-NOT-block test above: the flaky bucket lanes stay advisory; only
    # the dedicated boot check blocks. Required check kept green so the launch-smoke
    # is the sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Bucket launch-smoke (Mesa GL)","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "CI pending IN_PROGRESS -> return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_pending.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
}

@test "CI StatusContext state EXPECTED -> return 1 (pending)" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_ci_pending.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"StatusContext","context":"ci/lint","state":"EXPECTED","isRequired":true}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
    rm -f "$f"
}

@test "non-required allow-listed Sanitizer IN_PROGRESS blocks as pending (gate-escape #1237 fix)" {
    # The #923 fix made a TERMINAL non-required allow-listed FAILURE block, but
    # the pending count only ever looked at $req — so a non-required allow-listed
    # check still IN_PROGRESS (not yet terminal) was invisible: not failing (not
    # terminal) and not pending (not required) → GATES_PASSED fired and the merge
    # beat ASAN/Coverage/Bucket to the line (#1237/#1232/#1227/#1220/#1198). The
    # gate must now WAIT on an in-flight allow-listed check. Required check kept
    # green so the in-progress Sanitizer is the sole non-terminal context.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Sanitizer (ASAN via MSVC)","status":"IN_PROGRESS","conclusion":null,"isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
    rm -f "$f"
}

@test "arbitrary non-required check IN_PROGRESS blocks as pending (block-on-any-red)" {
    # The flip's core pending semantic: a non-required check whose name was NEVER
    # on the curated-era allow-list (CodeQL here) must hold GATES_PASSED while
    # in flight — every gate is a gate. Twin of the #1237 Sanitizer test above,
    # generalised from the curated names to any non-advisory name.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json"         "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes"         '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"CodeQL analyze (c-cpp)","status":"IN_PROGRESS","conclusion":null,"isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
    rm -f "$f"
}

@test "advisory-NAMED check IN_PROGRESS does NOT block (the one block-on-any-red escape)" {
    # Under block-on-any-red every non-required check blocks (pending or red) —
    # EXCEPT a check whose name literally contains "advisory": the sanctioned,
    # name-visible convention for a deliberately non-gating lane. No production
    # lane carries the token today (all-gates-blocking rename sweep); this pins
    # the escape so a future deliberately-advisory lane has a working contract.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Duplication scanner (advisory)","status":"IN_PROGRESS","conclusion":null,"isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 pending"* ]]
    rm -f "$f"
}

@test "CodeRabbit CHANGES_REQUESTED on current head -> return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_changes.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit: CHANGES_REQUESTED"* ]]
}

@test "user conversation comment -> return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_user_comment.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"User: 2"* ]]
}

# ---------- CodeRabbit identity ----------

@test "coderabbitai login (no [bot] suffix) matches" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.reviews.nodes" \
        '[{"author":{"login":"coderabbitai","__typename":"Bot"},"state":"CHANGES_REQUESTED","submittedAt":"2026-05-19T11:00:00Z","commit":{"oid":"abc123"}}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit: CHANGES_REQUESTED"* ]]
    rm -f "$f"
}

@test "coderabbitai[bot] login matches" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_changes.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit: CHANGES_REQUESTED"* ]]
}

@test "stale CodeRabbit APPROVED on old SHA -> return 1 (STALE)" {
    # Legacy fixture has no body — falls into STALE_UNKNOWN (safe-default block per
    # P1 fix in docs/self-improvement/categories/process.md).
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_UNKNOWN"* ]]
}

# ---------- CR three-bucket actionable parsing (P1 from 2026-05-21) ----------

@test "CR review on current head with 'Actionable comments posted: N>0' -> block (P1: don't merge unaddressed findings)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"COMMENTED (3 actionable"* ]]
    [[ "$output" == *"block"* ]]
}

@test "CR review on current head with 'Actionable comments posted: 0' -> pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_current_clean.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"COMMENTED (0 actionable)"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "STALE CR review with 'Actionable comments posted: N>0' -> block (P1: surface findings, don't force-merge)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_WITH_FINDINGS (5 actionable"* ]]
}

@test "STALE CR review with 'Actionable comments posted: 0' -> pass (prior review was clean)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_clean.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"STALE_CLEAN (0 actionable"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "H16: STALE CR with N>0 findings + all threads resolved + StatusContext SUCCESS -> pass (CR-driven thread resolution)" {
    # CR's "resolved threads + cleared StatusContext without re-posting a clean
    # review body" path. The old review reported N>0 actionable; CR re-evaluated
    # the current head, resolved each finding's thread (its accept signal), and
    # the per-head StatusContext fired SUCCESS. merge-gates used to wedge here
    # forever (STALE_WITH_FINDINGS). H16 treats this as STALE_RESOLVED → pass.
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_resolved.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"STALE_RESOLVED (4 actionable on prior commit, all threads resolved + status SUCCESS"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "H16: STALE CR with N>0 findings + all threads resolved but StatusContext NOT SUCCESS -> still block (need both signals)" {
    # CR's StatusContext must be SUCCESS — open=0 alone could mean the user
    # manually resolved threads without CR judgement. Without the SUCCESS
    # signal we conservatively block.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_stale_resolved.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes.1.state" \
        '"PENDING"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_WITH_FINDINGS (4 actionable"* ]]
    rm -f "$f"
}

@test "H16: STALE CR with N>0 findings + StatusContext SUCCESS but an open thread -> still block (need both signals)" {
    # Status=SUCCESS alone could be a stale placeholder. With even one open
    # CR thread, we conservatively block to surface the finding.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_stale_resolved.json" \
        "data.repository.pullRequest.reviewThreads.nodes.0.isResolved" \
        'false')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_WITH_FINDINGS (4 actionable"* ]]
    rm -f "$f"
}

@test "STALE CR with no Actionable header in body -> STALE_UNKNOWN block (safe default)" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_stale_clean.json" \
        "data.repository.pullRequest.reviews.nodes.0.body" \
        '"some completely unrelated body without the Actionable header"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_UNKNOWN"* ]]
    rm -f "$f"
}

@test "CR COMMENTED on current head with empty body -> pass (no Actionable header, conservative pass)" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_current_clean.json" \
        "data.repository.pullRequest.reviews.nodes.0.body" \
        '""')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"COMMENTED (no Actionable header)"* ]]
    rm -f "$f"
}

@test "CR body has 'Actionable comments posted: N' on line 7+ but not line 1 -> cr_actionable=-1 (per #360 first-line-only parse)" {
    # Per CodeRabbit on PR #360 — restrict parsing to first line. Body with the
    # header on a non-first line is a pathological case (e.g. nested finding
    # quoting CR's own header format). Should NOT be treated as actionable=N.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_current_clean.json" \
        "data.repository.pullRequest.reviews.nodes.0.body" \
        '"some unrelated first line\n\nLater on we mention Actionable comments posted: 99 inside a nested example"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"COMMENTED (no Actionable header)"* ]]
    rm -f "$f"
}

@test "CR APPROVED on current head with N>0 findings in body -> pass (approval trumps body)" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_findings.json" \
        "data.repository.pullRequest.reviews.nodes.0.state" \
        '"APPROVED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: APPROVED"* ]]
    rm -f "$f"
}

@test "no CodeRabbit review ever -> cr_state=NONE -> contributes to pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: NONE"* ]]
}

@test "CR installed + NONE + no SUCCESS status -> block until grace expires" {
    # CR installed (no auto-probe required — env override). Default MAX_POLLS=1 +
    # GRACE_POLLS=10 means poll 0 < 10 → cr_state_print="NONE+pending"; gates block.
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR installed + NONE + grace expired (GRACE_POLLS=0) -> pass with warn" {
    # GRACE_POLLS=0 → poll index 0 ≥ 0 immediately → fall through to pass.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+grace-expired"* ]]
    [[ "$output" == *"CodeRabbit grace window"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "C4 prong 2: CR installed + NONE + StatusContext=SUCCESS + CR inline comment on head -> pass" {
    # C4 prong 2 happy path. Status fires SUCCESS AND CR has posted at least
    # one review-thread comment whose commit.oid matches headRefOid (= CR has
    # actively reviewed THIS commit, not just emitted a placeholder).
    # Thread is resolved so cr_open doesn't independently block.
    local f
    f="$(mktemp)"
    jq '.data.repository.pullRequest.commits.nodes[0].commit.statusCheckRollup.contexts.nodes
            += [{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","isRequired":false}]
        | .data.repository.pullRequest.reviewThreads.nodes
            += [{"isResolved": true, "isOutdated": false,
                 "comments": {"pageInfo": {"hasNextPage": false},
                              "nodes": [{"author": {"login":"coderabbitai[bot]", "__typename":"Bot"},
                                         "commit": {"oid":"abc123"}}]}}]' \
        "$FIXTURES_DIR/merge_gates_pass.json" > "$f"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+status-SUCCESS+inline-evidence"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "C4 prong 2: CR installed + NONE + StatusContext=SUCCESS + NO inline CR comment -> block during grace" {
    # C4 prong 2 wait path. Status fires SUCCESS but no CR review-thread
    # comment on head — the C4 draft-PR bypass shape, or a status-only CR
    # config. Within grace window, hold.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+status-SUCCESS-waiting-for-inline"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR installed + NONE + StatusContext=SUCCESS + description 'Review skipped' (generic) -> terminal pass within grace" {
    # PR #976 regression: CR processed a docs-only PR and deliberately skipped
    # the incremental review — its "CodeRabbit" StatusContext is SUCCESS with
    # description "Review skipped" (NOT the too-many-files size-skip). That is
    # TERMINAL: no inline review will ever land. The gate must fast-pass instead
    # of sitting in the status-SUCCESS-waiting-for-inline grace (which burned
    # ~10 cycles before the passive grace-then-pass fired). Same shape as the
    # waiting-for-inline test above, only the description differs — proving the
    # description is the discriminator.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","description":"Review skipped","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+review-skipped"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR size-skip (Too many files) still BLOCKS even though description says Review skipped" {
    # Guard: the too-many-files size-skip arm must win over the new generic
    # terminal-skip pass. Here CR posts BOTH a real size-skip conversation
    # comment (structural "## Review skipped" callout + "Too many files!") AND a
    # StatusContext SUCCESS whose description contains "Review skipped". The
    # size-skip arm is ordered first, so this must still BLOCK — the terminal-skip
    # pass must never wave a 600-file reorg through.
    local f
    f="$(mktemp)"
    jq '.data.repository.pullRequest.commits.nodes[0].commit.statusCheckRollup.contexts.nodes
            += [{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","description":"Review skipped — Too many files","isRequired":false}]
        | .data.repository.pullRequest.comments.nodes
            += [{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},
                 "body":"> [!IMPORTANT]\n> ## Review skipped\n>\n> Too many files!\n>\n> This PR contains 300 files."}]' \
        "$FIXTURES_DIR/merge_gates_pass.json" > "$f"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+size-skip"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR path-filter skip comment (marker + '## Review skipped', no 'too many files') -> terminal pass, NOT size-skip" {
    # tooling 2026-08-06 cr-path-filter-skip-false-block regression: CR stamps
    # the HTML marker `skip review by coderabbit.ai` on EVERY skip comment,
    # whatever the reason. The old $crskip fired on the marker alone, routing a
    # PATH-FILTER skip into the size-skip hard-block arm — "split the PR" advice
    # on a 3-file diff. Real shape from PR #2016: the skip comment says "Review
    # was skipped due to path filters" while the CodeRabbit StatusContext
    # description reads "Review completed", so the status-description-keyed
    # fallback cannot carry the fact — the COMMENT surface must classify. Must
    # take the terminal review-skipped PASS, not the size-skip BLOCK, and not
    # burn the NONE grace. The diff is pure-docs (the comment-based arm is
    # $pureDocs-gated — see the code-diff counterpart test below).
    local f1 f
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"<!-- This is an auto-generated comment: skip review by coderabbit.ai -->\n\n> [!IMPORTANT]\n> ## Review skipped\n>\n> Review was skipped due to path filters\n>\n> Files ignored due to path filters (3)"}]')"
    f="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/notes/example.md"},{"path":"docs/self-improvement/merge-snapshots.jsonl"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+review-skipped"* ]]
    [[ "$output" != *"size-skip"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f1" "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR path-filter skip comment + CODE diff -> NOT a terminal pass (stale-comment fast-pass guarded by pureDocs)" {
    # The comment-based review-skipped arm is $pureDocs-gated: comments — unlike
    # the head-scoped StatusContext — persist across pushes, so a stale skip
    # comment from a docs-only head must not fast-pass a FRESH CODE push before
    # CR re-runs. A code diff with a genuine path-filter skip falls back to the
    # ordinary NONE grace (slower, never wedged) instead.
    local f1 f
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"<!-- This is an auto-generated comment: skip review by coderabbit.ai -->\n\n> [!IMPORTANT]\n> ## Review skipped\n>\n> Review was skipped due to path filters"}]')"
    f="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"src/core/Tracker.cpp"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=2
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [[ "$output" != *"size-skip"* ]]
    [[ "$output" != *"review-skipped"* ]]
    rm -f "$f1" "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR status description 'Review skipped: manual review required' -> NOT a terminal pass (manual trigger still possible)" {
    # CR round 2 on PR #2017: the manual-trigger state hides INSIDE a
    # "Review skipped: ..." STATUS description — observed live on PR #2014:
    # SUCCESS + "Review skipped: manual review required for this OSS
    # repository". A review CAN still be triggered, so the status arm must not
    # terminal-pass on the "Review skipped" prefix alone; the PR stays in the
    # ordinary status-SUCCESS-waiting-for-inline grace where a manual
    # `@coderabbitai review` resolves it.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","description":"Review skipped: manual review required for this OSS repository","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+status-SUCCESS-waiting-for-inline"* ]]
    [[ "$output" != *"NONE+review-skipped"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR status description 'Review skipped: rate limited' -> NOT a terminal pass (rate-limit machinery binds instead)" {
    # Same shape, rate-limit flavor: a "Review skipped: rate limited" STATUS
    # description is TEMPORARY — CR re-reviews when quota recovers. It must not
    # take the terminal review-skipped pass; the dedicated rate-limit
    # classifier picks it up (code PR, no current-head verdict -> pause/block).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","description":"Review skipped: rate limited","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=1
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"NONE+review-skipped"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR 'Review available on request' comment (marker, no '## Review skipped' heading) -> NONE grace keeps working" {
    # Manual-trigger repos (<10 stars): CR posts a "## Review available on
    # request" comment carrying the same generic skip marker. A review CAN
    # still be triggered on request, so this must be NEITHER a size-skip block
    # NOR a terminal review-skipped pass — the PR stays in the ordinary NONE
    # grace where the auto-nudge / a manual `@coderabbitai review` can still
    # resolve it.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"<!-- This is an auto-generated comment: skip review by coderabbit.ai -->\n\n> [!IMPORTANT]\n> ## Review available on request\n>\n> Reviews should be triggered manually for repositories with fewer than 10 stars."}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=2
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [[ "$output" != *"size-skip"* ]]
    [[ "$output" != *"review-skipped"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "C4 prong 2: NONE + StatusContext=SUCCESS + no inline + grace expired -> pass with no-inline-evidence WARN" {
    # Grace-expired path. Probably a status-only CR config — pass so the loop
    # never wedges. WARN names the suspicious shape so the operator can spot
    # the case where it's actually the C4 bypass + force-investigate.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"CodeRabbit","state":"SUCCESS","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+status-SUCCESS+no-inline-evidence"* ]]
    [[ "$output" == *"possible status-only config OR C4 bypass"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR completion via a CheckRun (not StatusContext) is recognized as status-SUCCESS" {
    # Regression: CodeRabbit can signal completion as a CheckRun ("CodeRabbit" /
    # "CR findings (0 actionable)") rather than a StatusContext. Field 13 must
    # normalize that CheckRun's SUCCESS conclusion to crStatusState=SUCCESS so a
    # clean PR (reviewDecision NONE) fast-passes the NONE branch instead of burning
    # the whole CR_GRACE_POLLS window. Mirrors the StatusContext grace-expired test
    # above but with the CheckRun shape (observed on PR #803).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"CheckRun","name":"CodeRabbit","conclusion":"SUCCESS","status":"COMPLETED","isRequired":false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"NONE+status-SUCCESS"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

# ---------- PR state early-exit ----------

@test "PR state=CLOSED -> return 4" {
    set_fixture "$FIXTURES_DIR/merge_gates_state.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 4 ]
    [[ "$output" == *"PR_CLOSED"* ]]
}

@test "PR state=MERGED -> return 4" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_state.json" \
        "data.repository.pullRequest.state" '"MERGED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 4 ]
    [[ "$output" == *"PR_MERGED"* ]]
    rm -f "$f"
}

# ---------- reviewDecision ----------

@test "reviewDecision=REVIEW_REQUIRED -> return 1" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviewDecision" '"REVIEW_REQUIRED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"reviewDecision: REVIEW_REQUIRED"* ]]
    rm -f "$f"
}

@test "reviewDecision=CHANGES_REQUESTED -> return 1" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviewDecision" '"CHANGES_REQUESTED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"reviewDecision: CHANGES_REQUESTED"* ]]
    rm -f "$f"
}

@test "reviewDecision=APPROVED -> contributes to pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"reviewDecision: APPROVED"* ]]
}

# ---------- Pagination overflow (returns 5) ----------

@test "contexts.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    [[ "$output" == *"PAGINATION_OVERFLOW"* ]]
    rm -f "$f"
}

@test "reviews.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.reviews.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "reviewThreads.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.reviewThreads.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "comments.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.comments.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "per-thread comments.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.reviewThreads.nodes" \
        '[{"isResolved":false,"isOutdated":false,"comments":{"pageInfo":{"hasNextPage":true},"nodes":[]}}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

# ---------- Filter edge cases (in pass fixture) ----------

@test "Bot-typename conversation comment does not block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
}

@test "outdated review thread does not block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
}

@test "self comment with case-variant login does not block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"User: 0"* ]]
}

@test "skipped / neutral / STALE conclusions do not block" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"0 fail, 0 pending"* ]]
}

# ---------- Latest-per-name dedup (rerun jobs) ----------

@test "rerun: same check name with stale FAILURE + fresh SUCCESS -> pass via latest-per-name dedup" {
    set_fixture "$FIXTURES_DIR/merge_gates_dedup_rerun_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"0 fail"* ]]
}

@test "dedup key separates CheckRun vs StatusContext with identical names (CR #398)" {
    # A CheckRun named "build" must NOT collide with a StatusContext whose
    # context is also "build" — both are required, both must be counted.
    # Composite [__typename, name] dedup key. With name-only dedup, one would
    # be silently dropped.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_dedup_rerun_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","startedAt":"2026-05-22T11:00:00Z","isRequired":true},
          {"__typename":"StatusContext","context":"build","state":"FAILURE","isRequired":true}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"2/2"* || "$output" == *"1/2"* ]]  # both required, not deduped to 1
    rm -f "$f"
}

@test "rerun: dedup picks latest by startedAt regardless of array order" {
    # Reverse order: SUCCESS first (older), FAILURE second (newer) — newer must win → still block.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_dedup_rerun_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","startedAt":"2026-05-22T11:00:00Z","isRequired":true},
          {"__typename":"CheckRun","name":"build","conclusion":"FAILURE","status":"COMPLETED","startedAt":"2026-05-22T12:00:00Z","isRequired":true}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

# ---------- Per-PR label overrides ----------

@test "tests-out-of-band label downgrades Test-delta FAILURE -> WARN, gates pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_tests_oob.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
    [[ "$output" == *"tests-out-of-band"* || "$output" == *"WARN: out-of-band"* ]]
    # GATE_SNAPSHOT names the bypassed CI check so the merge actor records it in
    # the snapshot ledger (mandatory-merge-snapshot-on-override-merge).
    [[ "$output" == *"GATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate"* ]]
}

@test "perf-out-of-band label downgrades Perf PR-fast FAILURE -> WARN, gates pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_perf_oob.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
    # GATE_SNAPSHOT names the bypassed Perf check (cr_override=0, no CR waiver).
    [[ "$output" == *"GATE_SNAPSHOT cr_override=0 downgraded=Perf PR-fast"* ]]
}

@test "non-required Intent section FAILURE blocks (pr-intent-capture-hardening #5)" {
    # "Intent section" joined the meant-to-block allow-list (ADR-0022): the
    # doc-validation Intent gate now exits non-zero on a missing/empty `## Intent`,
    # so a red "Intent section" must block the poller even though it is not a
    # branch-protection-required context. Required check kept green so Intent is the
    # sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Intent section","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "intent-out-of-band label downgrades Intent section FAILURE -> WARN, gates pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_intent_oob.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
    # GATE_SNAPSHOT names the bypassed Intent check (cr_override=0, no CR waiver).
    [[ "$output" == *"GATE_SNAPSHOT cr_override=0 downgraded=Intent section"* ]]
}

@test "non-required Plan-lock gate FAILURE blocks (plan-lock Layer C, items 5-7)" {
    # "Plan-lock gate" joined the meant-to-block allow-list: the server-side
    # cross-branch plan-lock collision net must block even though it is not a
    # branch-protection-required context. Required check kept green so the
    # Plan-lock gate is the sole failure.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Plan-lock gate","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "plan-lock-out-of-band label downgrades Plan-lock gate FAILURE -> WARN, gates pass" {
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"Plan-lock gate","status":"COMPLETED","conclusion":"FAILURE","isRequired":false}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.labels.nodes" '[{"name":"plan-lock-out-of-band"}]')"
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
    [[ "$output" == *"downgraded=Plan-lock gate"* ]]
    rm -f "$f1" "$f2"
}

@test "Plan-lock gate as a StatusContext blocks but is NOT downgradable (locks in CheckRun)" {
    # The $downgraded clause hard-codes .__typename == "CheckRun": a Plan-lock
    # gate posted as a StatusContext would block yet the plan-lock-out-of-band
    # label could NEVER downgrade it (a dead, un-overridable gate). This is why
    # plan-lock-gate.yml emits a job-name CheckRun, not a StatusContext.
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","status":"COMPLETED","conclusion":"SUCCESS","isRequired":true},{"__typename":"StatusContext","context":"Plan-lock gate","state":"FAILURE"}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.labels.nodes" '[{"name":"plan-lock-out-of-band"}]')"
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"0 warn-downgraded"* ]]
    rm -f "$f1" "$f2"
}

@test "out-of-band label does NOT silence unrelated failing checks" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_oob_other_fail_blocks.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
}

@test "no out-of-band label -> Test-delta FAILURE still blocks" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_label_tests_oob.json" \
        "data.repository.pullRequest.labels.nodes" '[]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"0 warn-downgraded"* ]]
    rm -f "$f"
}

@test "labels.pageInfo.hasNextPage=true -> return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.labels.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    [[ "$output" == *"PAGINATION_OVERFLOW"* ]]
    rm -f "$f"
}

# ---------- cr-out-of-band label (downgrades a CR block → WARN, CR gate only) ----------

@test "cr-out-of-band label downgrades CR CHANGES_REQUESTED block -> pass with WARN" {
    # merge_gates_cr_changes.json: CR CHANGES_REQUESTED on head + 2 unresolved
    # CR threads (cr_open=2). The label must waive BOTH the state verdict and
    # the open-CR-thread block; CI passes + reviewDecision APPROVED so the gate
    # passes overall.
    # PR-3: cr-out-of-band now REQUIRES a paired cr-disposition attestation, so
    # the fixture carries BOTH labels (without the disposition this would block —
    # see the dedicated WITHOUT-disposition regression below).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"},{"name":"cr-disposition:follow-up-pr"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition label downgraded CR block"* ]]
    [[ "$output" == *"CHANGES_REQUESTED"* ]]
    # GATE_SNAPSHOT marks the CR gate as load-bearingly bypassed (cr_override=1)
    # so the merge actor records "CodeRabbit" in the snapshot's redChecks
    # (mandatory-merge-snapshot-on-override-merge). No CI check downgraded here.
    [[ "$output" == *"GATE_SNAPSHOT cr_override=1 downgraded="* ]]
    rm -f "$f"
}

@test "cr-out-of-band label downgrades STALE_WITH_FINDINGS block -> pass with WARN" {
    # Covers the stale-family block path. Prior CR review had N>0 findings on an
    # old SHA (STALE_WITH_FINDINGS — normally a hard block that never passes on
    # timeout). cr-out-of-band downgrades it to WARN.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_stale_findings.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"},{"name":"cr-disposition:follow-up-pr"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition label downgraded CR block"* ]]
    [[ "$output" == *"STALE_WITH_FINDINGS"* ]]
    rm -f "$f"
}

@test "cr-out-of-band + cr-disposition on a NONE-within-grace block where CR NEVER RAN still BLOCKS (merge-pipeline-04 residual)" {
    # CR installed, NONE, within grace, and — crucially — the fixture carries NO
    # CodeRabbit context on the head (no review, no StatusContext, no CheckRun): CR
    # has NOT run. Previously cr-out-of-band + cr-disposition downgraded this to a
    # pass — the merge-pipeline-04 hole (a disposition attestation could wave through
    # a PR CodeRabbit never touched). Now the downgrade additionally requires
    # evidence CR actually ran, so the CR block STANDS.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"},{"name":"cr-disposition:acked"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    # Disable the auto-nudge so this test isolates the downgrade (no gh pr comment).
    export MERGE_GATES_STALE_REREVIEW_POLLS=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"NOT honoured"* ]]
    [[ "$output" == *"never ran"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_STALE_REREVIEW_POLLS
}

@test "cr-out-of-band + cr-disposition IS honoured once CR has run (SUCCESS StatusContext) on an otherwise-NONE block" {
    # Same NONE-within-grace shape, but now CodeRabbit HAS engaged the head: a
    # CodeRabbit StatusContext SUCCESS is present. cr_ran=true, so the disposition
    # downgrade is honoured and the gate passes — proving the residual guard gates
    # on CR-engagement evidence, not on cr_state alone.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"},{"name":"cr-disposition:acked"}]}')"
    local f2
    f2="$(fixture_override "$f" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename": "CheckRun", "name": "build", "conclusion": "SUCCESS", "status": "COMPLETED", "isRequired": true},{"__typename": "StatusContext", "context": "ci/standalone", "state": "SUCCESS", "isRequired": true},{"__typename": "StatusContext", "context": "CodeRabbit", "state": "SUCCESS", "isRequired": false}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_STALE_REREVIEW_POLLS=0
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition"* ]]
    rm -f "$f" "$f2"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_STALE_REREVIEW_POLLS
}

@test "cr-out-of-band does NOT bypass a failing CI check (CR gate only)" {
    # Add the label to a fixture whose Test-delta gate FAILS and which has NO
    # tests-out-of-band label. cr-out-of-band must NOT touch CI → still blocks.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_ci_fail.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    rm -f "$f"
}

@test "cr-out-of-band does NOT bypass a user conversation comment (CR gate only)" {
    # User comment present + cr-out-of-band label. The label waives only the CR
    # gate; the user-comment gate still blocks.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_user_comment.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"User: 2"* ]]
    rm -f "$f"
}

@test "regression: no cr-out-of-band label -> CR CHANGES_REQUESTED still blocks (no downgrade WARN)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_changes.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit: CHANGES_REQUESTED"* ]]
    [[ "$output" != *"cr-out-of-band label downgraded"* ]]
}

# ---------- PR-3 cr-out-of-band-disposition-trail (cr-disposition REQUIRED) ----------

@test "cr-out-of-band WITHOUT cr-disposition still BLOCKS (disposition required)" {
    # PR-3: a bare cr-out-of-band must NOT waive a CR block — the operator must
    # also attest a cr-disposition reason (label or PR-body marker). Here only
    # cr-out-of-band is present, so the CR CHANGES_REQUESTED block must stand.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"NOT honoured"* ]]
    [[ "$output" == *"cr-disposition"* ]]
    [[ "$output" == *"cr-out-of-band-disposition-trail"* ]]
    rm -f "$f"
}

@test "cr-out-of-band WITH cr-disposition LABEL downgrades CR block -> pass" {
    # PR-3: cr-out-of-band paired with a cr-disposition:<reason> LABEL is honoured.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"},{"name":"cr-disposition:follow-up-pr"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition label downgraded CR block"* ]]
    # The CR gate is recorded as load-bearingly bypassed in the snapshot ledger.
    [[ "$output" == *"GATE_SNAPSHOT cr_override=1"* ]]
    rm -f "$f"
}

@test "cr-out-of-band WITH cr-disposition PR-BODY marker downgrades CR block -> pass" {
    # PR-3: the disposition may be a grep-able marker line in the PR BODY instead
    # of a label. cr-out-of-band label + body marker -> honoured.
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"}]}')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.body" \
        '"Merging past CR; follow-up PR queued.\ncr-disposition: addressed-in-followup"')"
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition label downgraded CR block"* ]]
    rm -f "$f1" "$f2"
}

# ---------- CR=NONE early auto-nudge (@coderabbitai review, once per HEAD) ----------

@test "CR installed + NONE within grace + NONE_NUDGE_POLLS=1 -> posts exactly one @coderabbitai review" {
    # NONE early-nudge is OFF by default (Slice 2); opt in with NONE_NUDGE_POLLS=1
    # so a single NONE poll (streak 1 ≥ 1) fires the nudge once. Counter file
    # records each gh pr comment call; assert exactly one line.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [ -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    grep -q "@coderabbitai review" "$MERGE_GATES_STUB_COMMENT_COUNTER"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR=NONE nudge is idempotent per HEAD - 3 polls on same head still post once" {
    # Across multiple polls on the same head SHA, the shared once-per-HEAD guard
    # must keep the post count at exactly one (NONE_NUDGE_POLLS=1 to opt in).
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_MAX_POLLS=3
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_MAX_POLLS
}

@test "CR=NONE early-nudge OFF by default (NONE_NUDGE_POLLS=0) -> no post (Slice 2)" {
    # Default after Slice 2: the NONE early-nudge is disabled — .coderabbit.yaml
    # auto_review already reviews every push, so nudging was redundant. NONE still
    # blocks within grace, but no @coderabbitai review is posted.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [ ! -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR=NONE but a CodeRabbit context already present on head -> no redundant nudge" {
    # CR is mid-review (a CodeRabbit CheckRun exists, conclusion null) but hasn't
    # posted a review yet → state is NONE+pending. The early-nudge must NOT fire:
    # poking @coderabbitai review while CR is already reviewing is the "multiple
    # pokes" noise (e.g. a bot auto-commit moves the head, CR auto-reviews it, and
    # the poller used to re-poke on top). cr_context_present (field 21) gates it.
    # NONE_NUDGE_POLLS=1 so the default-off doesn't mask the context-present gate.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true},{"__typename":"StatusContext","context":"ci/standalone","state":"SUCCESS","isRequired":true},{"__typename":"CheckRun","name":"skipped-job","conclusion":"SKIPPED","status":"COMPLETED","isRequired":true},{"__typename":"CheckRun","name":"neutral-job","conclusion":"NEUTRAL","status":"COMPLETED","isRequired":true},{"__typename":"CheckRun","name":"stale-job","conclusion":"STALE","status":"COMPLETED","isRequired":true},{"__typename":"CheckRun","name":"CodeRabbit","conclusion":null,"status":"IN_PROGRESS","isRequired":false}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [ ! -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    rm -f "$f" "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR=NONE nudge: gh pr comment failure leaves guard unset -> retries each poll" {
    # When `gh pr comment` exits non-zero, nudge_coderabbit must NOT mark the
    # head as posted (guard stays unset), so a later poll on the same head
    # re-attempts. With a failing stub + 2 polls, the counter file records one
    # attempt per poll (2), the poller surfaces the retry WARN, and the run
    # still completes cleanly (exit 1, NONE+pending block).
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_MAX_POLLS=2
    export MERGE_GATES_STUB_COMMENT_EXIT=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"gh pr comment failed posting @coderabbitai review; will retry next poll"* ]]
    # Failed post → guard never set → one attempt logged per poll (no dedup).
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 2 ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_MAX_POLLS MERGE_GATES_STUB_COMMENT_EXIT
}

@test "CR not installed + NONE -> no nudge (NONE is steady state)" {
    # cr_installed=false (default 404 probe) → NONE passes, no nudge — even with
    # the early-nudge opted in, the cr_installed gate (not the default-off) wins.
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: NONE"* ]]
    [ ! -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
}

@test "NONE_NUDGE_POLLS=3 + single poll -> streak 1 < 3 -> no nudge yet (Slice 2 grace)" {
    # Opt-in but with a 3-poll grace: one NONE poll only reaches streak 1, below
    # the threshold, so auto_review still gets its window before any nudge.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=3
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [ ! -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    [[ "$output" == *"none_streak=1"* ]]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_NONE_NUDGE_POLLS
}

@test "NONE_NUDGE_POLLS=3 + PRIOR_NONE_STREAK=2 on same head -> streak 3 -> one nudge (carry round-trip)" {
    # The none_streak carries across MAX_POLLS=1 watcher cycles via
    # MERGE_GATES_PRIOR_NONE_STREAK/HEAD (mirror of the STALE carry). Seed a prior
    # streak of 2 on the fixture head (abc123); the third poll crosses the
    # threshold and fires exactly one nudge.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=3
    export MERGE_GATES_PRIOR_NONE_HEAD="abc123"
    export MERGE_GATES_PRIOR_NONE_STREAK=2
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    grep -q "@coderabbitai review" "$MERGE_GATES_STUB_COMMENT_COUNTER"
    [[ "$output" == *"none_streak=3"* ]]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_NONE_NUDGE_POLLS MERGE_GATES_PRIOR_NONE_HEAD MERGE_GATES_PRIOR_NONE_STREAK
}

@test "GATE_CARRY line carries none_head + none_streak (Slice 2 round-trip fields)" {
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=3
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"GATE_CARRY "* ]]
    [[ "$output" == *"none_head=abc123"* ]]
    [[ "$output" == *"none_streak=1"* ]]
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_NONE_NUDGE_POLLS
}

# ---------- cross-poll nudge/STALE persistence (MERGE_GATES_PRIOR_* <-> GATE_CARRY) ----------
# The watcher runs the poller with MERGE_GATES_MAX_POLLS=1 once per cycle, so the
# in-process once-per-HEAD guard + STALE streak reset every invocation. These
# assert the env round-trip (GATE_CARRY out, MERGE_GATES_PRIOR_* in) keeps the
# nudge to once-per-HEAD across cycles and lets the STALE streak accumulate.

@test "nudge guard survives MAX_POLLS=1 cycles via GATE_CARRY round-trip -> exactly one post" {
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"

    # Cycle 1: no prior → nudges once, emits GATE_CARRY carrying the head.
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    local carry head
    carry="$(printf '%s\n' "$output" | grep '^GATE_CARRY ')"
    [ -n "$carry" ]
    head="$(printf '%s' "$carry" | sed -E 's/.*nudge_head=([^ ]*).*/\1/')"
    [ -n "$head" ]

    # Cycle 2: seed the prior nudge head → guard matches → still exactly one post.
    export MERGE_GATES_PRIOR_NUDGE_HEAD="$head"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]

    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_PRIOR_NUDGE_HEAD
}

@test "nudge re-arms when MERGE_GATES_PRIOR_NUDGE_HEAD is a stale (different) head" {
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_PRIOR_NUDGE_HEAD="staleHEAD000"
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    # Prior head != fixture head (abc123) → guard misses → one fresh nudge.
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    [[ "$output" == *"GATE_CARRY nudge_head=abc123"* ]]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_PRIOR_NUDGE_HEAD
}

@test "STALE streak persists via MERGE_GATES_PRIOR_STALE_STREAK -> STALE nudge fires at threshold" {
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_STALE_REREVIEW_POLLS=3
    local stale_head
    stale_head="$(jq -r '.data.repository.pullRequest.headRefOid' "$FIXTURES_DIR/merge_gates_cr_stale_findings.json")"
    export MERGE_GATES_PRIOR_STALE_HEAD="$stale_head"
    export MERGE_GATES_PRIOR_STALE_STREAK=2
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_findings.json"
    # Seeded streak 2 + this single poll = 3 = threshold → STALE nudge fires once.
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    [[ "$output" == *"GATE_CARRY"*"stale_streak=3"* ]]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_STALE_REREVIEW_POLLS \
          MERGE_GATES_PRIOR_STALE_HEAD MERGE_GATES_PRIOR_STALE_STREAK
}

@test "GATE_CARRY is emitted on block without disturbing the Poll status line" {
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"GATE_CARRY nudge_head="* ]]
    [[ "$output" == *"Poll 1/1"* ]]
    [[ "$output" == *"CodeRabbit:"* ]]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

# ---------- CR size-skip (CR posts "Review skipped — too many files", NOT a review) ----------
# A PR over CodeRabbit's file limit gets an auto-generated PR *conversation*
# comment (not a review object). reviewDecision stays NONE + cr_state computes to
# NONE, so the passive NONE grace-then-pass backstop would otherwise wave a
# completely-unreviewed PR through. The poller detects the comment's HTML marker
# (`skip review by coderabbit.ai`) and hard-blocks unless `cr-out-of-band` is set.

@test "CR size-skip + no label -> BLOCK (not mergeable + actionable message)" {
    # CR installed, NONE, CR posted the size-skip comment. Must block regardless
    # of the NONE grace counter (GRACE_POLLS=0 forces the grace fall-through that
    # the size-skip guard must override).
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$FIXTURES_DIR/merge_gates_cr_size_skip.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+size-skip"* ]]
    [[ "$output" == *"BLOCK: CodeRabbit skipped review — too many files"* ]]
    [[ "$output" == *"cr-out-of-band"* ]]
    # The grace fall-through pass WARN must NOT have fired (skip guard short-circuits it).
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" != *"grace-expired"* ]]
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR size-skip via structural heading (## Review skipped + Too many files, no HTML marker) -> BLOCK" {
    # Robustness: even if CR drops the HTML comment marker, the structural
    # fallback (a "## Review skipped" heading together with "too many files")
    # still detects the skip. NOTE: the match is anchored to the heading, NOT a
    # loose contains() of the two phrases anywhere in the body — see the
    # prose-only regression below.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_size_skip.json" \
        "data.repository.pullRequest.comments.nodes.1.body" \
        '"> [!IMPORTANT]\n> ## Review skipped\n>\n> Too many files!\n>\n> This PR contains 300 files, which is 150 over the limit of 150.\n"')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+size-skip"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR clean summary that merely mentions 'Review skipped' + 'Too many files' as prose -> NOT size-skip" {
    # PR #980 regression: a CLEAN PR whose own diff/walkthrough discusses CR's
    # size-skip behaviour makes CR echo the phrases 'Review skipped' and 'Too
    # many files' into its summary walkthrough as PROSE — no HTML skip marker,
    # no '## Review skipped' heading. The old loose contains-AND-contains check
    # false-positived this as a size-skip and hard-blocked a clean PR. The
    # tightened detection (HTML marker OR structural heading) must NOT fire here;
    # with CI/user/reviewDecision all green this is a clean CR pass.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"<!-- This is an auto-generated comment: summarize by coderabbit.ai -->\nNo actionable comments were generated. 🎉\n\nAdds crReviewSkipped when StatusContext is SUCCESS with \"Review skipped\" in description (excluding the \"Too many files\" size-skip variant)."}]')"
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_CR_GRACE_POLLS=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" != *"size-skip"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED MERGE_GATES_CR_GRACE_POLLS
}

@test "CR size-skip + cr-out-of-band + cr-disposition PR-BODY marker -> PASS with tailored WARN" {
    # PR-3: the disposition attestation may be supplied via a PR-BODY marker
    # (`cr-disposition:<reason>`) instead of a label — exercises the body path.
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_size_skip.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"cr-out-of-band"}]}')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.body" \
        '"This reorg cannot be split.\ncr-disposition: over-CR-file-limit-acked"')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition — CR skipped review (too many files) overridden"* ]]
    rm -f "$f1" "$f2"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR size-skip -> @coderabbitai review auto-nudge is NOT posted (futile re-trigger suppressed)" {
    # The CR=NONE early-nudge (#554) must be suppressed for a size-skip — nudging
    # a PR that exceeds CR's file limit just makes CR skip again + spams the
    # thread. Counter file must have ZERO lines (the stub appends one line per
    # gh pr comment invocation).
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_cr_size_skip.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+size-skip"* ]]
    [ ! -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

@test "regression: genuine NONE (no skip comment) still auto-nudges + graces (size-skip does not steal the path)" {
    # No coderabbitai skip comment → cr_size_skipped=false → the early-nudge
    # fires exactly once and the gate blocks NONE+pending within grace, exactly
    # as #554 intended. Proves the size-skip guard is inert on genuine NONE.
    # NONE early-nudge is OFF by default since Slice 2 (#859); opt in with
    # NONE_NUDGE_POLLS=1 so this regression actually exercises the nudge path.
    export MERGE_GATES_CR_INSTALLED=true
    export MERGE_GATES_NONE_NUDGE_POLLS=1
    export MERGE_GATES_STUB_COMMENT_COUNTER="${BATS_TMPDIR:-/tmp}/cr-nudge-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [[ "$output" != *"size-skip"* ]]
    [ -f "$MERGE_GATES_STUB_COMMENT_COUNTER" ]
    [ "$(wc -l < "$MERGE_GATES_STUB_COMMENT_COUNTER")" -eq 1 ]
    rm -f "$MERGE_GATES_STUB_COMMENT_COUNTER"
    unset MERGE_GATES_CR_INSTALLED
}

@test "regression: real CR review on head wins over a stale size-skip comment (most-recent CR signal)" {
    # PR was reduced/split after CR's skip comment; CR then posted a real
    # COMMENTED review with 0 actionable on the current head. The on-head review
    # (cr_state=COMMENTED) must take precedence — the case never enters the NONE
    # branch, so the stale skip comment is ignored and the gate passes.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_size_skip.json" \
        "data.repository.pullRequest.reviews.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"state":"COMMENTED","submittedAt":"2026-05-29T12:00:00Z","commit":{"oid":"abc123"},"body":"**Actionable comments posted: 0**\n\nLGTM after the split."}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"COMMENTED (0 actionable)"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" != *"size-skip"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR size-skip but CR not installed -> NONE steady-state pass (skip guard gated on cr_installed)" {
    # Defensive: a coderabbitai skip comment with cr_installed=false (404 probe)
    # — the not-installed NONE steady-state pass still applies (no CR gate to
    # enforce). The size-skip block is gated on cr_installed=true for symmetry.
    set_fixture "$FIXTURES_DIR/merge_gates_cr_size_skip.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: NONE"* ]]
    [[ "$output" != *"size-skip"* ]]
}

# ---------- Required-context ground-truth cross-check (P1: required-absent → block) ----------
# docs/self-improvement/categories/tooling.md:328 — a branch-protection-required
# check that NEVER RAN on the head is absent from the rollup, so the isRequired-
# based ci_fail/ci_pend logic sees 0 required checks and scores a vacuous
# `CI: 0/0 pass` → GATES_PASSED, even though GitHub reports mergeStateStatus=BLOCKED
# (the #856 near-miss). The fix cross-checks branch_protection.required_contexts
# against the head rollup: a required name absent entirely blocks like a pending
# check. MERGE_GATES_REQUIRED_CONTEXTS injects the expected set per-test.

@test "required context ABSENT from head rollup -> block (NOT 0/0 GATES_PASSED) [P1]" {
    # The pass fixture's rollup has names build / ci/standalone (+ skipped/neutral/
    # stale). Declare a required context "Doc anchors + agent contract" that is NOT
    # in the rollup → it never ran on the head → must block. Pre-fix: the poller
    # ignored config entirely and passed (0 required by isRequired? no — fixture
    # marks build/ci required, but the missing config name had no node to count).
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"required-missing: Doc anchors + agent contract"* ]]
    [[ "$output" == *"1 req-missing"* ]]
}

@test "required context PRESENT + passing (SKIPPED) -> still passes (skipped-present != absent) [P1]" {
    # A docs-only PR where a required Windows-build check is SKIPPED (path-filtered)
    # must still pass: SKIPPED is a passing terminal state AND the context is
    # PRESENT in the rollup (not absent). The fixture's "skipped-job" (SKIPPED,
    # required) stands in for the path-filtered required check. Declaring it as a
    # required context must NOT block — only TRULY-ABSENT names block.
    export MERGE_GATES_REQUIRED_CONTEXTS="skipped-job"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"0 req-missing"* ]]
    [[ "$output" != *"required-missing"* ]]
}

@test "all required-config contexts present + passing -> passes (multi-name set) [P1]" {
    # Both declared required names exist in the rollup in passing states
    # (build=SUCCESS CheckRun, ci/standalone=SUCCESS StatusContext) → no absent →
    # pass. Proves the cross-check is satisfied by present+passing contexts.
    export MERGE_GATES_REQUIRED_CONTEXTS="build,ci/standalone"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"0 req-missing"* ]]
}

@test "required-config name present-but-FAILING is caught by ci_fail, not double-counted as absent [P1]" {
    # ci_fail fixture: required CheckRun "build" is FAILURE (present, failing).
    # Declaring "build" as a required context must NOT inflate req-missing — the
    # context IS present, just failing, so ci_fail owns the block. req-missing=0.
    export MERGE_GATES_REQUIRED_CONTEXTS="build"
    set_fixture "$FIXTURES_DIR/merge_gates_ci_fail.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"0 req-missing"* ]]
    [[ "$output" != *"required-missing"* ]]
}

@test "config name with UTF-8 em-dash present in rollup -> no false absent (UTF-8-safe match) [P1]" {
    # The real config name "Windows + MSVC (Smatchet light — AI/Whisper/MCP off)"
    # carries a UTF-8 em-dash. Add a matching SUCCESS context to the rollup and
    # declare it required — the em-dash must round-trip through the JSON-array
    # splice + jq match so it is NOT flagged absent. (Guards the cp1252 mojibake
    # trap the task called out.)
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '[{"__typename":"CheckRun","name":"Windows + MSVC (Smatchet light — AI/Whisper/MCP off)","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true}]')"
    export MERGE_GATES_REQUIRED_CONTEXTS="Windows + MSVC (Smatchet light — AI/Whisper/MCP off)"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"0 req-missing"* ]]
    rm -f "$f"
}

@test "MERGE_GATES_REQUIRED_CONTEXTS empty -> detector inert (legacy fixtures unaffected) [P1]" {
    # The setup() default ("") keeps the detector inert so the 98 legacy
    # synthetic-name fixtures don't newly block. Explicit re-assertion.
    export MERGE_GATES_REQUIRED_CONTEXTS=""
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"0 req-missing"* ]]
}

@test "required-context read falls back to project.config.json when override unset [P1]" {
    # Unset the override so the poller reads branch_protection.required_contexts
    # from a config file (MERGE_GATES_CONFIG_FILE points at a crafted minimal
    # config). Declare a required name NOT in the pass fixture's rollup → block.
    # Proves the file-read path (the production default) works UTF-8-safe via jq.
    local cfg
    cfg="$(mktemp)"
    cat > "$cfg" <<'CFG'
{
  "branch_protection": {
    "required_contexts": ["Doc anchors + agent contract", "Windows + MSVC (Smatchet light — AI/Whisper/MCP off)"]
  }
}
CFG
    unset MERGE_GATES_REQUIRED_CONTEXTS
    export MERGE_GATES_CONFIG_FILE="$cfg"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"required-missing: Doc anchors + agent contract"* ]]
    [[ "$output" == *"Windows + MSVC (Smatchet light — AI/Whisper/MCP off)"* ]]
    rm -f "$cfg"
}

# ---------- mergeStateStatus guard (P1 secondary: refuse GATES_PASSED on BLOCKED/BEHIND) ----------

@test "mergeStateStatus=BLOCKED -> block even when all gates green [P1]" {
    # All gates green (pass fixture) but GitHub reports BLOCKED → must NOT pass.
    # This is the direct backstop for the absent-required false-pass when the
    # config cross-check is inert (the setup() default leaves it inert here).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"mergeStateStatus=BLOCKED"* ]]
    rm -f "$f"
}

@test "mergeStateStatus=BLOCKED + MERGE_GATES_IGNORE_MERGESTATE=true -> passes (admin-merge escape) [P1]" {
    # The documented admin-merge escape for a positively-confirmed STALE-BLOCKED
    # PR that is actually all-green (AGENTS.md § Merge gates). Override flips the
    # mergeStateStatus guard off so the all-green gates pass.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    export MERGE_GATES_IGNORE_MERGESTATE=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
    unset MERGE_GATES_IGNORE_MERGESTATE
}

@test "mergeStateStatus=BEHIND -> block (strict branch protection, head behind base) [P1]" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BEHIND"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"mergeStateStatus=BEHIND"* ]]
    rm -f "$f"
}

@test "mergeStateStatus=UNSTABLE -> STILL passes when all required green (we merge on UNSTABLE) [P1 regression]" {
    # CRITICAL regression guard: UNSTABLE (non-required checks pending/failing) is
    # NOT in {BLOCKED,BEHIND} and must NOT be caught. #874/#876 merged on UNSTABLE.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"UNSTABLE"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "mergeStateStatus=CLEAN -> passes (the normal all-green case) [P1 regression]" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"CLEAN"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "mergeStateStatus=BLOCKED caused solely by required-absent -> both signals block, named [P1]" {
    # Combined real-world #856 shape: a required context absent from the head AND
    # GitHub reports BLOCKED. Both detectors fire; the poller names the missing
    # required context and refuses to pass.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"required-missing: Doc anchors + agent contract"* ]]
    rm -f "$f"
}

# ---------- required-missing cause attribution (process P1: DIRTY vs "never ran") ----------
# A CONFLICTED head produces the same all-contexts-absent shape as a CI miss —
# GitHub declines to build a DIRTY head at all — so the generic "never ran" hint
# sent one session through a full 90-poll timeout before the real cause (a
# conflict in docs/plans/INDEX.md) was found by hand. mergeStateStatus is already
# in the poller's GraphQL response, so the actionable cause is available on poll 1.

@test "required-missing on a DIRTY head names the CONFLICT, not the generic never-ran hint [P1]" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"DIRTY"')"
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"required-missing: Doc anchors + agent contract"* ]]
    [[ "$output" == *"CONFLICTED"* ]]
    [[ "$output" == *"mergeStateStatus=DIRTY"* ]]
    # The misleading hint must NOT appear for this cause.
    [[ "$output" != *"GITHUB_TOKEN bot push"* ]]
    rm -f "$f"
}

@test "required-missing on a NON-dirty head keeps the never-ran hint + lag caveat [P1]" {
    # Negative canary for the branch above: BLOCKED (not DIRTY) is the ordinary
    # absent-required shape and must keep the original diagnosis, now carrying the
    # creation-lag caveat so an early-poll absence is not read as permanent.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"required-missing: Doc anchors + agent contract"* ]]
    [[ "$output" == *"GITHUB_TOKEN bot push"* ]]
    [[ "$output" == *"LAGS"* ]]
    [[ "$output" != *"CONFLICTED"* ]]
    rm -f "$f"
}

@test "a DIRTY head with NO absent required context blocks on mergeStateStatus, no conflict hint [P1]" {
    # Two assertions in one shape. (a) The required-missing attribution rides on the
    # required-missing block, so it stays silent when nothing is absent — no spurious
    # required-missing/CONFLICTED line. (b) The head is STILL blocked, by the
    # mergeStateStatus guard: DIRTY joined BLOCKED|BEHIND (CR #1996) because a
    # conflicted head is unmergeable by construction, so an all-green DIRTY head
    # reaching GATES_PASSED was a false pass that only surfaced as a failed REST
    # merge later.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"DIRTY"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    ! grep -qx 'GATES_PASSED' <<<"$output"
    [[ "$output" == *"mergeStateStatus=DIRTY"* ]]
    [[ "$output" != *"required-missing"* ]]
    [[ "$output" != *"CONFLICTED"* ]]
    rm -f "$f"
}

@test "mergeStateStatus=DIRTY + MERGE_GATES_IGNORE_MERGESTATE=true -> escape still works [P1]" {
    # The documented escape must still cover the newly-blocked DIRTY state, so a
    # positively-confirmed stale-DIRTY PR is not wedged with no way out.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"DIRTY"')"
    export MERGE_GATES_IGNORE_MERGESTATE=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    unset MERGE_GATES_IGNORE_MERGESTATE
    rm -f "$f"
}

# ---------- Silent-BLOCKED cause naming (tooling P2: bot threads vs required_conversation_resolution) ----------
# The user-comment gate excludes bot threads, but branch protection's
# required_conversation_resolution counts EVERY unresolved thread — so the
# poller could read all-clear while GitHub held the merge on ten open CR
# threads (#1937, ~90 min burned + a manual GraphQL sweep). When BLOCKED is
# the only blocker and unfiltered threads are open, the poller now names them.
# MERGE_GATES_CONV_RES_REQUIRED is the test seam for the branch-protection read
# (mirrors pr-blocked-why.sh's PR_BLOCKED_WHY_CONV_RES_REQUIRED).

# blocked_with_bot_threads <n> — pass fixture, mergeStateStatus=BLOCKED, plus
# <n> unresolved non-outdated threads authored by a non-CR non-cursor bot (CR /
# cursor authorship would trip the cr_open / bb_open gates and mask the path
# under test). Echoes the fixture path.
blocked_with_bot_threads() {
    local n="$1" nodes="[]" i
    for ((i=0; i<n; i++)); do
        nodes=$(jq -c '. + [{"isResolved": false, "isOutdated": false,
            "comments": {"pageInfo": {"hasNextPage": false},
                         "nodes": [{"author": {"login": "github-actions[bot]", "__typename": "Bot"}}]}}]' <<<"$nodes")
    done
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    local f2
    f2="$(fixture_override "$f" "data.repository.pullRequest.reviewThreads.nodes" "$nodes")"
    rm -f "$f"
    echo "$f2"
}

@test "silent-BLOCKED: bot threads + conv-res required -> actionable cause named [P2]" {
    local f; f="$(blocked_with_bot_threads 2)"
    export MERGE_GATES_CONV_RES_REQUIRED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"2 unresolved review thread(s) (0 user, 2 bot)"* ]]
    [[ "$output" == *"waives the poller's CR gate only, never branch protection"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f"
}

@test "silent-BLOCKED: conv-res disabled -> no thread-cause line (generic BLOCK only) [P2]" {
    local f; f="$(blocked_with_bot_threads 2)"
    export MERGE_GATES_CONV_RES_REQUIRED=false
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"mergeStateStatus=BLOCKED"* ]]
    [[ "$output" != *"unresolved review thread(s)"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f"
}

@test "silent-BLOCKED: protection probe unknown -> hedged 'may require' still names the threads [P2]" {
    local f; f="$(blocked_with_bot_threads 1)"
    export MERGE_GATES_CONV_RES_REQUIRED=unknown
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 unresolved review thread(s) (0 user, 1 bot)"* ]]
    [[ "$output" == *"may require (protection probe failed)"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f"
}

@test "silent-BLOCKED: zero open threads -> no thread-cause line (negative canary) [P2]" {
    # The pass fixture's only unresolved thread is OUTDATED, so the unfiltered
    # count is 0 — BLOCKED alone must not fabricate a thread cause.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    export MERGE_GATES_CONV_RES_REQUIRED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"mergeStateStatus=BLOCKED"* ]]
    [[ "$output" != *"unresolved review thread(s)"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f"
}

@test "silent-BLOCKED: another gate red -> no thread-cause line (not the ONLY blocker) [P2]" {
    # A real user comment blocks the user gate; the thread-cause naming must
    # stay silent so it never distracts from an actual red gate.
    local f; f="$(blocked_with_bot_threads 2)"
    local f2
    f2="$(fixture_override "$f" "data.repository.pullRequest.comments.nodes" \
        '[{"author": {"login": "somehuman", "__typename": "User"}}]')"
    export MERGE_GATES_CONV_RES_REQUIRED=true
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"all other gates green"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f" "$f2"
}

@test "silent-BLOCKED: live probe (no test seam) reads the PR's ACTUAL base branch protection [P2]" {
    # Every other silent-BLOCKED test drives the MERGE_GATES_CONV_RES_REQUIRED
    # seam, so the real read path — pulls/<n> .base.ref, then THAT branch's
    # protection — was unexercised. This is the code that fixes judging a PR
    # by the config-named branch's setting; assert the probe queries the base
    # the pulls read returned, not "develop".
    local f; f="$(blocked_with_bot_threads 2)"
    set_fixture "$f"
    export MERGE_GATES_STUB_BASE_REF="release-1.2"
    export MERGE_GATES_STUB_PROTECTION="true"
    export MERGE_GATES_STUB_PROTECTION_PATH_FILE="$BATS_TEST_TMPDIR/protection-path"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"2 unresolved review thread(s) (0 user, 2 bot)"* ]]
    [[ "$output" == *"requires conversation resolution"* ]]
    grep -q "branches/release-1.2/protection" "$MERGE_GATES_STUB_PROTECTION_PATH_FILE"
    unset MERGE_GATES_STUB_BASE_REF MERGE_GATES_STUB_PROTECTION MERGE_GATES_STUB_PROTECTION_PATH_FILE
    rm -f "$f"
}

@test "silent-BLOCKED: user-thread parse miss (-1) withholds the user/bot split, keeps the total [P2]" {
    # A field-32 miss surfaces as thr_user_cnt=-1; defaulting it to 0 would
    # claim every thread is bot-authored — an invented count. The cause line
    # must keep the total and drop the breakdown.
    local f; f="$(blocked_with_bot_threads 2)"
    export MERGE_GATES_CONV_RES_REQUIRED=true
    export MERGE_GATES_TEST_THR_USER_CNT="-1"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"2 unresolved review thread(s) and branch protection requires"* ]]
    [[ "$output" != *"user,"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED MERGE_GATES_TEST_THR_USER_CNT
    rm -f "$f"
}

@test "silent-BLOCKED: a USER-authored open thread blocks the user gate, so naming stays silent [P2]" {
    # A user thread is never the SILENT cause — it reds the user-comment gate
    # (field 15 counts user-authored threads), which already names itself. The
    # thread-cause line is reserved for the invisible case: bot-only threads.
    local f nodes
    nodes='[{"isResolved": false, "isOutdated": false, "comments": {"pageInfo": {"hasNextPage": false}, "nodes": [{"author": {"login": "somehuman", "__typename": "User"}}]}}]'
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"BLOCKED"')"
    local f2
    f2="$(fixture_override "$f" "data.repository.pullRequest.reviewThreads.nodes" "$nodes")"
    set_fixture "$f2"
    export MERGE_GATES_CONV_RES_REQUIRED=true
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"all other gates green"* ]]
    unset MERGE_GATES_CONV_RES_REQUIRED
    rm -f "$f" "$f2"
}

# ---------- Actions-outage escalation (process P1 fix (3): CI unavailable gets a defined move) ----------
# The #1941 shape: Actions jammed repo-wide (75 runs stuck queued, zero runs
# created after 19:16Z, close/reopen produced nothing), so the head could never
# go green and the operator's options collapsed to "wait forever" or "--admin
# override". The poller now supplies the third option: after OUTAGE_POLLS
# consecutive unexplained required-absent polls it probes runs CREATED repo-wide
# since polling began; zero created → return 7 with the outage diagnosis instead
# of timing out at MAX_POLLS with a per-check message. The probe (not the
# threshold) separates outage from the ~27 min creation lag: backlogged-but-
# alive Actions still creates runs, so the count stays >0 and polling continues.

@test "outage: required-absent + zero runs created repo-wide -> return 7 + ESCALATE diagnosis [P1]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 7 ]
    [[ "$output" == *"ESCALATE: ACTIONS UNAVAILABLE"* ]]
    [[ "$output" == *"zero workflow runs created repo-wide"* ]]
    [[ "$output" == *"Do NOT --admin merge"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
}

@test "outage: Actions alive (runs being created) -> normal required-missing block, no escalation [P1]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=5
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"required-missing"* ]]
    [[ "$output" != *"ESCALATE"* ]]
}

@test "outage: probe failure -> keep polling with a WARN, never escalate on unverified evidence [P1]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=fail
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"outage probe failed"* ]]
    [[ "$output" != *"ESCALATE"* ]]
}

@test "outage: threshold not reached -> no probe verdict, normal block [P1]" {
    # Default OUTAGE_POLLS (15) with a single poll: the streak never reaches the
    # threshold, so even a zero-count stub cannot escalate.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"ESCALATE"* ]]
}

@test "outage: streak accumulates across polls and escalates at the threshold [P1]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=3
    export MERGE_GATES_OUTAGE_POLLS=3
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 7 ]
    [[ "$output" == *"3 consecutive polls"* ]]
}

@test "outage: a DIRTY head never escalates - the conflict fully explains the absence [P1]" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.mergeStateStatus" '"DIRTY"')"
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CONFLICTED"* ]]
    [[ "$output" != *"ESCALATE"* ]]
    rm -f "$f"
}

# ── cancelled-while-pending: required-missing disambiguation ────────────────
# A run GitHub cancels while still PENDING (one-pending-per-concurrency-group
# collapse) creates NO check-run, so the required context is absent forever and
# nothing re-triggers it — #1937 burned all 90 polls on that head. Once every
# other check is terminal-green, a cancelled run on the head with zero
# queued/in-progress runs is positive "never, not late" evidence: the poller
# names the run id + `gh run rerun <id>` and exits 8 instead of polling out.
# (required-check-cancelled-while-pending-wedges-poller, tooling P2.)

@test "cancelled-pending: absent required + terminal-green CI + name-matched cancelled run, none active -> return 8 + rerun advice [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_HEAD_RUNS="$(printf '31795783743\tcompleted\tcancelled\tDoc anchors + agent contract')"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 8 ]
    [[ "$output" == *"required-missing-cancelled"* ]]
    [[ "$output" == *"gh run rerun 31795783743"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
}

@test "cancelled-pending: an unrelated cancelled run (workflow name does not match the absent context) WARNs and keeps polling [P2]" {
    # A superseded comment-trigger burst leaves cancelled runs of OTHER
    # workflows while the required context is merely creation-lagged: rerun
    # advice on those cannot create the missing context, so no early exit —
    # but the candidates are surfaced for the name-mismatch wedge case.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_HEAD_RUNS="$(printf '111\tcompleted\tcancelled\tCR finding gate')"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
    [[ "$output" == *"none name-match"* ]]
    [[ "$output" == *"gh run rerun 111"* ]]
}

@test "cancelled-pending: substring workflow name (MSVC vs 'Windows + MSVC') does NOT take the exit - exact membership only [P2]" {
    # index()-style matching would let a cancelled run named "MSVC" match the
    # absent context "Windows + MSVC" and stop polling with rerun advice for
    # the wrong workflow (CodeRabbit round on this slice).
    export MERGE_GATES_REQUIRED_CONTEXTS="Windows + MSVC"
    export MERGE_GATES_STUB_HEAD_RUNS="$(printf '444\tcompleted\tcancelled\tMSVC')"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
    [[ "$output" == *"none name-match"* ]]
}

@test "cancelled-pending: a FULL first page (100 rows) is unverifiable evidence - keep polling [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    local rows i
    rows="$(printf '1\tcompleted\tcancelled\tDoc anchors + agent contract')"
    for i in $(seq 2 100); do
        rows="$rows$(printf '\n%s\tcompleted\tsuccess\tOther workflow' "$i")"
    done
    export MERGE_GATES_STUB_HEAD_RUNS="$rows"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
}

@test "cancelled-pending: a queued run on the head suppresses the early exit (something may still arrive) [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_HEAD_RUNS="$(printf '111\tcompleted\tcancelled\tDoc anchors + agent contract\n222\tqueued\t\tDoc anchors + agent contract')"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
}

@test "cancelled-pending: no runs on the head (pure creation-lag shape) keeps polling [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
}

@test "cancelled-pending: a probe failure keeps polling (early exit never taken on unverified evidence) [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_HEAD_RUNS=fail
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
}

@test "cancelled-pending: completed-success runs alone (no cancelled) keep polling [P2]" {
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_STUB_HEAD_RUNS="$(printf '333\tcompleted\tsuccess\tSome other workflow')"
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"required-missing-cancelled"* ]]
}

@test "cancelled-pending: outage tests' shape (no head-runs stub) still reaches the outage escalation [P2]" {
    # Regression guard on ORDERING: the cancelled probe runs before the outage
    # streak; with no cancelled evidence it must fall through so exit 7 still
    # fires exactly as before this probe existed.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 7 ]
    [[ "$output" == *"ESCALATE: ACTIONS UNAVAILABLE"* ]]
}

@test "outage: MERGE_GATES_OUTAGE_POLLS=0 is REJECTED (return 3) - no silent disable of the no-skip exit [P1]" {
    # 0 used to disable the escalation entirely; that let an env var
    # downgrade the Actions-unavailable state to rc 1/2, where the halt
    # prompt offers "Skip gates and merge anyway" — defeating exit 7's
    # no-skip design. Same fail-closed posture as MERGE_GATES_FRESHNESS:
    # a weakening value never passes.
    export MERGE_GATES_OUTAGE_POLLS=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
    [[ "$output" == *"MERGE_GATES_OUTAGE_POLLS must be a positive integer"* ]]
}

@test "outage: non-integer MERGE_GATES_OUTAGE_POLLS is rejected (return 3) [P1]" {
    export MERGE_GATES_OUTAGE_POLLS=soon
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
    [[ "$output" == *"MERGE_GATES_OUTAGE_POLLS must be a positive integer"* ]]
}

@test "outage: GATE_CARRY emits the outage streak + window anchor for the watcher [P1]" {
    # The watcher runs MERGE_GATES_MAX_POLLS=1 — without these fields on
    # GATE_CARRY the streak restarts every cycle and exit 7 can never fire.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_OUTAGE_POLLS=15
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"outage_head=abc123"* ]]
    [[ "$output" == *"outage_streak=1"* ]]
    [[ "$output" == *"outage_since=2"* ]]   # ISO year prefix — anchor was stamped
}

@test "outage: a PRIOR_-seeded streak reaches the threshold in a single-poll cycle [P1]" {
    # Watcher model: cycle N-1 carried streak=2; this cycle is the third
    # consecutive absent poll on the same head -> probe fires and escalates.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_OUTAGE_POLLS=3
    export MERGE_GATES_PRIOR_OUTAGE_HEAD=abc123
    export MERGE_GATES_PRIOR_OUTAGE_STREAK=2
    export MERGE_GATES_PRIOR_OUTAGE_SINCE=2026-08-13T10:00:00Z
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 7 ]
    [[ "$output" == *"3 consecutive polls"* ]]
    [[ "$output" == *"since 2026-08-13T10:00:00Z"* ]]
}

@test "outage: a carried streak from a DIFFERENT head restarts at 1 [P1]" {
    # A new push restarts CI's run-creation clock — a stale carried streak
    # must not count against the new head.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_OUTAGE_POLLS=3
    export MERGE_GATES_PRIOR_OUTAGE_HEAD=deadbeef
    export MERGE_GATES_PRIOR_OUTAGE_STREAK=99
    export MERGE_GATES_STUB_RUNS_CREATED=0
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"ESCALATE"* ]]
    [[ "$output" == *"outage_head=abc123"* ]]
    [[ "$output" == *"outage_streak=1"* ]]
}

@test "outage: a confirmed-alive probe resets the streak and re-anchors the window [P1]" {
    # Without the reset the probe re-fires every remaining poll, and a single
    # early run permanently masks a jam that begins later (fixed-window bug).
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=5
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"outage_streak=0"* ]]
    [[ "$output" != *"outage_since=2"* ]]   # anchor cleared (ISO year would follow otherwise)
}

@test "outage: a non-numeric probe response (jq null) is unverified, never alive [P1]" {
    # A 200 with an unexpected body yields the string "null" (gh exits 0):
    # neither the =0 escalate branch nor the -z WARN matched, so it silently
    # counted as evidence of life. It must route to the WARN branch.
    export MERGE_GATES_REQUIRED_CONTEXTS="Doc anchors + agent contract"
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_OUTAGE_POLLS=1
    export MERGE_GATES_STUB_RUNS_CREATED=null
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"outage probe failed"* ]]
    [[ "$output" != *"ESCALATE"* ]]
    # and it does NOT reset the streak (unverified evidence carries forward)
    [[ "$output" == *"outage_streak=1"* ]]
}

# ---------- gh_pr_ready_idempotent ----------

@test "gh_pr_ready_idempotent: already-ready (not in draft state) returns 0" {
    export MERGE_GATES_STUB_READY_STDERR="error: Pull request #1 is not in draft state"
    export MERGE_GATES_STUB_READY_EXIT=1
    run gh_pr_ready_idempotent 1
    [ "$status" -eq 0 ]
}

@test "gh_pr_ready_idempotent: unknown error + PR still draft returns 6" {
    # Genuine failure: gh pr ready can't promote AND isDraft=true confirms
    # the PR is still draft. Positive-check fallback agrees with the failure.
    export MERGE_GATES_STUB_READY_STDERR="error: 403 forbidden"
    export MERGE_GATES_STUB_READY_EXIT=1
    export MERGE_GATES_STUB_VIEW_ISDRAFT=true
    run gh_pr_ready_idempotent 1
    [ "$status" -eq 6 ]
}

@test "H2: gh_pr_ready_idempotent: unknown error + PR observably non-draft returns 0 (positive-check fallback)" {
    # The PR is already ready (isDraft=false) even though `gh pr ready` failed
    # with an unrecognised English message. The positive-check fallback agrees
    # with the desired end-state (PR is non-draft) — return 0.
    # This is the case the English-string match misses on `gh` wording changes
    # or locale-overridden CLIs.
    export MERGE_GATES_STUB_READY_STDERR="erreur : le PR n'est plus en brouillon"
    export MERGE_GATES_STUB_READY_EXIT=1
    export MERGE_GATES_STUB_VIEW_ISDRAFT=false
    run gh_pr_ready_idempotent 1
    [ "$status" -eq 0 ]
}

@test "H2: gh_pr_ready_idempotent: gh pr ready unknown error + gh pr view also fails returns 6" {
    # Fallback API also fails (gh fully broken or PR truly inaccessible).
    # Surface as exit 6 — caller halts auto-merge.
    export MERGE_GATES_STUB_READY_STDERR="error: 500 internal server error"
    export MERGE_GATES_STUB_READY_EXIT=1
    export MERGE_GATES_STUB_VIEW_EXIT=1
    run gh_pr_ready_idempotent 1
    [ "$status" -eq 6 ]
}

@test "gh_pr_ready_idempotent: success returns 0" {
    export MERGE_GATES_STUB_READY_EXIT=0
    run gh_pr_ready_idempotent 1
    [ "$status" -eq 0 ]
}

# ---------- Infra: gh failure / timeout / MAX_POLLS ----------

@test "3 consecutive gh failures -> return 3" {
    export MERGE_GATES_MAX_POLLS=3
    export MERGE_GATES_STUB_GH_FAIL="network error"
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
    [[ "$output" == *"GH_API_DOWN"* ]]
}

@test "MAX_POLLS=1 with blocking fixture -> return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_fail.json"
    export MERGE_GATES_MAX_POLLS=1
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
}

@test "ORCH_USER unset -> return 3" {
    unset ORCH_USER
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
}

# ---------- C2 (fail-closed on jq error) ----------
# docs/evaluation/agentic-infrastructure-2026-05-23.md § C2: jq failures
# previously left integer counters empty; `[ "" -eq 0 ]` errored out and the
# iteration kept polling, but a more sinister shape (jq parsing fine, returning
# unexpected output) could silently feed 0 into the pass-check. Defensive
# defaults of -1 on every jq-derived integer fail the pass-check explicitly.

@test "C2 fail-closed: malformed contexts.nodes (string not array) -> ci_* default -1 -> block" {
    # Break contexts.nodes shape so the ctx jq's `map(...)` crashes. Without
    # the defensive `|| echo -1`, the integer assignments would become empty
    # and the `[ "$ci_fail" -eq 0 ]` check would error-out into "not pass",
    # which gets the right outcome by accident. With the defensive defaults,
    # the failure is explicit: ci_fail=-1, the pass-check is structurally
    # false, the gate blocks for the right reason.
    local fixture
    fixture=$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.nodes" \
        '"not-an-array"')
    set_fixture "$fixture"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
}

@test "C2 fail-closed: missing reviewThreads -> cr_open / user default -1 -> block" {
    # Replace reviewThreads with a non-object so the jq queries against it
    # crash. The overflow check fires first (reviewThreads.pageInfo.hasNextPage
    # jq fails → defensive "true" → PAGINATION_OVERFLOW → return 5) before
    # cr_open / user can contribute. Either way the gate blocks (non-zero exit);
    # assert [ status -ne 0 ] rather than a specific code.
    local fixture
    fixture=$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviewThreads" \
        '"not-an-object"')
    set_fixture "$fixture"
    run poll_merge_gates org repo 1
    [ "$status" -ne 0 ]
}

# ---------- H1 (APPROVED → pass unconditionally even with open cr_open) ----------
# docs/evaluation/agentic-infrastructure-2026-05-23.md § H1: AGENTS.md spec
# says "APPROVED → pass unconditionally (approval trumps body)" but the
# pass-check unconditionally required cr_open==0, so an APPROVED review on
# the current head + any unresolved non-outdated CR thread (even one CR
# itself left for context) wedged the gate. Fix decomposes into an explicit
# `cr_open_blocks` that's false when cr_state==APPROVED.

@test "H1: APPROVED on head + 1 unresolved CR thread -> pass (approval trumps cr_open)" {
    # Start from the pass fixture (which has cr_state=NONE) and graft on:
    #   - an APPROVED CR review on the current head SHA,
    #   - an unresolved non-outdated thread with a CR comment (would
    #     have set cr_open=1 under the prior logic).
    # Expected pre-fix: gate blocks because cr_open=1.
    # Expected post-fix: gate passes — APPROVED ignores cr_open.
    local f1 f2
    f1=$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviews.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"state":"APPROVED","submittedAt":"2026-05-23T18:00:00Z","commit":{"oid":"abc123"},"body":""}]')
    f2=$(fixture_override "$f1" \
        "data.repository.pullRequest.reviewThreads.nodes" \
        '[{"isResolved":false,"isOutdated":false,"comments":{"nodes":[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"}}],"pageInfo":{"hasNextPage":false}}}]')
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: APPROVED"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$f1" "$f2"
}

@test "H1: COMMENTED on head + 1 unresolved CR thread still blocks (only APPROVED waives cr_open)" {
    # Anti-regression test: ensure the cr_open=0 requirement still binds for
    # non-APPROVED CR states. Use the existing CR-clean fixture (Actionable=0
    # → cr_pass=true under COMMENTED) and graft on an unresolved thread.
    local f
    f=$(fixture_override "$FIXTURES_DIR/merge_gates_cr_current_clean.json" \
        "data.repository.pullRequest.reviewThreads.nodes" \
        '[{"isResolved":false,"isOutdated":false,"comments":{"nodes":[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"}}],"pageInfo":{"hasNextPage":false}}}]')
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    rm -f "$f"
}

# ---------- H12 (cr_installed probe — distinguish 404 from other errors) ----------
# docs/evaluation/agentic-infrastructure-2026-05-23.md § H12: previously any
# non-zero `gh api .coderabbit.yaml` exit set cr_installed=false → the CR
# gate auto-passed on transient gh errors, auth failures, network blips.
# Fix-direction: confirmed 404 → cr_installed=false; anything else →
# cr_installed=true (fail safe; gate blocks on unknown state rather than
# silently passing).

@test "H12: cr_installed probe - confirmed 404 -> cr_installed=false -> NONE passes (preserved behavior)" {
    # Default stub behavior is 404. Existing test
    # "no CodeRabbit review ever → cr_state=NONE → contributes to pass"
    # already exercises this; re-asserting here for clarity grouped with H12.
    export MERGE_GATES_STUB_CR_CONFIG=404
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: NONE"* ]]
    unset MERGE_GATES_STUB_CR_CONFIG
}

@test "H12: cr_installed probe - 401 auth failure -> cr_installed=true (fail safe) -> NONE blocks until grace expires" {
    # With cr_installed=true (fail-safe assumed) and no CR review,
    # NONE+pending blocks on poll 1 (grace=10 by default, poll 0 < 10).
    # Pre-H12: any non-zero gh exit → cr_installed=false → NONE passes
    # silently. Post-H12: 401 not 404 → fail safe → block.
    export MERGE_GATES_STUB_CR_CONFIG=401
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    [[ "$output" == *"non-404 error"* ]]
    unset MERGE_GATES_STUB_CR_CONFIG
}

@test "H12: cr_installed probe - transient (no HTTP code) -> cr_installed=true (fail safe) -> block" {
    # Same as 401 test but with a stub error that doesn't carry "HTTP 404"
    # — covers DNS / connect / read-timeout shape.
    export MERGE_GATES_STUB_CR_CONFIG=transient
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    unset MERGE_GATES_STUB_CR_CONFIG
}

# ---------- ask_user_question shim ----------

@test "ask_user_question with MERGE_GATES_TEST_ANSWER returns canned answer" {
    export MERGE_GATES_TEST_ANSWER="Abandon"
    run ask_user_question "Pick:" "Skip" "Wait" "Abandon"
    [ "$status" -eq 0 ]
    [ "$output" = "Abandon" ]
}

@test "ask_user_question with no options returns 1" {
    run ask_user_question "Pick:"
    [ "$status" -eq 1 ]
}

@test "ask_user_question non-TTY without canned answer returns 1" {
    # bats runs scripts without a TTY by default — `read` would hang here
    # without the TTY guard. With the guard, this must return 1 fast.
    unset MERGE_GATES_TEST_ANSWER
    run ask_user_question "Pick:" "A" "B"
    [ "$status" -eq 1 ]
    [[ "$output" == *"stdin is not a TTY"* ]]
}

@test "MERGE_GATES_FLIP_READY=true calls gh pr ready before polling" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FLIP_READY=true
    # Marker-file approach: gh_pr_ready_idempotent captures stub stderr in a
    # local subshell, so stderr can't leak to bats $output. The stub writes
    # to MERGE_GATES_STUB_READY_MARKER when invoked — file-existence proves
    # the call path was taken even though stderr is swallowed.
    export MERGE_GATES_STUB_READY_MARKER="${BATS_TMPDIR:-/tmp}/ready-called-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_READY_MARKER"
    export MERGE_GATES_STUB_READY_EXIT=0
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [ -f "$MERGE_GATES_STUB_READY_MARKER" ]
    [[ "$output" == *"GATES_PASSED"* ]]
    rm -f "$MERGE_GATES_STUB_READY_MARKER"
}

@test "MERGE_GATES_FLIP_READY unset does NOT flip to ready (default poll-only)" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    unset MERGE_GATES_FLIP_READY
    # If gh pr ready were invoked the stub would touch this marker file.
    # Absence proves the flip path was NOT taken.
    export MERGE_GATES_STUB_READY_MARKER="${BATS_TMPDIR:-/tmp}/ready-called-${BATS_TEST_NUMBER}"
    rm -f "$MERGE_GATES_STUB_READY_MARKER"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [ ! -f "$MERGE_GATES_STUB_READY_MARKER" ]
    [[ "$output" != *"WARN: gh_pr_ready_idempotent"* ]]
}

@test "MERGE_GATES_FLIP_READY=true with non-zero gh pr ready logs WARN but continues" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    export MERGE_GATES_FLIP_READY=true
    export MERGE_GATES_STUB_READY_EXIT=1
    export MERGE_GATES_STUB_READY_STDERR="some gh pr ready failure"
    # gh pr view fallback says PR is still draft, so gh_pr_ready_idempotent returns 6.
    export MERGE_GATES_STUB_VIEW_ISDRAFT=true
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    # The WARN goes to stderr; bats `run` captures both into $output.
    [[ "$output" == *"WARN: gh_pr_ready_idempotent returned non-zero"* ]]
}

@test "gh-fail path enforces wall-clock timeout (returns 2 before 3 fails)" {
    # Intermittent fails that never hit 3-in-a-row would otherwise run forever.
    # Force TIMEOUT_SECONDS=0 so the very first failed poll triggers timeout.
    export MERGE_GATES_TIMEOUT_SECONDS=0
    export MERGE_GATES_MAX_POLLS=10
    export MERGE_GATES_STUB_GH_FAIL="transient error"
    run poll_merge_gates org repo 1
    [ "$status" -eq 2 ]
    [[ "$output" == *"GATES_TIMEOUT"* ]]
}

# ---------- Cursor Bugbot gate (#4) — docs/plans/.../bugbot-merge-gate.md ----------
# Bugbot (cursor[bot]) posts line-anchored inline findings (→ reviewThreads) +
# an always-COMMENTED summary review, and usage-cap status comments on the
# conversation tab. The gate guards on unresolved cursor[bot] inline findings
# (bb_open) only; three no-wedge hatches (TERMINAL short-circuit, the STALE
# BB_GRACE_POLLS wait, and the bugbot-out-of-band label) keep a usage-capped or
# silent Bugbot from ever wedging a merge. Decision ORDER is load-bearing: open
# findings BLOCK before the terminal-signal hatch is consulted.

@test "Bugbot (1) 2 open cursor[bot] threads on head -> BLOCK" {
    # bb_open=2 unresolved cursor[bot] threads + a COMMENTED Bugbot review on head.
    # CI green, CR not installed (NONE→pass), reviewDecision APPROVED, user=0, so
    # Bugbot is the SOLE blocker.
    set_fixture "$FIXTURES_DIR/merge_gates_bb_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"Bugbot: COMMENTED (2 unresolved)"* ]]
    [[ "$output" == *"BLOCK: Cursor Bugbot has 2 unresolved finding(s) on the head"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
}

@test "Bugbot (2) 2 open threads + bugbot-out-of-band label -> WARN/pass" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"bugbot-out-of-band"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"bugbot-out-of-band label downgraded Bugbot block (2 unresolved cursor[bot] finding(s)) to WARN"* ]]
    rm -f "$f"
}

@test "Bugbot (3) usage-limit conversation comment, no open threads -> PASS (terminal short-circuit, skips grace)" {
    # The spend-cap-no-wedge canary. Bugbot couldn't run → a status comment, no
    # review, no findings. Must PASS immediately even with the default grace
    # window in force (terminal short-circuit skips grace).
    set_fixture "$FIXTURES_DIR/merge_gates_bb_terminal.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"Bugbot: TERMINAL (Bugbot couldn't run / usage limit — no-wedge pass)"* ]]
}

@test "Bugbot (4) no cursor[bot] review on head + poll < BB_GRACE_POLLS -> PENDING (grace wait)" {
    # STALE: Bugbot reviewed a PRIOR commit only. Within the grace window the
    # gate waits (don't merge under a mid-re-review Bugbot).
    set_fixture "$FIXTURES_DIR/merge_gates_bb_stale.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"Bugbot: STALE-grace (no Bugbot review on head — poll 1/10)"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
}

@test "Bugbot (5) no cursor[bot] review on head + grace expired (BB_GRACE_POLLS=0) -> PASS (never wedge)" {
    export MERGE_GATES_BB_GRACE_POLLS=0
    set_fixture "$FIXTURES_DIR/merge_gates_bb_stale.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"Bugbot: STALE-grace-expired (pass)"* ]]
    unset MERGE_GATES_BB_GRACE_POLLS
}

@test "Bugbot (6) clean cursor[bot] review on head, bb_open=0 -> PASS" {
    set_fixture "$FIXTURES_DIR/merge_gates_bb_clean.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"Bugbot: COMMENTED (clean)"* ]]
}

@test "Bugbot (7) bugbot-out-of-band label with no review on head yet -> PASS (waiver short-circuits grace)" {
    # STALE base would PENDING within grace; the label short-circuits it to PASS
    # even with the default grace window (the oob check precedes the grace check).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_stale.json" \
        "data.repository.pullRequest.labels" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"name":"bugbot-out-of-band"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"Bugbot: bugbot-out-of-band (pass)"* ]]
    rm -f "$f"
}

@test "Bugbot (8) no cursor[bot] artefacts at all -> unchanged pass (Bugbot-free PR)" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"Bugbot: ABSENT (no Bugbot activity)"* ]]
}

@test "Bugbot (9) field-count guard fires on a mis-sized tuple (fail-closed canary)" {
    # An embedded newline in a tuple field inflates the field count past 33; the
    # -ne 33 fail-closed assertion must catch it (the tuple-order regression guard
    # that the appended Bugbot + selfImpOnly + pureDocs/crRateLimited/crDisposition
    # + thread-count fields rely on).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.headRefOid" \
        '"abc\n123"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -ne 0 ]
    [[ "$output" == *"expected 33"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "Bugbot (10) existing CR-finding block still blocks with Bugbot ABSENT (no interference)" {
    # Regression canary: a CR-finding block on a Bugbot-free PR must still block —
    # the ABSENT Bugbot bucket computes a clean pass and never masks the CR gate.
    set_fixture "$FIXTURES_DIR/merge_gates_cr_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"actionable"* ]]
    [[ "$output" == *"Bugbot: ABSENT"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
}

@test "Bugbot (11) open findings + usage-limit comment on head -> still BLOCK (terminal must NOT override findings)" {
    # Decision-order canary (plan Finding 1): the terminal short-circuit sits
    # BELOW the open-findings branch, so a usage-limit comment that coexists with
    # real unresolved cursor[bot] threads must still BLOCK — a spend cap does not
    # erase findings the user has not addressed.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"cursor[bot]","__typename":"Bot"},"body":"### Bugbot status - usage limit reached"}]')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"BLOCK: Cursor Bugbot has 2 unresolved finding(s) on the head"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "Bugbot (12) open findings withhold the AGGREGATE GATES_PASSED (bb_block consume-point canary)" {
    # plan Finding 2: the bucket alone only emits a verdict; the bb_block conjunct
    # in the GATES_PASSED predicate is what actually withholds the aggregate pass.
    # Everything else is green (0 CI fail, reviewDecision APPROVED) yet the gate
    # must NOT emit GATES_PASSED while Bugbot has open findings.
    set_fixture "$FIXTURES_DIR/merge_gates_bb_findings.json"
    run poll_merge_gates org repo 1
    [[ "$output" == *"0 fail"* ]]
    [[ "$output" == *"reviewDecision: APPROVED"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    [ "$status" -ne 0 ]
}

# ---------- Self-improvement doc PR auto-exemption (plan: self-improvement-pr-review-exemption) ----------
# A PR whose diff is ENTIRELY under docs/self-improvement/** auto-skips the CR + Bugbot
# gates (no label). $selfImpOnly (tuple field 27) is derived from the PR's files list;
# these cases inject a `files` node via fixture_override (legacy fixtures have none, so
# selfImpOnly defaults false there — the Bugbot 1-12 cases above are the not-exempt baseline).

@test "self-improvement (1) pure docs/self-improvement diff + open cursor[bot] finding -> PASS (Bugbot auto-skip)" {
    # bb_findings has 2 open cursor[bot] threads (Bugbot 1 → BLOCK). Tagging the diff
    # as pure self-improvement auto-downgrades that block to WARN → GATES_PASSED.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/self-improvement/categories/tooling/2026-06-20-x.md"},{"path":"docs/self-improvement/postmortems.md"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"self-improvement doc PR"* ]]
    [[ "$output" == *"Bugbot gate auto-skipped"* ]]
    [[ "$output" == *"self-improvement doc PR → WARN"* ]]
    rm -f "$f"
}

@test "self-improvement (2) mixed diff (self-improvement + one Source/ file) + open finding -> BLOCK (scope canary)" {
    # A single non-self-improvement path flips selfImpOnly false → the Bugbot block
    # stands. Guards the 'no code rides along unreviewed' scope decision.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/self-improvement/postmortems.md"},{"path":"Source/Core/src/Foo.cpp"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"BLOCK: Cursor Bugbot has 2 unresolved finding(s) on the head"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "self-improvement (3) >100-file page (pageInfo.hasNextPage) -> NOT exempt -> BLOCK (fail-safe canary)" {
    # Files overflow means we can't see every path, so selfImpOnly fails safe to
    # false even though every visible path is self-improvement → full gates.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":true},"nodes":[{"path":"docs/self-improvement/x.md"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"BLOCK: Cursor Bugbot has 2 unresolved finding(s) on the head"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "self-improvement (4) empty files list -> NOT exempt -> BLOCK (vacuous-all guard)" {
    # length 0 must NOT vacuously satisfy all() — an empty diff is not 'pure
    # self-improvement'. selfImpOnly false → the Bugbot block stands.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_findings.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"BLOCK: Cursor Bugbot has 2 unresolved finding(s) on the head"* ]]
    [[ "$output" != *"GATES_PASSED"* ]]
    rm -f "$f"
}

@test "self-improvement (5) STALE Bugbot (no review on head) + pure self-improvement -> PASS (short-circuits grace)" {
    # bb_stale would PENDING within the grace window (Bugbot 4). The self-improvement
    # branch precedes the STALE-grace check → PASS without waiting.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_bb_stale.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/self-improvement/applied.md"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"self-improvement-doc-pr (Bugbot STALE grace auto-skipped)"* ]]
    rm -f "$f"
}

@test "self-improvement (6) CR CHANGES_REQUESTED + pure self-improvement -> PASS (CR belt-and-suspenders)" {
    # cr_changes blocks on a CHANGES_REQUESTED review (CR test → return 1). The
    # self-improvement CR auto-skip downgrades it (the path_filter normally makes CR
    # 'Review skipped', but this covers a residual/stale CR block).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_cr_changes.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/self-improvement/categories/process/2026-06-20-y.md"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"self-improvement doc PR — CR gate auto-skipped"* ]]
    rm -f "$f"
}

@test "self-improvement (7) already-green pass fixture + self-improvement files -> PASS, exemption is moot (no spurious WARN)" {
    # When nothing is blocking, the exemption is load-bearing-only: it must NOT emit
    # an auto-skip WARN or set cr_override (the GATE_SNAPSHOT clean-merge contract).
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/self-improvement/AGENT_SELF_IMPROVEMENT.md"}]}')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"GATE_SNAPSHOT cr_override=0 downgraded="* ]]
    [[ "$output" != *"auto-skipped"* ]]
    rm -f "$f"
}

# ---------- CR rate-limit handling (PR-2: cr-review-skipped-pure-docs-auto-downgrade
#            + cr-rate-limit-code-pr-auto-pause) ----------
# A CR rate-limit skip is TEMPORARY (CR re-reviews on quota recovery), distinct
# from the terminal "Review skipped" pass. On a PURE-DOCS PR it auto-downgrades to
# WARN with no label (markdown is never compiled). On a CODE PR it PAUSES the gate
# (block) until CR re-reviews; cr-out-of-band alone will NOT waive it — the operator
# must ALSO attest a `cr-disposition:` label. The rate-limit signal is read from CR
# conversation-comment bodies (and the CodeRabbit StatusContext description). These
# tests build on merge_gates_pass.json (all CI green, reviewDecision APPROVED) with
# CR installed so the CR=NONE path is the realistic scenario, then layer a rate-limit
# comment + a files list (pure-docs vs code) via chained fixture_override.

@test "CR rate-limit + pure-docs PR PASS (auto-downgrade, no label)" {
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"> Review skipped\n\nCodeRabbit hit its rate limit. Please try again later."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"docs/agent-rules/merge-gates.md"},{"path":"AGENTS.md"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"rate-limited on a pure-docs PR"* ]]
    [[ "$output" == *"pure-docs-auto-downgrade"* ]]
    rm -f "$f1" "$f2"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR rate-limit + CODE PR + NO current-head CR verdict BLOCK (pause for re-review)" {
    # The rate-limit block on a CODE PR fires ONLY when cr_state == NONE (no CR
    # review object exists on the current head) — the rate-limit comment is then
    # the only CR signal, so pause for re-review. reviews.nodes empty => cr_state
    # NONE; set explicitly here so this case keeps exercising the block after the
    # finding fix gated the block on cr_state==NONE.
    local f1 f2 f3
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"> Review skipped\n\nCodeRabbit hit its rate limit. Please try again later."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"Source/Core/src/Foo.cpp"}]}')"
    f3="$(fixture_override "$f2" \
        "data.repository.pullRequest.reviews" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f3"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"rate-limited on a CODE PR"* ]]
    [[ "$output" == *"CODE-PR-pause"* ]]
    rm -f "$f1" "$f2" "$f3"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR rate-limit + CODE PR + current-head APPROVED verdict does NOT block (stale rate-limit ignored)" {
    # Regression for the PR-2 finding: a STALE rate-limit comment from a PRIOR
    # push must NOT override a legitimate current-head CR verdict. With an
    # APPROVED CR review on the head sha (abc123) the rate-limit block must NOT
    # fire — the current-head verdict wins and the gate passes, while the print
    # annotates the stale rate-limit as non-blocking.
    local f1 f2 f3
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"> Review skipped\n\nCodeRabbit hit its rate limit. Please try again later."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"Source/Core/src/Foo.cpp"}]}')"
    f3="$(fixture_override "$f2" \
        "data.repository.pullRequest.reviews" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"author":{"login":"coderabbitai[bot]"},"state":"APPROVED","submittedAt":"2026-06-19T00:00:00Z","commit":{"oid":"abc123"},"body":"Approved."}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f3"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" != *"CODE-PR-pause"* ]]
    [[ "$output" == *"rate-limit stale-on-prior-push"* ]]
    rm -f "$f1" "$f2" "$f3"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR rate-limit + CODE PR + cr-out-of-band ALONE still BLOCKS (needs cr-disposition)" {
    local f1 f2 f3
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"Review skipped: CodeRabbit rate limit reached, try again later."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"Source/Core/src/Foo.cpp"}]}')"
    f3="$(fixture_override "$f2" \
        "data.repository.pullRequest.labels.nodes" \
        '[{"name":"cr-out-of-band"}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f3"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" != *"GATES_PASSED"* ]]
    [[ "$output" == *"NOT honoured"* ]]
    [[ "$output" == *"cr-disposition"* ]]
    rm -f "$f1" "$f2" "$f3"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR rate-limit + CODE PR + cr-out-of-band + cr-disposition PASS (honoured)" {
    local f1 f2 f3
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"Review skipped: CodeRabbit rate limit reached, try again later."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"Source/Core/src/Foo.cpp"}]}')"
    f3="$(fixture_override "$f2" \
        "data.repository.pullRequest.labels.nodes" \
        '[{"name":"cr-out-of-band"},{"name":"cr-disposition:rate-limit-acked"}]')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f3"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"cr-out-of-band + cr-disposition"* ]]
    [[ "$output" == *"GATE_SNAPSHOT cr_override=1"* ]]
    rm -f "$f1" "$f2" "$f3"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR rate-limit parser: 'rate-limited' phrasing detected (CR-state parser fixture)" {
    # Fixture around the CR-state parser: the crRateLimited derivation must match
    # the hyphenated 'rate-limited' variant (not just 'rate limit'). Pure-docs so
    # the match path is the auto-downgrade WARN (asserts the regex fired).
    local f1 f2
    f1="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.comments.nodes" \
        '[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"body":"CodeRabbit is currently rate-limited; review deferred."}]')"
    f2="$(fixture_override "$f1" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"README.md"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f2"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"pure-docs-auto-downgrade"* ]]
    rm -f "$f1" "$f2"
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR no rate-limit comment: rate-limit path inert (negative canary)" {
    # No rate-limit signal anywhere → crRateLimited false → the rate-limit block is
    # never entered, even on a CODE PR. merge_gates_pass.json is all-green + CR NONE
    # (installed) so it passes the normal grace path; the rate-limit handling must
    # not spuriously fire.
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.files" \
        '{"pageInfo":{"hasNextPage":false},"nodes":[{"path":"Source/Core/src/Foo.cpp"}]}')"
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [[ "$output" != *"rate-limit"* ]]
    [[ "$output" != *"CODE-PR-pause"* ]]
    rm -f "$f"
    unset MERGE_GATES_CR_INSTALLED
}
