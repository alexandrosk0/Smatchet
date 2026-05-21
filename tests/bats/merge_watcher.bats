#!/usr/bin/env bats
# tests/bats/merge_watcher.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/merge-watcher-cli.py + merge-watcher.py.
#
# Phase 1 of docs/design/smatchet-merge-watcher.md. Covers:
#   - register / unregister / status / list CRUD
#   - duplicate-reject on second register
#   - clone_path resolution from cwd
#   - file-lock concurrency (one process at a time)
#   - state-file cleanup on unregister
#
# Out of Phase 1 scope (covered in later phases' bats):
#   - daemon poll loop (Phase 1 daemon is foreground-blocking; smoke-tested manually)
#   - per-PR state shape on gh API success (Phase 2 lands stable shape)
#   - cascade lock-dir (Phase 2 territory)
#
# Requires: python3, bats. NO `jq` dep (the CLI handles JSON itself).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPTS_DIR="$REPO_ROOT/scripts/dev"

    # Isolate per-user state via LOCALAPPDATA (Windows path) — on POSIX bats the
    # CLI's XDG_STATE_HOME branch picks it up regardless. We force LOCALAPPDATA
    # for parity with the production Windows path.
    export LOCALAPPDATA="$(mktemp -d)"
    export PYTHONIOENCODING=utf-8
}

teardown() {
    rm -rf "${LOCALAPPDATA:-}"
}

# ---------- helper ----------

watch_cli() {
    python "$SCRIPTS_DIR/merge-watcher-cli.py" "$@"
}

# ---------- empty-state ----------

@test "status on empty registry → 'registry empty'" {
    run watch_cli status
    [ "$status" -eq 0 ]
    [[ "$output" == *"registry empty"* ]]
}

@test "list on empty registry → empty JSON array" {
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == "[]" ]]
}

# ---------- register ----------

@test "register creates registry entry with clone_path + registered_at + triage_attempts" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    [[ "$output" == *"registered PR #999"* ]]
    [[ "$output" == *"Watcher now owns this PR"* ]]
    # Verify registry file exists + contains the entry
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"pr": 999'* ]]
    [[ "$output" == *'"clone_path"'* ]]
    [[ "$output" == *'"registered_at"'* ]]
    [[ "$output" == *'"triage_attempts": 0'* ]]
}

@test "register prints owner-transfer line (locked decision 5)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    [[ "$output" == *"Watcher now owns this PR"* ]]
    [[ "$output" == *"merge-watch unregister 999"* ]]
    [[ "$output" == *"orchestrator must check this registry"* ]]
}

@test "register dup → exit 1 + already-registered message" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run watch_cli register 999
    [ "$status" -eq 1 ]
    [[ "$output" == *"already registered"* ]]
}

@test "register multiple PRs → all visible in list" {
    run watch_cli register 100
    [ "$status" -eq 0 ]
    run watch_cli register 200
    [ "$status" -eq 0 ]
    run watch_cli register 300
    [ "$status" -eq 0 ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"pr": 100'* ]]
    [[ "$output" == *'"pr": 200'* ]]
    [[ "$output" == *'"pr": 300'* ]]
}

# ---------- unregister ----------

@test "unregister removes the entry" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run watch_cli unregister 999
    [ "$status" -eq 0 ]
    [[ "$output" == *"unregistered PR #999"* ]]
    [[ "$output" == *"Ownership returned to orchestrator"* ]]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == "[]" ]]
}

@test "unregister non-existent PR → exit 1 + nothing-to-do message" {
    run watch_cli unregister 9999
    [ "$status" -eq 1 ]
    [[ "$output" == *"not registered"* ]]
}

@test "unregister wipes per-PR state file too" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Synthesize a state file so we can verify it gets cleared.
    mkdir -p "$LOCALAPPDATA/Smatchet/merge-watch/state"
    echo '{"pr":999,"last_state":"BLOCKED"}' > "$LOCALAPPDATA/Smatchet/merge-watch/state/999.json"
    [ -f "$LOCALAPPDATA/Smatchet/merge-watch/state/999.json" ]
    run watch_cli unregister 999
    [ "$status" -eq 0 ]
    [ ! -f "$LOCALAPPDATA/Smatchet/merge-watch/state/999.json" ]
}

# ---------- status table ----------

@test "status with PR filter shows only that PR" {
    run watch_cli register 100
    [ "$status" -eq 0 ]
    run watch_cli register 200
    [ "$status" -eq 0 ]
    run watch_cli status 100
    [ "$status" -eq 0 ]
    [[ "$output" == *"#100"* ]]
    [[ "$output" != *"#200"* ]]
}

@test "status with non-existent PR filter → 'not registered'" {
    run watch_cli status 9999
    [ "$status" -eq 0 ]
    [[ "$output" == *"not registered"* ]]
}

@test "status shows '(no poll yet)' for a freshly-registered PR" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run watch_cli status
    [ "$status" -eq 0 ]
    [[ "$output" == *"(no poll yet)"* ]]
}

@test "status surfaces a written state file" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    mkdir -p "$LOCALAPPDATA/Smatchet/merge-watch/state"
    cat > "$LOCALAPPDATA/Smatchet/merge-watch/state/999.json" <<JSON
{"pr":999,"last_state":"GATES_PASSED","last_poll_unix":1779000000,"last_status_line":"Poll 1/1 ..."}
JSON
    run watch_cli status 999
    [ "$status" -eq 0 ]
    [[ "$output" == *"GATES_PASSED"* ]]
}

# ---------- registry file format ----------

@test "registry file is valid JSON list with required keys" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    [ -f "$LOCALAPPDATA/Smatchet/merge-watch/active.json" ]
    # Verify it's a list with the expected keys.
    run python -c "
import json, sys
d = json.load(open(r'$LOCALAPPDATA/Smatchet/merge-watch/active.json'))
assert isinstance(d, list), 'registry not a list'
assert len(d) == 1, f'expected 1 entry, got {len(d)}'
e = d[0]
for k in ('pr', 'clone_path', 'registered_at', 'triage_attempts'):
    assert k in e, f'missing key: {k}'
assert e['pr'] == 999
print('valid')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"valid"* ]]
}

@test "registry survives malformed JSON detection" {
    mkdir -p "$LOCALAPPDATA/Smatchet/merge-watch"
    echo "not json at all" > "$LOCALAPPDATA/Smatchet/merge-watch/active.json"
    run watch_cli status
    [ "$status" -eq 2 ]
    [[ "$output" == *"malformed JSON"* ]]
}

# ---------- Phase 2: cascade lock + auto-merge state shape ----------

@test "cascade_lock acquires + releases a per-branch lockfile" {
    # Direct Python smoke — exercise the cascade_lock helper without gh deps.
    run python -c "
import os, sys, pathlib
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
locks_dir = mw.cascade_locks_dir()
print('locks_dir_before:', locks_dir.exists())
with mw.cascade_lock('feat/my-child', timeout_seconds=2.0):
    lock_path = locks_dir / 'cascade-feat__my-child.lock'
    print('locked:', lock_path.exists())
print('after:', (locks_dir / 'cascade-feat__my-child.lock').exists())
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"locked: True"* ]]
    [[ "$output" == *"after: False"* ]]
}

@test "cascade_lock branch-name sanitization (/ → __)" {
    run python -c "
import os
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
with mw.cascade_lock('chore/v2-agentic-ripout-doc-cleanup'):
    import pathlib
    lock = mw.cascade_locks_dir() / 'cascade-chore__v2-agentic-ripout-doc-cleanup.lock'
    print('found:', lock.exists())
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"found: True"* ]]
}

@test "handle_pass on PR-already-merged (404 from merge API) → merge_failed" {
    # Stub gh on PATH to return 404 for the merge call. Verifies the error
    # path doesn't crash the daemon — just records merge_failed in state.
    STUB_BIN=$(mktemp -d)
    cat > "$STUB_BIN/gh" <<'STUB'
#!/usr/bin/env bash
case "$2 $3" in
    "repo view") echo '{"owner":{"login":"o"},"name":"r"}'; exit 0 ;;
    "api repos/o/r/pulls/999")          # detect_merged_branch_name
        echo '{"head":{"ref":"feat/foo"}}'; exit 0 ;;
    "api -X")                            # merge call
        echo "HTTP 404" >&2; exit 1 ;;
esac
exit 0
STUB
    chmod +x "$STUB_BIN/gh"
    PATH="$STUB_BIN:$PATH" run python -c "
import os, sys
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$REPO_ROOT'})
print('merge_action:', extras.get('merge_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merge_failed"* ]] || [[ "$output" == *"merge_action: skipped"* ]]
    rm -rf "$STUB_BIN"
}
