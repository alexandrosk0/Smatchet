#!/usr/bin/env bats
# tests/bats/fail_open_gate_cluster.bats
# ----------------------------------------------------------------------------
# Asserts-failure regression suite for the 6-HIGH + 2-MEDIUM fail-open gate
# cluster (docs/self-improvement/categories/tooling.md →
# `fail-open-gate-cluster-zero-match-and-transient-error`; Batch-8/9 historical
# review #439–#541 / #331–#438).
#
# The fixed fail-open class: a gate / test-driver that returns GREEN on a
# NON-result — a filter matching ZERO tests, a transient API error, or a
# non-recursive scan that misses sites. Each test below feeds a SYNTHETIC
# non-result and asserts the gate now FAILS-CLOSED (exits non-zero), proving the
# guard is live. (build-log-regex lesson: a false-pass regression must be caught
# at authoring time — these are the negative-test fixtures.)
#
# selftest: asserts-failure — every @test here drives a synthetic non-result and
# asserts a NON-ZERO exit; none is a pass-only smoke.
#
# Requires: bash, python (json stdlib only), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    DEV="$REPO_ROOT/scripts/dev"
    export DEV
    # A python interpreter the drivers can use (they probe PYTHON / python).
    # Exec-validate each candidate: a bare `command -v python3` matches the
    # Windows WinStore alias stub, which resolves on PATH but exits non-zero on
    # run — the skip guard would never fire and the drivers would parse nothing.
    PY=""
    for c in python python3 py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    [ -n "$PY" ] || skip "no working python interpreter on PATH"
    export PYTHON="$PY"
    STUBDIR="$(mktemp -d)"
    export STUBDIR
}

teardown() {
    [ -n "${STUBDIR:-}" ] && rm -rf "$STUBDIR"
}

# make_exe_stub <passed> <failed> [log] — write a fake Smatchet.exe that ignores
# its args and prints the ui_test.run JSON envelope the drivers parse. Exposed
# to the driver via SMATCHET_EXE.
make_exe_stub() {
    local passed="$1" failed="$2" log="${3:-ok}"
    local exe="$STUBDIR/Smatchet.exe"
    cat > "$exe" <<EOF
#!/usr/bin/env bash
# Fake bucket-E exe: emit only the JSON envelope on stdout.
printf '%s\n' '{"data":{"passed":${passed},"failed":${failed},"log":"${log}"}}'
exit 0
EOF
    chmod +x "$exe"
    echo "$exe"
}

# run_driver <driver-basename> — run a UI-test driver against the stub exe.
run_driver() {
    local driver="$1"
    SMATCHET_EXE="$STUBDIR/Smatchet.exe" \
    SMATCHET_TEST_PORT=0 \
    UI_TEST_FILTER="SyntheticNonexistentFilter" \
        run bash "$DEV/$driver"
}

# ---------------------------------------------------------------------------
# (B) Zero-match filter → must FAIL-CLOSED (passed=0 failed=0)
# ---------------------------------------------------------------------------
ZERO_MATCH_DRIVERS=(
    test-ui-mcp-lua-fresh-state-race.sh
    test-ui-agent-proposal-store-sqlite.sh
    test-ui-ai-assistant-preferences.sh
    test-ui-description-tooltip-markdown-render.sh
    test-ui-spawn-warmup-deterministic-gate.sh
)

@test "zero-match (passed=0 failed=0) fails-closed in every cluster driver" {
    make_exe_stub 0 0 >/dev/null
    for d in "${ZERO_MATCH_DRIVERS[@]}"; do
        run_driver "$d"
        [ "$status" -ne 0 ] || {
            echo "FAIL-OPEN REGRESSION: $d exited 0 on a 0-test run" >&2
            echo "output: $output" >&2
            return 1
        }
        echo "$output" | grep -q "matched 0 tests"
    done
}

@test "a real passing run (passed>0 failed=0) still exits 0 (guard does not false-fail)" {
    make_exe_stub 3 0 >/dev/null
    for d in "${ZERO_MATCH_DRIVERS[@]}"; do
        run_driver "$d"
        [ "$status" -eq 0 ] || {
            echo "FALSE-FAIL: $d exited $status on a legit 3-pass run" >&2
            echo "output: $output" >&2
            return 1
        }
    done
}

@test "a genuine failure (failed>0) still exits non-zero" {
    make_exe_stub 2 1 >/dev/null
    for d in "${ZERO_MATCH_DRIVERS[@]}"; do
        run_driver "$d"
        [ "$status" -ne 0 ] || {
            echo "MISSED FAILURE: $d exited 0 with failed=1" >&2
            return 1
        }
    done
}

# ---------------------------------------------------------------------------
# (C) Non-recursive scan → must catch a violation in a SUBDIR (#430)
# ---------------------------------------------------------------------------
@test "test-tooltip-wrapwidth.sh scans subdirs (recursive os.walk), not just root" {
    # Synthetic source tree: an offending markdown-tooltip block lives under a
    # subdir the old os.listdir() scan never reached. The fixed os.walk() must
    # find it and the gate must FAIL.
    local fakerepo; fakerepo="$(mktemp -d)"
    mkdir -p "$fakerepo/Source/Core/src/Ui"
    cat > "$fakerepo/Source/Core/src/Ui/Offender.cpp" <<'CPP'
void Draw() {
    if (ImGui::BeginTooltip()) {
        MarkdownPreviewRender::Render(text);   // missing the wrap-width field
        ImGui::EndTooltip();
    }
}
CPP
    # Copy the gate in so its `cd "$(dirname)/../.."` resolves to fakerepo.
    mkdir -p "$fakerepo/scripts/dev"
    cp "$DEV/test-tooltip-wrapwidth.sh" "$fakerepo/scripts/dev/"
    run bash "$fakerepo/scripts/dev/test-tooltip-wrapwidth.sh"
    rm -rf "$fakerepo"
    [ "$status" -eq 1 ] || {
        echo "FAIL-OPEN REGRESSION: subdir violation NOT caught (non-recursive scan?)" >&2
        echo "output: $output" >&2
        return 1
    }
    echo "$output" | grep -q "Offender.cpp"
}

@test "test-tooltip-wrapwidth.sh passes when the subdir site has opts.wrapWidth" {
    local fakerepo; fakerepo="$(mktemp -d)"
    mkdir -p "$fakerepo/Source/Core/src/Ui"
    cat > "$fakerepo/Source/Core/src/Ui/Good.cpp" <<'CPP'
void Draw() {
    if (ImGui::BeginTooltip()) {
        opts.wrapWidth = 400.0f;
        MarkdownPreviewRender::Render(text, opts);
        ImGui::EndTooltip();
    }
}
CPP
    mkdir -p "$fakerepo/scripts/dev"
    cp "$DEV/test-tooltip-wrapwidth.sh" "$fakerepo/scripts/dev/"
    run bash "$fakerepo/scripts/dev/test-tooltip-wrapwidth.sh"
    rm -rf "$fakerepo"
    [ "$status" -eq 0 ] || {
        echo "FALSE-FAIL: compliant subdir site flagged" >&2
        echo "output: $output" >&2
        return 1
    }
}
