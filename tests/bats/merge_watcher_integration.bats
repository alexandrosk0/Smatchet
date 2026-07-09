#!/usr/bin/env bats
# tests/bats/merge_watcher_integration.bats
# ----------------------------------------------------------------------------
# Phase-5 end-to-end coverage for the merge-watcher daemon state machine:
#
#   register -> poll BLOCKED -> handle_pass squash-merge -> cascade -> unregister
#
# The unit-level bats (merge_watcher.bats) cover registry CRUD + per-helper
# behavior; this file exercises the daemon's full handlers (poll_one /
# handle_pass) end-to-end.
#
# gh is NOT stubbed on PATH. The daemon resolves an ABSOLUTE GH_BIN at import
# via shutil.which, and native-Windows Python's shutil.which skips extensionless
# PATH `gh` stubs (it requires a PATHEXT extension) — so the old PATH-stub
# approach silently ran the REAL gh on Windows and every case failed (the suite
# was never gated, so this rotted undetected). Instead we monkeypatch the
# daemon's named gh seams (_pr_lifecycle_state, _poll_owner_repo,
# _poll_run_gates, ensure_pr_ready_for_review, _gh_owner_repo,
# detect_merged_branch_name, squash_merge_pr, find_stacked_children,
# cascade_update_child) so every case is deterministic + offline on all OSes.
#
# Requires: python3, bats. No jq / gh / network dep.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPTS_DIR="$REPO_ROOT/agents/scripts/core"

    # Isolate per-user watcher state in a temp sandbox. The watcher root reads
    # LOCALAPPDATA on Windows; cygpath -m converts the git-bash mktemp path to a
    # mixed C:/... form (drive letter + forward slashes) that BOTH native-Windows
    # Python and git-bash filesystem tests accept — without it, native Python
    # mis-resolves the driveless /c/... path and registry writes/reads diverge
    # (the bug PR #527 fixed for merge_watcher.bats; the same fix was never
    # applied to this file, contributing to its Windows breakage).
    SMATCHET_TEST_TMP="$(mktemp -d)"
    export SMATCHET_TEST_TMP
    if command -v cygpath >/dev/null 2>&1; then
        LOCALAPPDATA="$(cygpath -m "$SMATCHET_TEST_TMP")"
    else
        LOCALAPPDATA="$SMATCHET_TEST_TMP"
    fi
    # POSIX watcher root — without this the CLI writes into the REAL
    # $HOME/.local/state and the tests read $LOCALAPPDATA, so every case
    # false-failed headless AND leaked registry state across runs (the same
    # both-env-vars rule merge_watcher.bats setup() documents).
    XDG_STATE_HOME="$SMATCHET_TEST_TMP"
    export LOCALAPPDATA XDG_STATE_HOME
    export PYTHONIOENCODING=utf-8

    # WATCH_ROOT = where watcher_root() actually lands per-OS (Windows Python
    # reads LOCALAPPDATA/Smatchet; POSIX reads XDG_STATE_HOME/smatchet).
    case "$OSTYPE" in
        msys*|cygwin*|win*) WATCH_ROOT="$LOCALAPPDATA/Smatchet/merge-watch" ;;
        *) WATCH_ROOT="$XDG_STATE_HOME/smatchet/merge-watch" ;;
    esac
    export WATCH_ROOT
}

teardown() {
    # Guard the delete: if setup() aborted before SMATCHET_TEST_TMP was set,
    # `rm -rf ""` can return non-zero and obscure the real failure (CR #538).
    if [ -n "${SMATCHET_TEST_TMP:-}" ]; then
        rm -rf "$SMATCHET_TEST_TMP"
    fi
}

watch_cli() {
    python "$SCRIPTS_DIR/merge-watcher-cli.py" "$@"
}

@test "integration: poll-blocked CR-finding fixture -> daemon lands in BLOCKED + _bump_triage_attempts increments registry" {
    run watch_cli register 100
    [ "$status" -eq 0 ]

    run python -c "
import sys, os, importlib.util, json, types
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
clone = json.load(open(os.path.join(r'$WATCH_ROOT', 'active.json')))[0]['clone_path']
# Patch every gh seam poll_one touches so it reaches the gates-parse path
# deterministically + offline. A CR-finding gates result is returncode 1 -> BLOCKED.
m._pr_lifecycle_state = lambda pr, cp: 'OPEN'
m._poll_owner_repo = lambda pr, cp: ('acme', 'smatchet')
m.ensure_pr_ready_for_review = lambda owner, repo, pr: True
m._poll_run_gates = lambda owner, repo, pr, env: types.SimpleNamespace(
    returncode=1,
    stdout='Poll 1/1 CI: 3/3 pass (0 fail) | CodeRabbit: COMMENTED (2 actionable - block) | User: 0',
    stderr='')
state = m.poll_one({'pr': 100, 'clone_path': clone, 'triage_attempts': 0})
print('last_state:', state['last_state'])
m._bump_triage_attempts(100, clone, 1)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"last_state: BLOCKED"* ]]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 1'* ]]
}

@test "integration: handle_pass squash-merges via stubbed seams + drops from registry" {
    run watch_cli register 100
    [ "$status" -eq 0 ]

    run python -c "
import sys, os, importlib.util, json
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = json.load(open(os.path.join(r'$WATCH_ROOT', 'active.json')))[0]
m._gh_owner_repo = lambda cp: ('acme', 'smatchet')
m.ensure_pr_ready_for_review = lambda owner, repo, pr: True
m.detect_merged_branch_name = lambda owner, repo, pr: 'feat/x'
m.squash_merge_pr = lambda owner, repo, pr: 'deadbeef1234'
m.find_stacked_children = lambda owner, repo, head: []
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

@test "integration: handle_pass cascade detects stacked child via stubbed seam" {
    run watch_cli register 100
    [ "$status" -eq 0 ]

    run python -c "
import sys, os, importlib.util, json
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = json.load(open(os.path.join(r'$WATCH_ROOT', 'active.json')))[0]
m._gh_owner_repo = lambda cp: ('acme', 'smatchet')
m.ensure_pr_ready_for_review = lambda owner, repo, pr: True
m.detect_merged_branch_name = lambda owner, repo, pr: 'feat/x'
m.squash_merge_pr = lambda owner, repo, pr: 'deadbeef1234'
m.find_stacked_children = lambda owner, repo, head: [{'number': 101, 'headRefName': 'feat/y', 'title': 'child'}]
m.cascade_update_child = lambda owner, repo, child_pr: (True, 'queued')
extras = m.handle_pass(entry)
print('cascade_count:', len(extras.get('cascade_children', [])))
c = extras['cascade_children'][0]
print('child_pr:', c['pr'])
print('child_head:', c['head'])
print('child_ok:', c['ok'])
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"cascade_count: 1"* ]]
    [[ "$output" == *"child_pr: 101"* ]]
    [[ "$output" == *"child_head: feat/y"* ]]
    [[ "$output" == *"child_ok: True"* ]]
}

@test "integration: full state-machine walk register -> poll -> write_state -> unregister leaves registry clean" {
    run watch_cli register 100
    [ "$status" -eq 0 ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"pr": 100'* ]]

    run python -c "
import sys, os, importlib.util, json, types
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = json.load(open(os.path.join(r'$WATCH_ROOT', 'active.json')))[0]
m._pr_lifecycle_state = lambda pr, cp: 'OPEN'
m._poll_owner_repo = lambda pr, cp: ('acme', 'smatchet')
m.ensure_pr_ready_for_review = lambda owner, repo, pr: True
m._poll_run_gates = lambda owner, repo, pr, env: types.SimpleNamespace(
    returncode=1, stdout='Poll 1/1 CI: 3/3 pass | CodeRabbit: COMMENTED (2 actionable - block)', stderr='')
state = m.poll_one(entry)
m.write_state(state)
print('last_state:', state['last_state'])
print('OK')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"last_state: BLOCKED"* ]]
    [[ "$output" == *"OK"* ]]
    # State file should exist after write_state.
    [ -f "$WATCH_ROOT/state/100.json" ]
    # Unregister wipes the entry + its state file.
    run watch_cli unregister 100
    [ "$status" -eq 0 ]
    [ ! -f "$WATCH_ROOT/state/100.json" ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == "[]" ]]
}
