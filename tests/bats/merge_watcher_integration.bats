#!/usr/bin/env bats
# tests/bats/merge_watcher_integration.bats
# ----------------------------------------------------------------------------
# Phase 5 of docs/design/smatchet-merge-watcher.md — end-to-end integration
# coverage. Stubs `gh` on PATH to drive a fake PR through the full lifecycle:
#
#   register → poll BLOCKED → poll PASSED → squash-merge → cascade → unregister
#
# The unit-level bats (merge_watcher.bats) cover registry CRUD + per-helper
# behavior; this file exercises the daemon's full state machine against
# canned `gh api` fixtures.
#
# Requires: python3, bats. No jq dep.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPTS_DIR="$REPO_ROOT/scripts/dev"

    # Isolate per-user state via LOCALAPPDATA (the watcher's per-user root).
    #
    # git-bash mktemp yields a driveless POSIX path (/c/Users/...); native
    # Windows Python — both the CLI subprocess and the in-process module under
    # test — mis-resolves that for open()/json.load. The python blocks below
    # read r'$LOCALAPPDATA' verbatim, so a driveless value FileNotFounds the
    # registry read (the CLI's `register` write lands elsewhere). cygpath -m
    # converts to a mixed C:/... form (drive letter + forward slashes) that
    # BOTH native Python and git-bash filesystem tests accept. POSIX has no
    # cygpath and mktemp already returns a valid path, so use it verbatim there.
    # Keep the raw POSIX path in SMATCHET_TEST_TMP for teardown's rm. Mirrors
    # PR #527's fix to merge_watcher.bats.
    SMATCHET_TEST_TMP="$(mktemp -d)"
    if command -v cygpath >/dev/null 2>&1; then
        LOCALAPPDATA="$(cygpath -m "$SMATCHET_TEST_TMP")"
    else
        LOCALAPPDATA="$SMATCHET_TEST_TMP"
    fi
    export LOCALAPPDATA SMATCHET_TEST_TMP
    export PYTHONIOENCODING=utf-8

    # Stub gh on PATH with a fixture-driven response set.
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR
    cat > "$STUB_BIN_DIR/gh" <<'STUB'
#!/usr/bin/env bash
# Stub gh — fixture-driven per $MERGE_WATCHER_TEST_PHASE.
# Phases:
#   "register"     — gh repo view + gh rev-parse work normally
#   "poll-blocked" — gh api graphql for merge-gates returns CR BLOCKED
#   "poll-passed"  — gh api graphql returns GATES_PASSED
#   "merged"       — gh api -X PUT /merge returns merged:true
#   "cascade"      — gh pr list returns one stacked child
case "$1 $2" in
    "repo view")
        echo '{"owner":{"login":"acme"},"name":"smatchet"}'
        exit 0
        ;;
    "pr ready")
        # ensure_pr_ready_for_review — exit 0 ⇒ caller treats PR as non-draft.
        exit 0
        ;;
    "api graphql")
        case "${MERGE_WATCHER_TEST_PHASE:-poll-blocked}" in
            poll-blocked)
                # Force gates to BLOCKED state via CR-finding fixture.
                cat <<JSON
{"data":{"repository":{"pullRequest":{"state":"OPEN","isDraft":false,"reviewDecision":"APPROVED","headRefOid":"abc123","headRefName":"feat/x","commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"pageInfo":{"hasNextPage":false},"nodes":[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true}]}}}}]},"reviews":{"pageInfo":{"hasNextPage":false},"nodes":[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"state":"COMMENTED","submittedAt":"2026-05-21T10:00:00Z","commit":{"oid":"abc123"},"body":"**Actionable comments posted: 2**\nFinding details..."}]},"reviewThreads":{"pageInfo":{"hasNextPage":false},"nodes":[]},"comments":{"pageInfo":{"hasNextPage":false},"nodes":[]}}}}}
JSON
                exit 0
                ;;
            poll-passed)
                cat <<JSON
{"data":{"repository":{"pullRequest":{"state":"OPEN","isDraft":false,"reviewDecision":"APPROVED","headRefOid":"abc123","headRefName":"feat/x","commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"pageInfo":{"hasNextPage":false},"nodes":[{"__typename":"CheckRun","name":"build","conclusion":"SUCCESS","status":"COMPLETED","isRequired":true}]}}}}]},"reviews":{"pageInfo":{"hasNextPage":false},"nodes":[{"author":{"login":"coderabbitai[bot]","__typename":"Bot"},"state":"COMMENTED","submittedAt":"2026-05-21T10:00:00Z","commit":{"oid":"abc123"},"body":"**Actionable comments posted: 0**"}]},"reviewThreads":{"pageInfo":{"hasNextPage":false},"nodes":[]},"comments":{"pageInfo":{"hasNextPage":false},"nodes":[]}}}}}
JSON
                exit 0
                ;;
        esac
        ;;
    "api repos/acme/smatchet/pulls/100")
        echo '{"head":{"ref":"feat/x"},"number":100}'
        exit 0
        ;;
    "pr list")
        case "${MERGE_WATCHER_TEST_PHASE:-}" in
            cascade)
                echo '[{"number":101,"headRefName":"feat/y","title":"child"}]'
                ;;
            *)
                echo '[]'
                ;;
        esac
        exit 0
        ;;
    "api -X")
        # `gh api -X PUT <path>` — squash_merge_pr (.../pulls/N/merge) and
        # cascade_update_child (.../pulls/N/update-branch) share this "$1 $2"
        # prefix. The merge fixture must return merged:true while update-branch
        # returns queued, so route on the REST path. Matching "$1 $2" alone is
        # exactly the rot this gate now catches: the old stub returned queued
        # for BOTH, so squash_merge_pr saw merged=false and handle_pass
        # mis-reported merge_failed.
        case "$*" in
            *"/merge"*)         echo '{"merged":true,"sha":"deadbeef1234"}' ;;
            *"/update-branch"*) echo '{"message":"queued"}' ;;
            *)                  echo '{"message":"queued"}' ;;
        esac
        exit 0
        ;;
esac
# Per CodeRabbit on PR #368 — unknown gh invocations must fail loudly so a
# missing-case in the stub doesn't silently turn into a passing test.
echo "stub-gh: unhandled invocation: $*" >&2
exit 1
STUB
    chmod +x "$STUB_BIN_DIR/gh"

    # Windows shim. merge-watcher.py resolves GH_BIN via shutil.which("gh") at
    # import (native Windows Python), then calls subprocess.run([GH_BIN, ...]).
    # Native CreateProcess cannot exec a POSIX bash-script "gh" and ignores the
    # extensionless stub entirely — it falls through to the real
    # C:\Program Files\GitHub CLI\gh.EXE and hits live GitHub (real PR #100 is
    # merged → PR_CLOSED_OR_MERGED, real branch/sha leak into asserts). A
    # `gh.cmd` forwarder IS resolvable by shutil.which (PATHEXT includes .CMD)
    # and, because STUB_BIN_DIR is first on PATH, wins over the real gh.EXE; it
    # re-dispatches into the bash stub so the in-process module AND the
    # merge-gates.sh subprocess both see canned fixtures. The stub routes on
    # "$1 $2" plus the merge/update-branch REST-path token — all space-free —
    # so cmd→bash arg re-quoting of the remaining args is harmless. POSIX bats
    # has no .cmd concept and resolves the extensionless `gh` directly, so this
    # file is inert there. Mirrors merge-watcher.py's gh-resolution path.
    printf '@echo off\r\nbash "%%~dp0gh" %%*\r\n' > "$STUB_BIN_DIR/gh.cmd"

    export PATH="$STUB_BIN_DIR:$PATH"

    # Stub the merge-gates.sh path — daemon's poll_one calls it directly.
    export MERGE_GATES_MAX_POLLS=1
    export MERGE_GATES_POLL_INTERVAL=0
    export ORCH_USER=acme
    export MERGE_GATES_CR_INSTALLED=true
}

teardown() {
    rm -rf "${SMATCHET_TEST_TMP:-}" "${STUB_BIN_DIR:-}"
}

watch_cli() {
    python "$SCRIPTS_DIR/merge-watcher-cli.py" "$@"
}

@test "integration: poll-blocked CR-finding fixture → daemon lands in BLOCKED state + _bump_triage_attempts increments registry" {
    # Per CodeRabbit on PR #368 — test name promised triage_attempts assertion
    # but the original body only checked last_state. Now explicitly bumps the
    # counter via the helper + verifies via the CLI's list view.
    run watch_cli register 100
    [ "$status" -eq 0 ]

    export MERGE_WATCHER_TEST_PHASE=poll-blocked
    run python -c "
import sys, os, importlib.util, json
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
clone = json.load(open(os.path.join(r'$LOCALAPPDATA', 'Smatchet', 'merge-watch', 'active.json')))[0]['clone_path']
entry = {'pr': 100, 'clone_path': clone, 'triage_attempts': 0}
state = m.poll_one(entry)
print('last_state:', state['last_state'])
# Exercise the daemon-loop BLOCKED branch's registry-increment path directly
# (the full handle_blocked_cr_triage would invoke coderabbit-triage.py subprocess,
# which needs more gh stub work; the increment helper is what matters here).
m._bump_triage_attempts(100, clone, 1)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"last_state: BLOCKED"* ]]
    # Verify the counter incremented in the registry.
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 1'* ]]
}

@test "integration: handle_pass squash-merges via gh api stub + drops from registry" {
    run watch_cli register 100
    [ "$status" -eq 0 ]

    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
import json, os
entry = json.load(open(os.path.join(r'$LOCALAPPDATA', 'Smatchet', 'merge-watch', 'active.json')))[0]
extras = m.handle_pass(entry)
print('merge_action:', extras.get('merge_action'))
print('merge_sha:', extras.get('merge_sha'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merged"* ]]
    [[ "$output" == *"merge_sha: deadbeef1234"* ]]

    # Registry should now be empty (PR dropped after successful merge).
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == "[]" ]]
}

@test "integration: handle_pass cascade detects stacked child via pr list stub" {
    run watch_cli register 100
    [ "$status" -eq 0 ]

    export MERGE_WATCHER_TEST_PHASE=cascade
    run python -c "
import sys, importlib.util, json, os
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = json.load(open(os.path.join(r'$LOCALAPPDATA', 'Smatchet', 'merge-watch', 'active.json')))[0]
extras = m.handle_pass(entry)
print('cascade_count:', len(extras.get('cascade_children', [])))
if extras.get('cascade_children'):
    c = extras['cascade_children'][0]
    print('child_pr:', c['pr'])
    print('child_head:', c['head'])
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"cascade_count: 1"* ]]
    [[ "$output" == *"child_pr: 101"* ]]
    [[ "$output" == *"child_head: feat/y"* ]]
}

@test "integration: full state-machine walk register → BLOCKED → unregister leaves registry clean" {
    # Belt-and-suspenders end-to-end smoke. Combines CRUD + poll + cleanup in
    # one test so a regression to any one stage fails this case.
    # Per CodeRabbit on PR #368 — every `run` followed by an output assertion
    # also gets a `[ "$status" -eq 0 ]` guard so the assertion never silently
    # succeeds on a crashed run.
    run watch_cli register 100
    [ "$status" -eq 0 ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"pr": 100'* ]]
    # Poll once (fakeable; we don't care about the result here — just that the
    # daemon helper doesn't crash on a registered PR with stub gh available).
    export MERGE_WATCHER_TEST_PHASE=poll-blocked
    run python -c "
import sys, importlib.util, json, os
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = json.load(open(os.path.join(r'$LOCALAPPDATA', 'Smatchet', 'merge-watch', 'active.json')))[0]
state = m.poll_one(entry)
m.write_state(state)
print('OK')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
    # State file should exist.
    [ -f "$LOCALAPPDATA/Smatchet/merge-watch/state/100.json" ]
    # Unregister + verify clean
    run watch_cli unregister 100
    [ "$status" -eq 0 ]
    [ ! -f "$LOCALAPPDATA/Smatchet/merge-watch/state/100.json" ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == "[]" ]]
}
