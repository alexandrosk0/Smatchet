#!/usr/bin/env bats
# tests/bats/merge_gates.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/merge-gates.sh — poll_merge_gates + helpers.
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
    export SCRIPTS_DIR="$REPO_ROOT/scripts/dev"

    # Standard test env
    export ORCH_USER="alexkonstantonis"
    export MERGE_GATES_POLL_INTERVAL=0
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_TIMEOUT_SECONDS=3600

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
            cat "${MERGE_GATES_TEST_FIXTURE:?stub gh: fixture not set}"
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
    # shellcheck source=../../scripts/dev/merge-gates.sh
    source "$SCRIPTS_DIR/merge-gates.sh"
}

teardown() {
    rm -rf "$STUB_BIN_DIR"
    unset MERGE_GATES_TEST_FIXTURE MERGE_GATES_STUB_GH_FAIL MERGE_GATES_STUB_READY_STDERR MERGE_GATES_STUB_READY_EXIT
    unset MERGE_GATES_TEST_ANSWER MERGE_GATES_STUB_CR_CONFIG MERGE_GATES_STUB_READY_MARKER
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

@test "all gates pass → return 0 + GATES_PASSED" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "CI conclusion FAILURE → return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_fail.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
}

@test "CI StatusContext state ERROR → return 1" {
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

@test "CI pending IN_PROGRESS → return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_pending.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 pending"* ]]
}

@test "CI StatusContext state EXPECTED → return 1 (pending)" {
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

@test "CodeRabbit CHANGES_REQUESTED on current head → return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_changes.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"CodeRabbit: CHANGES_REQUESTED"* ]]
}

@test "user conversation comment → return 1" {
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

@test "stale CodeRabbit APPROVED on old SHA → return 1 (STALE)" {
    # Legacy fixture has no body — falls into STALE_UNKNOWN (safe-default block per
    # P1 fix in docs/backlog/agent-self-improvement/process.md).
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_UNKNOWN"* ]]
}

# ---------- CR three-bucket actionable parsing (P1 from 2026-05-21) ----------

@test "CR review on current head with 'Actionable comments posted: N>0' → block (P1: don't merge unaddressed findings)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"COMMENTED (3 actionable"* ]]
    [[ "$output" == *"block"* ]]
}

@test "CR review on current head with 'Actionable comments posted: 0' → pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_current_clean.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"COMMENTED (0 actionable)"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "STALE CR review with 'Actionable comments posted: N>0' → block (P1: surface findings, don't force-merge)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_findings.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE_WITH_FINDINGS (5 actionable"* ]]
}

@test "STALE CR review with 'Actionable comments posted: 0' → pass (prior review was clean)" {
    set_fixture "$FIXTURES_DIR/merge_gates_cr_stale_clean.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"STALE_CLEAN (0 actionable"* ]]
    [[ "$output" == *"GATES_PASSED"* ]]
}

@test "H16: STALE CR with N>0 findings + all threads resolved + StatusContext SUCCESS → pass (CR-driven thread resolution)" {
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

@test "H16: STALE CR with N>0 findings + all threads resolved but StatusContext NOT SUCCESS → still block (need both signals)" {
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

@test "H16: STALE CR with N>0 findings + StatusContext SUCCESS but an open thread → still block (need both signals)" {
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

@test "STALE CR with no Actionable header in body → STALE_UNKNOWN block (safe default)" {
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

@test "CR COMMENTED on current head with empty body → pass (no Actionable header, conservative pass)" {
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

@test "CR body has 'Actionable comments posted: N' on line 7+ but not line 1 → cr_actionable=-1 (per #360 first-line-only parse)" {
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

@test "CR APPROVED on current head with N>0 findings in body → pass (approval trumps body)" {
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

@test "no CodeRabbit review ever → cr_state=NONE → contributes to pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"CodeRabbit: NONE"* ]]
}

@test "CR installed + NONE + no SUCCESS status → block until grace expires" {
    # CR installed (no auto-probe required — env override). Default MAX_POLLS=1 +
    # GRACE_POLLS=10 means poll 0 < 10 → cr_state_print="NONE+pending"; gates block.
    export MERGE_GATES_CR_INSTALLED=true
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"NONE+pending"* ]]
    unset MERGE_GATES_CR_INSTALLED
}

@test "CR installed + NONE + grace expired (GRACE_POLLS=0) → pass with warn" {
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

@test "C4 prong 2: CR installed + NONE + StatusContext=SUCCESS + CR inline comment on head → pass" {
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

@test "C4 prong 2: CR installed + NONE + StatusContext=SUCCESS + NO inline CR comment → block during grace" {
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

@test "C4 prong 2: NONE + StatusContext=SUCCESS + no inline + grace expired → pass with no-inline-evidence WARN" {
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

# ---------- PR state early-exit ----------

@test "PR state=CLOSED → return 4" {
    set_fixture "$FIXTURES_DIR/merge_gates_state.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 4 ]
    [[ "$output" == *"PR_CLOSED"* ]]
}

@test "PR state=MERGED → return 4" {
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

@test "reviewDecision=REVIEW_REQUIRED → return 1" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviewDecision" '"REVIEW_REQUIRED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"reviewDecision: REVIEW_REQUIRED"* ]]
    rm -f "$f"
}

@test "reviewDecision=CHANGES_REQUESTED → return 1" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pass.json" \
        "data.repository.pullRequest.reviewDecision" '"CHANGES_REQUESTED"')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"reviewDecision: CHANGES_REQUESTED"* ]]
    rm -f "$f"
}

@test "reviewDecision=APPROVED → contributes to pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_pass.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"reviewDecision: APPROVED"* ]]
}

# ---------- Pagination overflow (returns 5) ----------

@test "contexts.pageInfo.hasNextPage=true → return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.commits.nodes.0.commit.statusCheckRollup.contexts.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    [[ "$output" == *"PAGINATION_OVERFLOW"* ]]
    rm -f "$f"
}

@test "reviews.pageInfo.hasNextPage=true → return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.reviews.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "reviewThreads.pageInfo.hasNextPage=true → return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.reviewThreads.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "comments.pageInfo.hasNextPage=true → return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.comments.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    rm -f "$f"
}

@test "per-thread comments.pageInfo.hasNextPage=true → return 5" {
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

@test "rerun: same check name with stale FAILURE + fresh SUCCESS → pass via latest-per-name dedup" {
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

@test "tests-out-of-band label downgrades Test-delta FAILURE → WARN, gates pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_tests_oob.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
    [[ "$output" == *"tests-out-of-band"* || "$output" == *"WARN: out-of-band"* ]]
}

@test "perf-out-of-band label downgrades Perf PR-fast FAILURE → WARN, gates pass" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_perf_oob.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
}

@test "out-of-band label does NOT silence unrelated failing checks" {
    set_fixture "$FIXTURES_DIR/merge_gates_label_oob_other_fail_blocks.json"
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
    [[ "$output" == *"1 fail"* ]]
    [[ "$output" == *"1 warn-downgraded"* ]]
}

@test "no out-of-band label → Test-delta FAILURE still blocks" {
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

@test "labels.pageInfo.hasNextPage=true → return 5" {
    local f
    f="$(fixture_override "$FIXTURES_DIR/merge_gates_pagination.json" \
        "data.repository.pullRequest.labels.pageInfo.hasNextPage" 'true')"
    set_fixture "$f"
    run poll_merge_gates org repo 1
    [ "$status" -eq 5 ]
    [[ "$output" == *"PAGINATION_OVERFLOW"* ]]
    rm -f "$f"
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

@test "3 consecutive gh failures → return 3" {
    export MERGE_GATES_MAX_POLLS=3
    export MERGE_GATES_STUB_GH_FAIL="network error"
    run poll_merge_gates org repo 1
    [ "$status" -eq 3 ]
    [[ "$output" == *"GH_API_DOWN"* ]]
}

@test "MAX_POLLS=1 with blocking fixture → return 1" {
    set_fixture "$FIXTURES_DIR/merge_gates_ci_fail.json"
    export MERGE_GATES_MAX_POLLS=1
    run poll_merge_gates org repo 1
    [ "$status" -eq 1 ]
}

@test "ORCH_USER unset → return 3" {
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

@test "C2 fail-closed: malformed contexts.nodes (string not array) → ci_* default -1 → block" {
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

@test "C2 fail-closed: missing reviewThreads → cr_open / user default -1 → block" {
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

@test "H1: APPROVED on head + 1 unresolved CR thread → pass (approval trumps cr_open)" {
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

@test "H12: cr_installed probe — confirmed 404 → cr_installed=false → NONE passes (preserved behavior)" {
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

@test "H12: cr_installed probe — 401 auth failure → cr_installed=true (fail safe) → NONE blocks until grace expires" {
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

@test "H12: cr_installed probe — transient (no HTTP code) → cr_installed=true (fail safe) → block" {
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
