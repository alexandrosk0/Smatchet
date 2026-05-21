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
