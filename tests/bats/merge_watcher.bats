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

# ---------- Phase 3: CR-triage classifier ----------

@test "classifier parses CR review body + rejects string_view as invariant violation" {
    run python -c "
import sys, importlib.util
sys.modules['cr'] = sys.modules.get('cr', None)
spec = importlib.util.spec_from_file_location('cr', r'$SCRIPTS_DIR/coderabbit-triage.py')
m = importlib.util.module_from_spec(spec)
sys.modules['cr'] = m
spec.loader.exec_module(m)
body = '''In \`@Source_Core/src/X.cpp\`:\n- Around line 5: Use std::string_view instead of const std::string& for read-only parameters to avoid heap allocation.'''
findings = [m.classify_finding(f) for f in m.parse_findings(body)]
print('count:', len(findings))
print('verdict:', findings[0].verdict.value if findings else 'NONE')
print('reason:', findings[0].reason if findings else 'NONE')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"count: 1"* ]]
    [[ "$output" == *"verdict: REJECT_INVARIANT"* ]]
    [[ "$output" == *"string_view banned"* ]]
}

@test "classifier accepts a legit finding (no Smatchet rule violation)" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('cr', r'$SCRIPTS_DIR/coderabbit-triage.py')
m = importlib.util.module_from_spec(spec)
sys.modules['cr'] = m
spec.loader.exec_module(m)
body = '''In \`@Source_Core/src/X.cpp\`:\n- Around line 5: The function ignores the return value of fclose(), which can mask write errors on buffered streams. Capture the return + LOG_WARN if non-zero so silent data loss is visible.'''
findings = [m.classify_finding(f) for f in m.parse_findings(body)]
print('verdict:', findings[0].verdict.value if findings else 'NONE')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict: VALID"* ]]
}

@test "classifier flags too-short body as AMBIGUOUS" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('cr', r'$SCRIPTS_DIR/coderabbit-triage.py')
m = importlib.util.module_from_spec(spec)
sys.modules['cr'] = m
spec.loader.exec_module(m)
body = '''In \`@Source_Core/src/X.cpp\`:\n- Around line 5: short'''
findings = [m.classify_finding(f) for f in m.parse_findings(body)]
print('verdict:', findings[0].verdict.value if findings else 'NONE')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict: REJECT_AMBIGUOUS"* ]]
}

@test "classifier rejects structured bindings (banned by C++14 hard)" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('cr', r'$SCRIPTS_DIR/coderabbit-triage.py')
m = importlib.util.module_from_spec(spec)
sys.modules['cr'] = m
spec.loader.exec_module(m)
body = '''In \`@Source_Core/include/X.h\`:\n- Around line 5: Replace the manual loop with a structured binding for iteration to improve readability and avoid the auxiliary index variable.'''
findings = [m.classify_finding(f) for f in m.parse_findings(body)]
print('verdict:', findings[0].verdict.value if findings else 'NONE')
print('reason:', findings[0].reason if findings else 'NONE')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict: REJECT_INVARIANT"* ]]
    [[ "$output" == *"structured bindings"* ]]
}

@test "classifier rejects raw new/delete (RAII required)" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('cr', r'$SCRIPTS_DIR/coderabbit-triage.py')
m = importlib.util.module_from_spec(spec)
sys.modules['cr'] = m
spec.loader.exec_module(m)
body = '''In \`@Source_Core/src/Y.cpp\`:\n- Around line 5: Replace the std::unique_ptr usage with raw new Foo() for clarity — the explicit allocation makes ownership obvious.'''
findings = [m.classify_finding(f) for f in m.parse_findings(body)]
print('verdict:', findings[0].verdict.value if findings else 'NONE')
print('reason:', findings[0].reason if findings else 'NONE')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"verdict: REJECT_INVARIANT"* ]]
    [[ "$output" == *"raw new"* ]]
}

@test "_bump_triage_attempts increments registry counter" {
    # Register a PR first
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Bump via the daemon's helper
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec)
sys.modules['mw'] = m
spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 2)
"
    [ "$status" -eq 0 ]
    # Verify
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 2'* ]]
}

@test "_bump_triage_attempts persists triage_for_head_sha when given" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 3, 'feedface1234567890abcdef0987654321deadbe')
"
    [ "$status" -eq 0 ]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 3'* ]]
    [[ "$output" == *'"triage_for_head_sha": "feedface1234567890abcdef0987654321deadbe"'* ]]
}

@test "handle_blocked_cr_triage resets counter when HEAD moved since last triage" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Pre-populate: counter at budget, prior triage was on old HEAD.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 5, 'oldheadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
"
    [ "$status" -eq 0 ]
    # Now call handle_blocked_cr_triage with a CR-finding status line +
    # stub gh to return a NEW head_sha. Expect counter resets to 1.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'newheadbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'}
# Stub the subprocess that would invoke the triage classifier — return
# rc=0 so handle_blocked_cr_triage takes the success branch.
import subprocess
class FakeResult:
    returncode = 0
    stdout = 'ok'
    stderr = ''
subprocess.run = lambda *a, **kw: FakeResult()
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'triage_attempts': 5,
         'triage_for_head_sha': 'oldheadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'}
status_line = 'Poll 1/1 CodeRabbit: COMMENTED (2 actionable - block)'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    # Counter reset to 0 then bumped → triage_attempts: 1.
    [[ "$output" == *"'triage_attempts': 1"* ]]
    # Reset annotation present in extras (per-HEAD reset diagnostic).
    [[ "$output" == *"'triage_reset_on_head_change'"* ]]
    [[ "$output" == *"oldhead"* ]]
    [[ "$output" == *"newhead"* ]]
    # Registry persists the new head_sha for the next poll.
    run watch_cli list
    [[ "$output" == *'"triage_for_head_sha": "newheadbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"'* ]]
}

@test "handle_blocked_cr_triage preserves counter when HEAD unchanged (same head_sha)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 0, 'samehead1111111111111111111111111111111111')
"
    [ "$status" -eq 0 ]
    # Stub gh to return the SAME head_sha — counter must increment normally.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'samehead1111111111111111111111111111111111'}
import subprocess
class FakeResult:
    returncode = 0
    stdout = 'ok'
    stderr = ''
subprocess.run = lambda *a, **kw: FakeResult()
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'triage_attempts': 0,
         'triage_for_head_sha': 'samehead1111111111111111111111111111111111'}
status_line = 'Poll 1/1 CodeRabbit: COMMENTED (1 actionable - block)'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    # Counter went 0 → 1 (no reset, no annotation).
    [[ "$output" == *"'triage_attempts': 1"* ]]
    [[ "$output" != *"triage_reset_on_head_change"* ]]
}

@test "handle_blocked_cr_triage falls back to legacy counter when gh returns no head_sha" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 2, 'oldhead2222222222222222222222222222222222')
"
    [ "$status" -eq 0 ]
    # Stub gh to fail (RuntimeError) → fall back to legacy increment.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
def _failing_gh_json(args, **kw):
    raise RuntimeError('gh down')
m._gh_json = _failing_gh_json
import subprocess
class FakeResult:
    returncode = 0
    stdout = 'ok'
    stderr = ''
subprocess.run = lambda *a, **kw: FakeResult()
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'triage_attempts': 2,
         'triage_for_head_sha': 'oldhead2222222222222222222222222222222222'}
status_line = 'Poll 1/1 CodeRabbit: COMMENTED (1 actionable - block)'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    # No head_sha → no reset, counter goes 2 → 3 via legacy path.
    [[ "$output" == *"'triage_attempts': 3"* ]]
    [[ "$output" != *"triage_reset_on_head_change"* ]]
}

@test "_looks_like_cr_finding_block matches expected status-line shapes" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec)
sys.modules['mw'] = m
spec.loader.exec_module(m)
# Should match
assert m._looks_like_cr_finding_block('Poll 1/1 CodeRabbit: COMMENTED (3 actionable - block)')
assert m._looks_like_cr_finding_block('Poll 1/1 CodeRabbit: STALE_WITH_FINDINGS (5 actionable on prior commit - block)')
assert m._looks_like_cr_finding_block('Poll 1/1 CodeRabbit: CHANGES_REQUESTED')
# Should NOT match
assert not m._looks_like_cr_finding_block('Poll 1/1 CI: 5/8 pass (1 fail, 2 pending)')
assert not m._looks_like_cr_finding_block('Poll 1/1 CodeRabbit: NONE+grace-expired')
assert not m._looks_like_cr_finding_block('Poll 1/1 User: 2')
print('all assertions passed')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"all assertions passed"* ]]
}

# ---------- Option A: auto-act spawn-Claude-headless guards ----------

@test "maybe_auto_act: returns empty when MERGE_WATCH_AUTO_ACT unset (default off)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ.pop('MERGE_WATCH_AUTO_ACT', None)
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)'}
state = {'last_state': 'TRIAGE_BUDGET_EXHAUSTED',
         'last_status_line': 'Poll 1/1 CodeRabbit: STALE_WITH_FINDINGS (5 actionable on prior commit - block)'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}

@test "maybe_auto_act: returns empty on non-CR-finding state even when AUTO_ACT=true" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_AUTO_ACT'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CI: 4/5 pass (1 fail, 0 pending) | CodeRabbit: NONE+pending'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}

@test "maybe_auto_act: dedup suppresses repeat on same head_sha" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_AUTO_ACT'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
# Stub _gh_owner_repo + _gh_json so the function never reaches gh.
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'deadbeefcafebabe1234567890abcdef12345678'}
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'deadbeefcafebabe1234567890abcdef12345678'}
state = {'last_state': 'TRIAGE_BUDGET_EXHAUSTED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (3 actionable - block)'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"suppressed (already acted on this head_sha)"* ]]
}

@test "maybe_auto_act: refuses when claude binary missing" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_AUTO_ACT'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'aaaa1111bbbb2222cccc3333dddd4444eeee5555'}
m.shutil.which = lambda _n: None  # force claude-missing path
entry = {'pr': 999, 'clone_path': r'$(pwd)'}
state = {'last_state': 'TRIAGE_BUDGET_EXHAUSTED',
         'last_status_line': 'Poll 1/1 CodeRabbit: CHANGES_REQUESTED'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"skipped: claude binary not on PATH"* ]]
}

@test "maybe_auto_act: refuses when per-PR budget exhausted" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_AUTO_ACT'] = 'true'
os.environ['MERGE_WATCH_AUTO_ACT_BUDGET'] = '2'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'feedfacefeedfacefeedfacefeedfacefeedface'}
entry = {'pr': 999, 'clone_path': r'$(pwd)', 'auto_act_attempts': 2}
state = {'last_state': 'TRIAGE_BUDGET_EXHAUSTED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (1 actionable - block)'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"BUDGET_EXHAUSTED (2/2)"* ]]
}

# ---------- Triage budget default (option C: fast notify on CR findings) ----------

@test "MERGE_WATCH_TRIAGE_BUDGET default is 1 (was 3 — option C)" {
    # Triage retries don't fix code; they re-classify. Default lowered so the
    # notify surface fires on the next poll after CR posts findings, not three
    # polls later. Test reads the literal default from the source so a stray
    # bump back to 3 fails the gate.
    run python -c "
import re, pathlib
src = pathlib.Path(r'$SCRIPTS_DIR/merge-watcher.py').read_text(encoding='utf-8')
m = re.search(r'MERGE_WATCH_TRIAGE_BUDGET\", \"(\d+)\"', src)
assert m, 'budget default literal not found'
assert m.group(1) == '1', f'budget default is {m.group(1)}, expected 1'
print('budget default ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"budget default ok"* ]]
}

# ---------- Phase 4a: notify dispatch ----------

@test "smatchet-notify.sh --help prints inputs documentation" {
    run bash "$SCRIPTS_DIR/smatchet-notify.sh" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"PR number"* ]]
    [[ "$output" == *"CI_FAIL"* ]]
}

@test "smatchet-notify.sh missing --pr → exit 2" {
    run bash "$SCRIPTS_DIR/smatchet-notify.sh" --state CI_FAIL --message "test"
    [ "$status" -eq 2 ]
    [[ "$output" == *"required"* ]]
}

@test "smatchet-notify.sh unknown arg → exit 2" {
    run bash "$SCRIPTS_DIR/smatchet-notify.sh" --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown arg"* ]]
}

@test "smatchet-notify.sh with no Smatchet + no BurntToast → exit 1 + 'ALL channels failed'" {
    # POSIX bats; no Windows BurntToast available. HTTP POST will fail (no
    # server bound) + Windows branch may or may not fire depending on $OSTYPE.
    # Either way, with no toast targets up, the final dispatch fails.
    SMATCHET_NOTIFY_HOST=127.0.0.1 SMATCHET_NOTIFY_PORT=1 \
        run bash "$SCRIPTS_DIR/smatchet-notify.sh" --pr 999 --state CI_FAIL --message "bats test"
    # On non-Windows, exit=1 + ALL channels failed.
    # On Windows w/o BurntToast, also exit=1.
    [ "$status" -eq 1 ]
    [[ "$output" == *"ALL channels failed"* ]] || [[ "$output" == *"channels failed"* ]]
}

@test "NOTIFY_STATES contains the 6 expected terminal states" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
expected = {'CI_FAIL', 'GH_API_DOWN', 'PR_CLOSED_OR_MERGED', 'PAGINATION_OVERFLOW', 'TIMEOUT', 'TRIAGE_BUDGET_EXHAUSTED'}
assert m.NOTIFY_STATES == expected, f'got {m.NOTIFY_STATES}'
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "maybe_notify suppresses repeat-notify for the same state" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # First call — should attempt notify (notify_action key present)
    # Second call — should suppress
    run python -c "
import sys, os, importlib.util, json
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)

clone = r'$(pwd)'
entry = {'pr': 999, 'clone_path': clone, 'triage_attempts': 0}
state1 = {'last_state': 'CI_FAIL', 'last_status_line': 'first hit', 'pr': 999, 'clone_path': clone, 'last_poll_unix': 1, 'gates_return_code': 1}
extras1 = m.maybe_notify(state1, entry)
state1.update(extras1)
m.write_state(state1)
print('action1:', extras1.get('notify_action', 'NONE'))

# Second cycle with SAME state — suppression should fire.
state2 = {'last_state': 'CI_FAIL', 'last_status_line': 'second hit', 'pr': 999, 'clone_path': clone, 'last_poll_unix': 2, 'gates_return_code': 1}
extras2 = m.maybe_notify(state2, entry)
print('action2:', extras2.get('notify_action', 'NONE'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"action2: suppressed"* ]]
}

@test "maybe_notify on a non-terminal state returns empty extras (no notify)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)'}
state = {'last_state': 'GATES_PASSED', 'pr': 999, 'last_poll_unix': 1}
extras = m.maybe_notify(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}
