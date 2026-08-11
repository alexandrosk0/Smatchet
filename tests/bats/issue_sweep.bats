#!/usr/bin/env bats
# tests/bats/issue_sweep.bats
# Coverage for agents/scripts/core/issue-sweep.sh (issue-triage Slice 3).
# Tests the gh-free surface via an ISSUES_JSON fixture: verdict logic + the
# [issue-propose] line + the bot-only auto-act guardrail (dry-run mutates nothing).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPT="$REPO_ROOT/agents/scripts/core/issue-sweep.sh"
    WORK="$BATS_TEST_TMPDIR"; export WORK
    # Exec-validate the interpreter: a bare `command -v python3` matches the
    # Windows WinStore alias stub, which resolves on PATH but exits non-zero on
    # run — the skip guard then never fires and every verdict assertion fails.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    [ -n "$PY" ] || skip "no working python interpreter"
    cat > "$WORK/issues.json" <<'JSON'
[
 {"number":734,"title":"bulkImportFutures.clear() blocks UI thread","author":{"login":"app/coderabbitai"},"labels":[]},
 {"number":900,"title":"User crash on export","author":{"login":"alexk"},"labels":[{"name":"bug"},{"name":"P0"}]},
 {"number":901,"title":"bot found a P1 race","author":{"login":"coderabbitai[bot]"},"labels":[{"name":"bug"},{"name":"P1"}]}
]
JSON
    export ISSUES_JSON="$WORK/issues.json"
}

@test "bot-authored unlabeled Issue -> RELABEL" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RELABEL #734"*"bot"* ]]
}

@test "human-authored Issue -> KEEP (report-only, never auto-acted)" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"KEEP    #900"*"human"* ]]
}

@test "labelled bot Issue -> KEEP (already triaged)" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"KEEP    #901"* ]]
}

@test "the top P0 is surfaced as an [issue-propose] line" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"[issue-propose] #900"* ]]
    [[ "$output" == *"gh issue develop 900"* ]]
}

@test "dry-run acts on nothing" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"dry-run; --apply acts on bot-authored only"* ]]
}

@test "bad arg exits 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}

# ---------- --apply path (bats-coverage-08) ----------
# Stub `gh` on PATH so --apply's auto-act calls are recorded, not real. The stub
# logs every invocation to $GH_LOG and exits 0; ISSUES_JSON/MERGED_PRS_JSON still
# feed the read side, so the ONLY live gh calls are the mutating edits under test.
_mk_gh_stub() {
    mkdir -p "$WORK/bin"
    export GH_LOG="$WORK/gh.log"; : > "$GH_LOG"
    cat > "$WORK/bin/gh" <<STUB
#!/usr/bin/env bash
echo "gh \$*" >> "$GH_LOG"
exit 0
STUB
    chmod +x "$WORK/bin/gh"
    export PATH="$WORK/bin:$PATH"
}

@test "--apply relabels a bot-authored stray via gh issue edit" {
    _mk_gh_stub
    MERGED_PRS_JSON="$WORK/none.json"; echo '[]' > "$MERGED_PRS_JSON"
    ISSUES_JSON="$ISSUES_JSON" MERGED_PRS_JSON="$MERGED_PRS_JSON" run bash "$SCRIPT" --apply
    [ "$status" -eq 0 ]
    # Only the unlabeled bot Issue #734 is acted on -> exactly one bot-only action.
    [[ "$output" == *"applied 1 bot-only action(s)"* ]]
    grep -q "gh issue edit 734 --repo .* --add-label bug" "$GH_LOG"
}

@test "--apply never auto-acts on a human-authored Issue" {
    _mk_gh_stub
    MERGED_PRS_JSON="$WORK/none.json"; echo '[]' > "$MERGED_PRS_JSON"
    ISSUES_JSON="$ISSUES_JSON" MERGED_PRS_JSON="$MERGED_PRS_JSON" run bash "$SCRIPT" --apply
    [ "$status" -eq 0 ]
    # #900 is human-authored (KEEP, report-only) -> gh must never touch it.
    ! grep -q "gh issue edit 900" "$GH_LOG"
}

@test "--apply strips a lingering out-of-band label off a merged PR" {
    _mk_gh_stub
    ISSUES_JSON="$WORK/empty.json"; echo '[]' > "$ISSUES_JSON"
    cat > "$WORK/merged.json" <<'JSON'
[
 {"number":555,"labels":[{"name":"cr-out-of-band"},{"name":"area:sync"}]}
]
JSON
    ISSUES_JSON="$ISSUES_JSON" MERGED_PRS_JSON="$WORK/merged.json" run bash "$SCRIPT" --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"OOB-STRIP #555 remove-label 'cr-out-of-band'"* ]]
    [[ "$output" == *"removed 1"* ]]
    grep -q "gh pr edit 555 --repo .* --remove-label cr-out-of-band" "$GH_LOG"
}

# ── Gate-logic staleness wiring (script-freshness-remaining-callers, P3) ──────
# These assert the CALL, not the library — script_freshness.bats already covers
# fresh/stale/unverifiable classification and the print policy. What is untested
# elsewhere, and what actually regressed on the other callers, is the wiring:
# whether the script locates the lib RELATIVE TO ITSELF and passes its real
# declared set. A hook that is present but never reached is the same invisible
# no-op this whole line of work exists to close, so it gets a live assertion
# rather than a grep over the source.
#
# Method: run a COPY of the script beside a stub lib. ISSUE_SWEEP_DIR is derived
# from BASH_SOURCE, so the copy sources the stub instead of the real library,
# and the stub records exactly what it was called with.
_mk_sweep_copy() {   # -> $COPY_DIR with issue-sweep.sh + lib/script-freshness.sh stub
    COPY_DIR="$WORK/copy"; export COPY_DIR
    mkdir -p "$COPY_DIR/lib"
    cp "$SCRIPT" "$COPY_DIR/issue-sweep.sh"
    export STALE_LOG="$WORK/stale.log"
    cat > "$COPY_DIR/lib/script-freshness.sh" <<STUB
warn_if_script_stale() { printf '%s\n' "\$*" >> "$STALE_LOG"; return 0; }
STUB
}

@test "freshness: the sweep calls warn_if_script_stale with its own declared set" {
    _mk_sweep_copy
    ISSUES_JSON="$ISSUES_JSON" run bash "$COPY_DIR/issue-sweep.sh"
    [ "$status" -eq 0 ]
    [ -f "$STALE_LOG" ]
    # The label, plus BOTH members of the declared set: the entry point alone is
    # not enough once the verdict depends on sourced logic — a stale helper beside
    # a current entry point is exactly as wrong, and invisible.
    grep -q "issue-sweep triage logic" "$STALE_LOG"
    grep -q "agents/scripts/core/issue-sweep.sh" "$STALE_LOG"
    grep -q "agents/scripts/core/lib/script-freshness.sh" "$STALE_LOG"
}

@test "freshness: the warning is emitted AFTER the sweep's own verdicts" {
    # Ordering is the contract: the note qualifies the verdicts above it, so it
    # must not print before there is anything to qualify.
    _mk_sweep_copy
    cat > "$COPY_DIR/lib/script-freshness.sh" <<'STUB'
warn_if_script_stale() { echo "FRESHNESS-MARKER"; return 0; }
STUB
    ISSUES_JSON="$ISSUES_JSON" run bash "$COPY_DIR/issue-sweep.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RELABEL #734"*"FRESHNESS-MARKER"* ]]
}

@test "freshness: a missing lib degrades to silence, never an error" {
    # A partial checkout must not turn an advisory nudge into a hard failure.
    _mk_sweep_copy
    rm -f "$COPY_DIR/lib/script-freshness.sh"
    ISSUES_JSON="$ISSUES_JSON" run bash "$COPY_DIR/issue-sweep.sh"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RELABEL #734"* ]]
}
