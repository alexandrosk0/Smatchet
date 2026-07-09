#!/usr/bin/env bats
# tests/bats/merge_watcher.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/merge-watcher-cli.py + merge-watcher.py.
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
#
# ----------------------------------------------------------------------------
# Windows / git-bash seams (shared by every gh-stubbing bats in tests/bats/):
#
#  1. gh.cmd forwarder. merge-watcher's _resolve_bin() uses shutil.which("gh"),
#     which on Windows honours PATHEXT and so resolves a real `gh.exe` / `gh.cmd`
#     in preference to an extensionless bash-script stub on PATH — the stub is
#     bypassed and the test hits live GitHub. A shebang script also can't be
#     subprocess.run() directly on Windows even by explicit path. Tests that
#     stub gh therefore `skip` under msys/cygwin/win OSTYPE (see the per-test
#     skip guards below); the cross-platform fix is a `gh.cmd` forwarder on PATH,
#     not an extensionless script (backlog: merge-watcher-gh-stub-windows).
#
#  2. cygpath -m LOCALAPPDATA. git-bash mktemp yields a driveless POSIX path
#     (/c/Users/...) that native Windows Python mis-resolves; `cygpath -m`
#     converts it to the mixed C:/... form (drive letter + forward slashes) that
#     BOTH native Python and git-bash filesystem tests accept, and which matches
#     `git rev-parse --show-toplevel`. POSIX has no cygpath and mktemp already
#     returns a valid path, so it is used verbatim there (see setup() below).
#
#  3. REST path-token routing. The gh stubs route on `case "$2 $3"` — the verb +
#     first path segment of the gh argv (`api repos/o/r/pulls/999`, `pr merge`,
#     `pr view`, `repo view`). When merge-watcher swaps a `gh api -X PUT
#     .../merge` REST call for a `gh pr merge --auto` invocation (or vice versa),
#     the stub's `case` arms must be updated to the new argv shape or the stub
#     silently falls through to `exit 0` and the assertion tests nothing.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    # Scripts-under-test live in THIS checkout (a linked worktree during local
    # dev) — load the modules from here.
    export SCRIPTS_DIR="$REPO_ROOT/agents/scripts/core"

    # CLONE_PATH = the clone_path `register` stores, which resolve_clone_path()
    # canonicalizes to the PRIMARY clone via `git rev-parse --git-common-dir`
    # (its parent dir). Tests that hardcode a clone_path to match a registered
    # entry MUST use CLONE_PATH, not REPO_ROOT: in a primary clone they're equal
    # (show-toplevel == git-common-dir parent), but from a LINKED WORKTREE they
    # diverge (REPO_ROOT = worktree, CLONE_PATH = main) — conflating them made
    # ~4 tests false-fail only under worktree dispatch (tooling.md
    # merge-watcher-bats-repo-root-conflates-script-path-and-clone-path). Derived
    # in git's mixed C:/... form (dirname of --git-common-dir) so it matches the
    # `replace(os.sep,"/")` form resolve_clone_path() writes.
    _common="$(git rev-parse --git-common-dir)"
    case "$_common" in /*|[A-Za-z]:/*) ;; *) _common="$REPO_ROOT/$_common" ;; esac
    CLONE_PATH="$(dirname "$_common")"
    export CLONE_PATH

    # Isolate per-user watcher state in a temp sandbox. watcher_root() in
    # merge-watcher-cli.py reads LOCALAPPDATA on Windows (os.name == "nt") and
    # XDG_STATE_HOME (else real $HOME/.local/state) on POSIX, so BOTH must point
    # into the temp dir — setting only LOCALAPPDATA leaks POSIX state into the
    # real home and reintroduces cross-test flake (CodeRabbit, PR #527).
    #
    # git-bash mktemp yields a driveless POSIX path (/c/Users/...); native
    # Windows Python — both the CLI subprocess and the in-process module under
    # test — mis-resolves that for open()/CreateProcess, so the registry write
    # and read land in different places (FileNotFoundError / WinError 267) and
    # clone_path stops matching the drive-qualified value `register` stores.
    # cygpath -m converts to a mixed C:/... form (drive letter + forward
    # slashes) that BOTH native Python and git-bash filesystem tests accept,
    # and which matches `git rev-parse --show-toplevel`. POSIX has no cygpath
    # and mktemp already returns a valid path, so use it verbatim there. Keep
    # the raw POSIX path for teardown's rm (git-bash rm wants the /c/... form).
    SMATCHET_TEST_TMP="$(mktemp -d)"
    if command -v cygpath >/dev/null 2>&1; then
        LOCALAPPDATA="$(cygpath -m "$SMATCHET_TEST_TMP")"
    else
        LOCALAPPDATA="$SMATCHET_TEST_TMP"
    fi
    # POSIX watcher root. On Windows the os.name == "nt" branch never reads
    # XDG_STATE_HOME, so the raw (driveless) value is inert there; on POSIX it
    # must be the native path, hence raw rather than cygpath -m.
    XDG_STATE_HOME="$SMATCHET_TEST_TMP"
    export LOCALAPPDATA XDG_STATE_HOME SMATCHET_TEST_TMP
    export PYTHONIOENCODING=utf-8

    # WATCH_ROOT = where watcher_root() actually lands per-OS: the LOCALAPPDATA
    # branch fires only under native Windows Python (os.name == "nt"); POSIX
    # uses $XDG_STATE_HOME/smatchet (lowercase). Tests asserting on registry /
    # state files MUST use this, not a hardcoded $LOCALAPPDATA/Smatchet path —
    # that conflation made 4 tests false-fail on Linux (headless CI lane).
    case "$OSTYPE" in
        msys*|cygwin*|win*) WATCH_ROOT="$LOCALAPPDATA/Smatchet/merge-watch" ;;
        *) WATCH_ROOT="$XDG_STATE_HOME/smatchet/merge-watch" ;;
    esac
    export WATCH_ROOT
}

teardown() {
    rm -rf "${SMATCHET_TEST_TMP:-}"
}

# ---------- helper ----------

watch_cli() {
    python "$SCRIPTS_DIR/merge-watcher-cli.py" "$@"
}

# ---------- cross-poll nudge/STALE persistence (registry-counter, mirrors cr_none_grace) ----------

@test "_parse_gate_carry extracts nudge_head/stale_head/stale_streak; None when absent" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
got = m._parse_gate_carry("Poll 1/1 — CI: ...\nGATE_CARRY nudge_head=abc stale_head=xyz stale_streak=5\n")
assert got == {"nudged_head": "abc", "stale_head": "xyz", "stale_streak": 5}, got
assert m._parse_gate_carry("no carry line here") is None
g2 = m._parse_gate_carry("GATE_CARRY nudge_head= stale_head= stale_streak=0")
assert g2 == {"nudged_head": "", "stale_head": "", "stale_streak": 0}, g2
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "_parse_gate_snapshot extracts downgraded names + cr_override; None when absent" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

# CI downgrade names contain spaces + commas (jq join(", ")) — must survive intact.
got = m._parse_gate_snapshot(
    "Poll 1/1 — CI: ...\n"
    "GATE_SNAPSHOT cr_override=0 downgraded=Test-delta gate, Perf PR-fast (ubuntu)\n"
    "GATES_PASSED\n"
)
assert got == {"downgraded": ["Test-delta gate", "Perf PR-fast (ubuntu)"], "cr_override": False}, got

# cr-out-of-band waived a CR block, no CI downgrade → empty downgraded, cr_override True.
g2 = m._parse_gate_snapshot("GATE_SNAPSHOT cr_override=1 downgraded=")
assert g2 == {"downgraded": [], "cr_override": True}, g2

# Clean pass (label present but moot / nothing downgraded) → empty + False.
g3 = m._parse_gate_snapshot("GATE_SNAPSHOT cr_override=0 downgraded=")
assert g3 == {"downgraded": [], "cr_override": False}, g3

# No GATE_SNAPSHOT line at all → None (caller falls back to redChecks=[]).
assert m._parse_gate_snapshot("Poll 1/1 — CI: ...\nGATES_PASSED\n") is None
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "handle_pass with override gate_snapshot writes merge-snapshot redChecks naming bypassed checks" {
    # An override-label merge (tests-out-of-band + cr-out-of-band) must write a
    # lossless ledger row whose redChecks names what the override bypassed
    # (mandatory-merge-snapshot-on-override-merge). gate_snapshot carries the
    # downgraded CI check + the cr_override flag; the row's overrideLabels carry
    # the labels present on the PR.
    LEDGER="$SMATCHET_TEST_TMP/merge-snapshots.jsonl"
    # Monkeypatch the Python gh-seams directly (portable: no extensionless-stub
    # exec, which Windows native python can't run). _append_merge_snapshot still
    # shells out to the REAL merge-snapshot-append.sh writing to $LEDGER — that
    # path (the unit under test) is exercised for real.
    run python -c "
import os
os.environ['MERGE_SNAPSHOT_LEDGER'] = r'$LEDGER'
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
# gh seams: owner/repo, merge (no-op), branch detect, stacked children none.
mw._gh_owner_repo = lambda clone_path: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: True
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/foo'
mw.squash_merge_pr = lambda o, r, pr: 'abc123def456'
mw.find_stacked_children = lambda o, r, b: []
mw.maybe_remove_from_registry = lambda pr, cp: None
# _append_merge_snapshot reads labels+headRefOid via _gh_json('pr view').
mw._gh_json = lambda args, **kw: {'headRefOid': 'head789', 'labels': [
    {'name': 'tests-out-of-band'}, {'name': 'cr-out-of-band'}, {'name': 'unrelated'}]}
gs = {'downgraded': ['Test-delta gate'], 'cr_override': True}
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'}, gate_snapshot=gs)
print('merge_action:', extras.get('merge_action'))
print('snapshot:', extras.get('merge_snapshot'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merged"* ]]
    [[ "$output" == *"snapshot_appended"* ]]
    # Exactly one ledger row, naming both bypassed checks + the override labels.
    # `unrelated` is a non-override label and must NOT appear in overrideLabels.
    [ -f "$LEDGER" ]
    run jq -e '.pr == 999 and (.redChecks | index("Test-delta gate")) != null and (.redChecks | index("CodeRabbit")) != null and (.overrideLabels | index("tests-out-of-band")) != null and (.overrideLabels | index("cr-out-of-band")) != null and (.overrideLabels | index("unrelated")) == null' "$LEDGER"
    [ "$status" -eq 0 ]
    run bash -c "wc -l < '$LEDGER'"
    [ "$output" -eq 1 ]
}

@test "handle_pass clean merge (no gate_snapshot) writes empty redChecks, single row, no double-write" {
    # A clean (no-override) merge must still write a row but with redChecks=[]
    # so postmortem-owed never spuriously flags it. Idempotency: handle_pass'd
    # twice for the same pr+mergeCommit writes exactly one line.
    LEDGER="$SMATCHET_TEST_TMP/merge-snapshots-clean.jsonl"
    run python -c "
import os
os.environ['MERGE_SNAPSHOT_LEDGER'] = r'$LEDGER'
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
mw._gh_owner_repo = lambda clone_path: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: True
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/foo'
mw.squash_merge_pr = lambda o, r, pr: 'clean99'
mw.find_stacked_children = lambda o, r, b: []
mw.maybe_remove_from_registry = lambda pr, cp: None
# Clean merge: no override labels present.
mw._gh_json = lambda args, **kw: {'headRefOid': 'head000', 'labels': []}
e1 = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'}, gate_snapshot=None)
e2 = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'}, gate_snapshot=None)
print('s1:', e1.get('merge_snapshot'))
print('s2:', e2.get('merge_snapshot'))
"
    [ "$status" -eq 0 ]
    [ -f "$LEDGER" ]
    run jq -e '.redChecks == [] and .overrideLabels == []' "$LEDGER"
    [ "$status" -eq 0 ]
    # Idempotent: two handle_pass calls, same pr+mergeCommit → one line only.
    run bash -c "wc -l < '$LEDGER'"
    [ "$output" -eq 1 ]
}

@test "_bump_nudge_state persists nudged_head/stale_head/stale_streak into the registry entry" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Single self-contained block: load the module, read the registered entry's
    # clone_path via its own reader, bump, re-read, assert the round-trip.
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
entries = m._CLI.read_registry()
assert entries, "no registry entries after register"
clone = entries[0]["clone_path"]
m._bump_nudge_state(999, clone, "headSHA999", "headSHA999", 4)
e = m._CLI.read_registry()[0]
assert e["nudged_head"] == "headSHA999", e
assert e["stale_head"] == "headSHA999", e
assert e["stale_streak"] == 4, e
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

# ---------- empty-state ----------

@test "status on empty registry -> 'registry empty'" {
    run watch_cli status
    [ "$status" -eq 0 ]
    [[ "$output" == *"registry empty"* ]]
}

@test "list on empty registry -> empty JSON array" {
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

@test "register dup -> exit 1 + already-registered message" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run watch_cli register 999
    [ "$status" -eq 1 ]
    [[ "$output" == *"already registered"* ]]
}

@test "register multiple PRs -> all visible in list" {
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

@test "register from a linked worktree canonicalizes onto the main clone (no orphan dupe) (tooling.md :28)" {
    # A throwaway main repo + a linked worktree off it.
    local main="$SMATCHET_TEST_TMP/mainrepo"
    git init -q -b develop "$main"
    git -C "$main" -c user.email=t@t -c user.name=t commit --allow-empty -q -m seed
    local wt="$SMATCHET_TEST_TMP/wt"
    git -C "$main" worktree add -q -b feat "$wt" >/dev/null 2>&1

    # Register from the WORKTREE cwd — must store the MAIN clone path.
    run bash -c "cd '$wt' && python '$SCRIPTS_DIR/merge-watcher-cli.py' register 4242"
    [ "$status" -eq 0 ]

    # Registering the SAME PR from the MAIN clone is now a DUP (same canonical
    # key) — proves the worktree registration landed on the main clone, not on
    # the ephemeral worktree path.
    run bash -c "cd '$main' && python '$SCRIPTS_DIR/merge-watcher-cli.py' register 4242"
    [ "$status" -eq 1 ]
    [[ "$output" == *"already registered"* ]]

    # Exactly one entry, keyed at the main clone (basename mainrepo), not the wt.
    run watch_cli list
    [ "$status" -eq 0 ]
    [ "$(printf '%s' "$output" | grep -c '"pr": 4242')" -eq 1 ]
    [[ "$output" == *"mainrepo"* ]]
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

@test "unregister non-existent PR -> exit 1 + nothing-to-do message" {
    run watch_cli unregister 9999
    [ "$status" -eq 1 ]
    [[ "$output" == *"not registered"* ]]
}

@test "unregister wipes per-PR state file too" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Synthesize a state file so we can verify it gets cleared.
    mkdir -p "$WATCH_ROOT/state"
    echo '{"pr":999,"last_state":"BLOCKED"}' > "$WATCH_ROOT/state/999.json"
    [ -f "$WATCH_ROOT/state/999.json" ]
    run watch_cli unregister 999
    [ "$status" -eq 0 ]
    [ ! -f "$WATCH_ROOT/state/999.json" ]
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

@test "status with non-existent PR filter -> 'not registered'" {
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
    mkdir -p "$WATCH_ROOT/state"
    cat > "$WATCH_ROOT/state/999.json" <<JSON
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
    [ -f "$WATCH_ROOT/active.json" ]
    # Verify it's a list with the expected keys.
    run python -c "
import json, sys
d = json.load(open(r'$WATCH_ROOT/active.json'))
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
    mkdir -p "$WATCH_ROOT"
    echo "not json at all" > "$WATCH_ROOT/active.json"
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

@test "cascade_lock branch-name sanitization (/ -> __)" {
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

@test "handle_pass on PR-already-merged (gh pr merge fails) -> merge_failed" {
    # Stub gh on PATH to fail the `gh pr merge --auto` call. Verifies the error
    # path doesn't crash the daemon — just records merge_failed in state.
    # (squash_merge_pr now enables auto-merge via `gh pr merge --squash --auto`
    # for merge-queue safety, replacing the former `gh api -X PUT .../merge`.)
    STUB_BIN=$(mktemp -d)
    cat > "$STUB_BIN/gh" <<'STUB'
#!/usr/bin/env bash
case "$2 $3" in
    "repo view") echo '{"owner":{"login":"o"},"name":"r"}'; exit 0 ;;
    "api repos/o/r/pulls/999")          # detect_merged_branch_name
        echo '{"head":{"ref":"feat/foo"}}'; exit 0 ;;
    "pr merge")                          # auto-merge enable call
        echo "failed to merge: Pull Request is not mergeable" >&2; exit 1 ;;
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
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'})
print('merge_action:', extras.get('merge_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merge_failed"* ]] || [[ "$output" == *"merge_action: skipped"* ]]
    rm -rf "$STUB_BIN"
}

@test "handle_pass enqueues on a merge queue (state OPEN after --auto) -> merge_action: enqueued" {
    # Cross-platform: monkeypatch the Python gh-seams directly (same idiom as the
    # passing handle_pass override/clean tests above) instead of an extensionless
    # bash `gh` stub on PATH. The stub approach was Windows-unresolvable —
    # _resolve_bin()'s shutil.which("gh") skips a name with no PATHEXT extension
    # and a shebang script can't be subprocess.run() directly on Windows — so the
    # two queue tests were Windows-skipped (backlog merge-watcher-gh-stub-windows).
    # Patching squash_merge_pr to return ENQUEUED_SENTINEL exercises the same
    # handle_pass decision path (enqueued → no cascade/snapshot/registry-drop) on
    # every host, launching no subprocess. Backlog item resolved.
    # docs/plans/active/build-quality-velocity-hardening.md #14 path B.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
mw._gh_owner_repo = lambda clone_path: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: True
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/foo'
# Merge-queue-safe path: gh pr merge --auto enqueues, PR stays OPEN →
# squash_merge_pr returns ENQUEUED_SENTINEL → handle_pass records 'enqueued'.
mw.squash_merge_pr = lambda o, r, pr: mw.ENQUEUED_SENTINEL
# These must NOT be reached on the enqueued early-return; trip loudly if they are.
mw.find_stacked_children = lambda o, r, b: (_ for _ in ()).throw(AssertionError('cascade reached on enqueue'))
mw._append_merge_snapshot = lambda *a, **k: (_ for _ in ()).throw(AssertionError('snapshot reached on enqueue'))
mw.maybe_remove_from_registry = lambda pr, cp: (_ for _ in ()).throw(AssertionError('registry-drop reached on enqueue'))
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'})
print('merge_action:', extras.get('merge_action'))
print('merge_sha:', extras.get('merge_sha', '<none>'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: enqueued"* ]]
    [[ "$output" == *"merge_sha: <none>"* ]]
}

@test "handle_pass merges immediately when no queue (state MERGED after --auto) -> merge_action: merged" {
    # Cross-platform — same monkeypatch idiom as the enqueued test above (was
    # Windows-skipped under the unresolvable extensionless gh bash stub; backlog
    # merge-watcher-gh-stub-windows). No merge queue: squash_merge_pr returns a
    # real SHA → handle_pass proceeds to merged + snapshot + cascade. Patch
    # _append_merge_snapshot + find_stacked_children so no gh subprocess launches
    # (mirrors the line-822 cascade test); the merged decision path is exercised.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mw)
mw._gh_owner_repo = lambda clone_path: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: True
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/foo'
# No queue: immediate merge → squash_merge_pr returns the merge-commit SHA.
mw.squash_merge_pr = lambda o, r, pr: 'abc123def456'
mw._append_merge_snapshot = lambda o, r, pr, sha, gate_snapshot=None: 'snapshot_skipped'
mw.maybe_remove_from_registry = lambda pr, cp: None
mw.find_stacked_children = lambda o, r, b: []
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'})
print('merge_action:', extras.get('merge_action'))
print('merge_sha:', extras.get('merge_sha', '<none>'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merged"* ]]
    [[ "$output" == *"merge_sha: abc123def456"* ]]
}

@test "gh-json: invalid cwd raises RuntimeError (not OSError) so _gh_owner_repo degrades to None" {
    # Regression: a stale/moved registered clone_path (or a driveless POSIX path
    # on Windows) makes subprocess cwd= raise NotADirectoryError [WinError 267] /
    # FileNotFoundError. _gh_json must normalize that to RuntimeError so callers'
    # `except RuntimeError` (e.g. _gh_owner_repo, maybe_notify) catch it instead
    # of crashing the daemon poll.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
bad = '/c/nonexistent-driveless-clone-path-xyz'
try:
    mw._gh_json(['repo', 'view', '--json', 'owner,name'], cwd=bad)
    print('NO_EXCEPTION')
except RuntimeError:
    print('runtimeerror')
except OSError as e:
    print('leaked_oserror:', e)
print('owner_repo:', mw._gh_owner_repo(bad))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"runtimeerror"* ]]
    [[ "$output" != *"leaked_oserror"* ]]
    [[ "$output" == *"owner_repo: None"* ]]
}

# ---------- daemon-crash resilience: squash timeout + per-PR backstop ----------

@test "squash_merge_pr normalizes a gh launch/timeout into RuntimeError (not TimeoutExpired) - daemon-crash guard" {
    # Regression (infra-outage): the `gh pr merge --auto` subprocess.run carries
    # timeout=60. A bare subprocess.TimeoutExpired is NOT a RuntimeError, so it
    # would escape handle_pass (`except RuntimeError`), unwind daemon_loop's
    # per-PR body past `except StopSignal`, and crash the whole daemon — stranding
    # every registered PR. squash_merge_pr must normalize launch/timeout failures
    # to RuntimeError, mirroring _gh_json.
    run python -c "
import importlib.util, subprocess
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
def boom(*a, **k):
    raise subprocess.TimeoutExpired(cmd='gh pr merge', timeout=60)
mw.subprocess.run = boom
try:
    mw.squash_merge_pr('o', 'r', 999)
    print('NO_EXCEPTION')
except RuntimeError as e:
    print('runtimeerror:', 'timed out' in str(e))
except subprocess.TimeoutExpired:
    print('leaked_timeout')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"runtimeerror: True"* ]]
    [[ "$output" != *"leaked_timeout"* ]]
    [[ "$output" != *"NO_EXCEPTION"* ]]
}

@test "handle_pass degrades to merge_failed when squash_merge_pr times out (no crash escape)" {
    # End-to-end: a gh-merge timeout flows squash_merge_pr -> RuntimeError ->
    # handle_pass `except RuntimeError` -> merge_failed extras. handle_pass must
    # return cleanly (the daemon records merge_failed + retries next cycle), never
    # let a TimeoutExpired propagate up the per-PR loop.
    run python -c "
import importlib.util, subprocess
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_owner_repo = lambda cp: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: None
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/foo'
def boom(*a, **k):
    raise subprocess.TimeoutExpired(cmd='gh pr merge', timeout=60)
mw.subprocess.run = boom
extras = mw.handle_pass({'pr': 999, 'clone_path': r'$CLONE_PATH'})
print('merge_action:', extras.get('merge_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merge_failed"* ]]
    [[ "$output" == *"timed out"* ]]
}

@test "daemon_loop per-PR backstop: a transient exception in one PR is logged + the loop continues to the next PR" {
    # Layer-2 structural fix: process_registered_pr is wrapped per-iteration so
    # one PR's unexpected raise degrades to a retry instead of crashing the whole
    # daemon. Both registered PRs must be attempted; daemon_loop returns 0.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
seen = []
def fake_proc(entry):
    seen.append(entry['pr'])
    raise ValueError('simulated transient gh failure')
mw.process_registered_pr = fake_proc
mw.read_registry = lambda: [{'pr': 901, 'clone_path': 'x'}, {'pr': 902, 'clone_path': 'x'}]
mw.write_pid_file = lambda: None
mw.clear_pid_file = lambda: None
def stop(_):
    raise mw.StopSignal()
mw.time.sleep = stop
rc = mw.daemon_loop(0)
print('rc:', rc)
print('seen:', seen)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rc: 0"* ]]
    [[ "$output" == *"seen: [901, 902]"* ]]
    [[ "$output" == *"WARN: PR#901 poll cycle raised ValueError"* ]]
    [[ "$output" == *"WARN: PR#902 poll cycle raised ValueError"* ]]
}

@test "daemon_loop per-PR backstop re-raises StopSignal (clean shutdown not swallowed by the broad except)" {
    # Critical ordering guard: StopSignal is an Exception subclass raised from the
    # SIGINT/SIGTERM handler. The per-PR `except StopSignal: raise` MUST precede
    # `except Exception`, or Ctrl-C during a PR's processing gets swallowed and the
    # daemon never stops. If StopSignal raised in PR 901 propagates correctly, PR
    # 902 is never reached (seen == [901]); if it were swallowed, both would run.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
seen = []
def fake_proc(entry):
    seen.append(entry['pr'])
    raise mw.StopSignal()
mw.process_registered_pr = fake_proc
mw.read_registry = lambda: [{'pr': 901, 'clone_path': 'x'}, {'pr': 902, 'clone_path': 'x'}]
mw.write_pid_file = lambda: None
mw.clear_pid_file = lambda: None
def stop(_):
    raise mw.StopSignal()
mw.time.sleep = stop
rc = mw.daemon_loop(0)
print('rc:', rc)
print('seen:', seen)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rc: 0"* ]]
    [[ "$output" == *"seen: [901]"* ]]
}

@test "daemon_loop per-CYCLE backstop: a cycle-scope read_registry raise is logged + the daemon continues (not a whole-daemon crash)" {
    # The per-PR backstop only wraps process_registered_pr; read_registry runs at
    # cycle scope OUTSIDE it. A malformed/locked registry file makes read_registry
    # raise a bare RuntimeError (or read_text OSError) — without the per-cycle
    # backstop that escapes the StopSignal-only outer handler into the catch-less
    # main and crashes the WHOLE daemon, stranding every registered PR. With the
    # fix it degrades to a logged skip-this-cycle and the daemon stays up.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
proc_called = []
mw.process_registered_pr = lambda entry: proc_called.append(entry)
def bad_registry():
    raise RuntimeError('malformed registry: not a list')
mw.read_registry = bad_registry
mw.write_pid_file = lambda: None
mw.clear_pid_file = lambda: None
def stop(_):
    raise mw.StopSignal()
mw.time.sleep = stop
rc = mw.daemon_loop(0)
print('rc:', rc)
print('proc_called:', len(proc_called))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rc: 0"* ]]
    [[ "$output" == *"WARN: cycle raised RuntimeError"* ]]
    [[ "$output" == *"stop signal received"* ]]
    [[ "$output" == *"proc_called: 0"* ]]
}

@test "daemon_loop per-CYCLE backstop re-raises StopSignal (clean shutdown during read_registry not swallowed)" {
    # Ordering guard for the per-cycle backstop: a signal arriving DURING
    # read_registry raises StopSignal at cycle scope. The per-cycle
    # `except StopSignal: raise` MUST precede `except Exception`, or the broad
    # backstop swallows it into a 'cycle raised StopSignal' WARN and the daemon
    # never stops. Correct ordering re-raises straight to the outer handler with
    # NO per-cycle WARN; a swapped order would log the WARN.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
def signal_mid_read():
    raise mw.StopSignal()
mw.read_registry = signal_mid_read
mw.write_pid_file = lambda: None
mw.clear_pid_file = lambda: None
def stop(_):
    raise mw.StopSignal()
mw.time.sleep = stop
rc = mw.daemon_loop(0)
print('rc:', rc)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rc: 0"* ]]
    [[ "$output" == *"stop signal received"* ]]
    [[ "$output" != *"cycle raised StopSignal"* ]]
}

@test "handle_pass cascade: a hung child's subprocess.TimeoutExpired degrades to a per-child ERR + the loop continues to siblings (no escape)" {
    # Same trap-class as the squash timeout, one layer down: cascade_update_child's
    # `gh api PUT update-branch` runs subprocess.run(timeout=30) un-normalized, and
    # the cascade loop's guard used to be `except TimeoutError` only. subprocess.
    # TimeoutExpired is a subprocess.SubprocessError, NOT a builtins.TimeoutError, so
    # a hung child would slip that narrow except, unwind out of handle_pass MID-cascade
    # (post-merge), and only be coarsely caught by the L2 per-PR backstop — silently
    # dropping update-branch dispatch to every sibling after the hung one. The widened
    # `except (TimeoutError, OSError, subprocess.SubprocessError)` must degrade the hung
    # child to ok=False and STILL dispatch to the next sibling (proving loop-continue).
    run python -c "
import importlib.util, subprocess, contextlib
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_owner_repo = lambda cp: ('o', 'r')
mw.ensure_pr_ready_for_review = lambda o, r, pr: None
mw.detect_merged_branch_name = lambda o, r, pr: 'feat/parent'
mw.squash_merge_pr = lambda o, r, pr: 'sha123abc'
mw._append_merge_snapshot = lambda o, r, pr, sha, gate_snapshot=None: 'snapshot_skipped'
mw.maybe_remove_from_registry = lambda pr, cp: None
mw.find_stacked_children = lambda o, r, b: [
    {'number': 801, 'headRefName': 'feat/child-a'},
    {'number': 802, 'headRefName': 'feat/child-b'},
]
mw.cascade_lock = lambda head, **k: contextlib.nullcontext()
def cascade(o, r, child_pr):
    if child_pr == 801:
        raise subprocess.TimeoutExpired(cmd='gh api PUT update-branch', timeout=30)
    return True, 'update-branch dispatched'
mw.cascade_update_child = cascade
extras = mw.handle_pass({'pr': 999, 'clone_path': 'x'})
print('merge_action:', extras.get('merge_action'))
kids = extras.get('cascade_children', [])
print('child_count:', len(kids))
for k in kids:
    print('child:', k['pr'], k['ok'], k['msg'])
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge_action: merged"* ]]
    [[ "$output" == *"child_count: 2"* ]]
    # First child's subprocess timeout degraded to a per-child ERR (not an escape).
    [[ "$output" == *"child: 801 False TimeoutExpired"* ]]
    # The sibling after the hung child was STILL reached — the loop continued.
    [[ "$output" == *"child: 802 True update-branch dispatched"* ]]
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
body = '''In \`@Source/Core/src/X.cpp\`:\n- Around line 5: Use std::string_view instead of const std::string& for read-only parameters to avoid heap allocation.'''
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
body = '''In \`@Source/Core/src/X.cpp\`:\n- Around line 5: The function ignores the return value of fclose(), which can mask write errors on buffered streams. Capture the return + LOG_WARN if non-zero so silent data loss is visible.'''
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
body = '''In \`@Source/Core/src/X.cpp\`:\n- Around line 5: short'''
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
body = '''In \`@Source/Core/include/X.h\`:\n- Around line 5: Replace the manual loop with a structured binding for iteration to improve readability and avoid the auxiliary index variable.'''
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
body = '''In \`@Source/Core/src/Y.cpp\`:\n- Around line 5: Replace the std::unique_ptr usage with raw new Foo() for clarity — the explicit allocation makes ownership obvious.'''
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
m._bump_triage_attempts(999, r'$CLONE_PATH', 2)
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
m._bump_triage_attempts(999, r'$CLONE_PATH', 3, 'feedface1234567890abcdef0987654321deadbe')
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
m._bump_triage_attempts(999, r'$CLONE_PATH', 5, 'oldheadaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
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
entry = {'pr': 999, 'clone_path': r'$CLONE_PATH',
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
    [ "$status" -eq 0 ]
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
import os
# Pin budget > pre-populated count so the same-head clamp (attempts_before > budget)
# does NOT fire — this test exercises the legacy fall-back INCREMENT path (2 -> 3
# when gh returns no head_sha), not the BUDGET_EXHAUSTED early-return. The default
# budget is 1, which would exhaust at 2 and short-circuit before the legacy path.
os.environ['MERGE_WATCH_TRIAGE_BUDGET'] = '3'
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

# ---------- PR-3: triage-attempts clamp (merge-watcher-triage-attempts-unbounded) ----------

@test "handle_blocked_cr_triage clamps: same-head re-poll while EXHAUSTED does NOT increment triage_attempts" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Pre-populate: counter already at budget+1 (exhausted, budget default 1 → clamp 2),
    # pinned to a head_sha. A same-head re-poll must early-return WITHOUT bumping.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$CLONE_PATH', 2, 'sameheadcccccccccccccccccccccccccccccccc')
"
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
# Same head_sha as the pre-populated counter — no per-HEAD reset.
m._gh_json = lambda args, **kw: {'headRefOid': 'sameheadcccccccccccccccccccccccccccccccc'}
# If the clamp leaks, this would invoke the triage classifier subprocess; fail loudly.
import subprocess
def _boom(*a, **kw):
    raise AssertionError('triage classifier subprocess must NOT run while exhausted')
subprocess.run = _boom
entry = {'pr': 999, 'clone_path': r'$CLONE_PATH',
         'triage_attempts': 2,
         'triage_for_head_sha': 'sameheadcccccccccccccccccccccccccccccccc'}
status_line = 'Poll 1/1 CodeRabbit: COMMENTED (3 actionable - block)'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    # State surfaces EXHAUSTED, attempts stay clamped at 2 (budget+1), no further bump.
    [[ "$output" == *"'triage_attempts': 2"* ]]
    [[ "$output" == *"TRIAGE_BUDGET_EXHAUSTED"* ]]
    # Registry counter was NOT incremented (still 2, not 3).
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 2'* ]]
    [[ "$output" != *'"triage_attempts": 3'* ]]
}

@test "handle_blocked_cr_triage still bumps to budget+1 on the FIRST exhausting poll (clamp is at budget+1, not budget)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Counter at budget (1), same head → this poll bumps to 2 (budget+1) and EXHAUSTS.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$CLONE_PATH', 1, 'sameheaddddddddddddddddddddddddddddddddd')
"
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._gh_json = lambda args, **kw: {'headRefOid': 'sameheaddddddddddddddddddddddddddddddddd'}
import subprocess
class FakeResult:
    returncode = 0; stdout = 'ok'; stderr = ''
subprocess.run = lambda *a, **kw: FakeResult()
entry = {'pr': 999, 'clone_path': r'$CLONE_PATH',
         'triage_attempts': 1,
         'triage_for_head_sha': 'sameheaddddddddddddddddddddddddddddddddd'}
extras = m.handle_blocked_cr_triage(entry, 'Poll 1/1 CodeRabbit: COMMENTED (1 actionable - block)')
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"'triage_attempts': 2"* ]]
    [[ "$output" == *"TRIAGE_BUDGET_EXHAUSTED"* ]]
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *'"triage_attempts": 2'* ]]
}

# ---------- PR-3: agent-event sink (merge-watcher-agent-notify) ----------

@test "append_agent_event + maybe_emit_agent_event write one NDJSON line; suppress same-state re-emit" {
    run python -c "
import sys, importlib.util, json
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 4242, 'clone_path': r'$CLONE_PATH'}
state = {'pr': 4242, 'last_state': 'STUCK_NEEDS_ATTENTION',
         'last_status_line': 'wedged', 'stuck_reason': 'BEHIND'}
extras = m.maybe_emit_agent_event(state, entry)
assert extras.get('agent_event_action','').startswith('emitted'), extras
sink = m.agent_events_path()
lines = [l for l in sink.read_text(encoding='utf-8').splitlines() if l.strip()]
assert len(lines) == 1, lines
ev = json.loads(lines[0])
assert ev['pr'] == 4242 and ev['state'] == 'STUCK_NEEDS_ATTENTION' and ev['stuck_reason'] == 'BEHIND', ev
# Write the prior-state file so the suppression key is present, then re-emit same state.
m.state_dir().mkdir(parents=True, exist_ok=True)
import pathlib
(m.state_dir()/'4242.json').write_text(json.dumps({'agent_event_emitted_for_state':'STUCK_NEEDS_ATTENTION'}), encoding='utf-8')
extras2 = m.maybe_emit_agent_event(state, entry)
assert extras2.get('agent_event_action') == 'suppressed (same state as last event)', extras2
lines2 = [l for l in sink.read_text(encoding='utf-8').splitlines() if l.strip()]
assert len(lines2) == 1, lines2  # no second line
# A non-event state writes nothing.
assert m.maybe_emit_agent_event({'pr':4242,'last_state':'BLOCKED'}, entry) == {}
print('OK')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "merge-watcher-cli await returns the matching event for a PR; --until filters BLOCKED vs terminal" {
    # Seed the sink with a GATES_PASSED (terminal) event for PR 555.
    run python -c "
import sys, importlib.util, json
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m.append_agent_event({'ts_unix':1,'pr':555,'state':'GATES_PASSED','status_line':'merged','source':'merge-watcher'})
print(m.agent_events_path())
"
    [ "$status" -eq 0 ]
    # await --until terminal returns immediately... BUT await ignores pre-existing
    # events (records start offset). So with a fresh appended-after-start model the
    # seeded line is BEFORE start; a --timeout makes it return 124. Verify the
    # blocking/terminal classification via a direct call instead (deterministic).
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('cli', r'$SCRIPTS_DIR/merge-watcher-cli.py')
m = importlib.util.module_from_spec(spec); sys.modules['cli']=m; spec.loader.exec_module(m)
# A GATES_PASSED already in the sink BEFORE await starts is not matched (offset model)
# → with a short timeout, await exits 124. Confirms await does not return on stale events.
import argparse
ns = argparse.Namespace(pr='555', until='terminal', timeout=1.0)
rc = m.cmd_await(ns)
assert rc == 124, rc
print('OK timeout-on-stale')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK timeout-on-stale"* ]]
}

@test "merge-watcher-cli ledger-guard: clean ledger exits 0; uncommitted ledger rows exit 1" {
    # Build a throwaway git repo with the ledger path.
    GTMP="$(mktemp -d)"
    if command -v cygpath >/dev/null 2>&1; then GTMP_M="$(cygpath -m "$GTMP")"; else GTMP_M="$GTMP"; fi
    git -C "$GTMP" init -q
    git -C "$GTMP" config user.email t@t.t
    git -C "$GTMP" config user.name t
    mkdir -p "$GTMP/docs/self-improvement"
    echo '{"pr":1,"mergeCommit":"a"}' > "$GTMP/docs/self-improvement/merge-snapshots.jsonl"
    git -C "$GTMP" add -A
    git -C "$GTMP" commit -qm init
    # Clean tree → exit 0.
    run python "$SCRIPTS_DIR/merge-watcher-cli.py" ledger-guard --clone-path "$GTMP_M"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ledger clean"* ]]
    # Append an uncommitted row → exit 1, names the harvest action.
    echo '{"pr":2,"mergeCommit":"b"}' >> "$GTMP/docs/self-improvement/merge-snapshots.jsonl"
    run python "$SCRIPTS_DIR/merge-watcher-cli.py" ledger-guard --clone-path "$GTMP_M"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING"* ]]
    [[ "$output" == *"UNCOMMITTED"* ]]
    rm -rf "$GTMP"
}

@test "ledger_has_uncommitted_rows fail-closes (dirty=True) on a non-git path" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
import tempfile, os
d = tempfile.mkdtemp()  # not a git repo
dirty, detail = m.ledger_has_uncommitted_rows(d)
assert dirty is True, (dirty, detail)
print('OK fail-closed:', detail)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK fail-closed"* ]]
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
# The claude-on-PATH / clean-clone / zero-live-sessions gates run BEFORE the
# dedup check; stub all three (as the budget test below does) so the dedup
# branch is reached regardless of host PATH, tree state, or live sessions.
import shutil, subprocess
shutil.which = lambda _n: '/fake/bin/claude'
class _CleanGit:
    returncode = 0
    stdout = ''
    stderr = ''
subprocess.run = lambda *a, **kw: _CleanGit()
m._count_live_sessions = lambda _p: 0
# The dedup key is read from the REGISTRY entry (via the atomic reserve), NOT
# from the passed-in entry dict — seed it on the SAME head _gh_json returns,
# keyed on the clone_path register stored ($CLONE_PATH, not pwd: from a linked
# worktree pwd misses the entry and the not-in-registry fallback masks the test).
clone = r'$CLONE_PATH'
m._bump_auto_act_state(999, clone, 'deadbeefcafebabe1234567890abcdef12345678', 1)
entry = {'pr': 999, 'clone_path': clone,
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
# gh pr view returns a head_sha that differs from the seeded dedup head, so
# the reserve reaches the budget branch instead of short-circuiting on dedup.
m._gh_json = lambda args, **kw: {'headRefOid': 'feedfacefeedfacefeedfacefeedfacefeedface'}
# maybe_auto_act gates on claude-on-PATH, a clean clone, AND zero live sessions
# in the clone tree before the budget check; stub all three so the test is
# deterministic regardless of host PATH, the working tree's git state, or how
# many real sessions are live in CLONE_PATH (which is the primary clone — a dev
# box mid-session has live sessions there and the live-session gate would defer
# before the budget branch is reached; CI/Linux has neither claude nor sessions).
import shutil, subprocess
shutil.which = lambda _n: '/fake/bin/claude'
class _CleanGit:
    returncode = 0
    stdout = ''
    stderr = ''
subprocess.run = lambda *a, **kw: _CleanGit()
m._count_live_sessions = lambda _p: 0
# The budget counter is read from the REGISTRY entry (via the atomic reserve),
# NOT from the passed-in entry dict — seed it to the budget on a PRIOR head so
# this poll's (different) head is not deduped and trips the budget ceiling.
# clone_path must equal the value register stored (drive-qualified) to match
# the registry key.
clone = r'$CLONE_PATH'
m._bump_auto_act_state(999, clone, 'priorheadpriorheadpriorheadpriorhead0000', 2)
entry = {'pr': 999, 'clone_path': clone, 'auto_act_attempts': 2}
state = {'last_state': 'TRIAGE_BUDGET_EXHAUSTED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (1 actionable - block)'}
extras = m.maybe_auto_act(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"BUDGET_EXHAUSTED (2/2)"* ]]
}

# ---------- Option A: concurrent-session confinement (#913 fast-follow) ----------

@test "_count_live_sessions: fresh ts OR live ppid counts; stale+dead and absent dir do not" {
    run python - <<'PY'
import importlib.util, os, sys, time, tempfile, pathlib
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
tree = tempfile.mkdtemp()
reg = pathlib.Path(tree) / ".claude" / ".active-sessions"
reg.mkdir(parents=True)
now = int(time.time())
(reg / "fresh").write_text("branch=feat/a\nsha=x\nppid=999999999\nts=%d\n" % now)
(reg / "stale-dead").write_text("branch=feat/b\nsha=y\nppid=2147483646\nts=%d\n" % (now - 99999))
(reg / "stale-but-alive").write_text("branch=feat/c\nppid=%d\nts=%d\n" % (os.getpid(), now - 99999))
assert m._count_live_sessions(tree) == 2, m._count_live_sessions(tree)
assert m._count_live_sessions(tempfile.mkdtemp()) == 0  # no .active-sessions dir
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "maybe_auto_act: defers (no budget consumed) when a live session shares the clone tree" {
    run python - <<'PY'
import importlib.util, os, sys, time, tempfile, pathlib, subprocess
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_ACT"] = "true"
m._gh_owner_repo = lambda _p: ("alexandrosk0", "Smatchet")
m._gh_json = lambda args, **kw: {"headRefOid": "cafef00dcafef00dcafef00dcafef00dcafef00d"}
m.shutil.which = lambda _n: "/fake/bin/claude"
class _Clean:
    returncode = 0; stdout = ""; stderr = ""
subprocess.run = lambda *a, **kw: _Clean()   # clean `git status --porcelain`
clone = tempfile.mkdtemp()
reg = pathlib.Path(clone) / ".claude" / ".active-sessions"
reg.mkdir(parents=True)
(reg / "live").write_text("branch=feat/x\nsha=y\nppid=999999999\nts=%d\n" % int(time.time()))
entry = {"pr": 999, "clone_path": clone}
state = {"last_state": "TRIAGE_BUDGET_EXHAUSTED",
         "last_status_line": "Poll 1/1 CodeRabbit: COMMENTED (3 actionable - block)"}
extras = m.maybe_auto_act(state, entry)
print("extras:", extras)
assert "deferred" in extras.get("auto_act_action", ""), extras
assert "auto_act_attempts" not in extras, "defer must not consume a budget slot"
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"deferred"* ]]
}

# ---------- Triage budget default (option C: fast notify on CR findings) ----------

@test "MERGE_WATCH_TRIAGE_BUDGET default is 1 (was 3 - option C)" {
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

@test "smatchet-notify.sh missing --pr -> exit 2" {
    run bash "$SCRIPTS_DIR/smatchet-notify.sh" --state CI_FAIL --message "test"
    [ "$status" -eq 2 ]
    [[ "$output" == *"required"* ]]
}

@test "smatchet-notify.sh unknown arg -> exit 2" {
    run bash "$SCRIPTS_DIR/smatchet-notify.sh" --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown arg"* ]]
}

@test "smatchet-notify.sh with no writable channel -> exit 1 + 'ALL channels failed'" {
    # Channel 3 (file log) is a guaranteed fallback whenever LOCALAPPDATA /
    # XDG_STATE_HOME / HOME point somewhere writable — and setup() always sets
    # LOCALAPPDATA. So the all-channels-failed exit is only reachable with all
    # three path vars unset, AND no Smatchet (dead HTTP host), AND channel 2
    # (Windows toast) skipped. `env -u` strips the path vars for this one
    # invocation (a plain VAR=val prefix on `run` would not make the file-log
    # channel unavailable, so the script would always succeed via file-log and
    # exit 0). SMATCHET_NOTIFY_NO_WINDOWS_TOAST=1 deterministically skips the
    # toast channel — on git-bash OSTYPE=msys always triggers it, and once
    # merge-watcher-notify-setup.ps1 has installed BurntToast the toast would
    # otherwise SUCCEED and this test would flip to exit 0.
    run env -u LOCALAPPDATA -u XDG_STATE_HOME -u HOME \
        SMATCHET_NOTIFY_HOST=127.0.0.1 SMATCHET_NOTIFY_PORT=1 \
        SMATCHET_NOTIFY_NO_WINDOWS_TOAST=1 \
        bash "$SCRIPTS_DIR/smatchet-notify.sh" --pr 999 --state CI_FAIL --message "bats test"
    [ "$status" -eq 1 ]
    [[ "$output" == *"ALL channels failed"* ]]
}

@test "NOTIFY_STATES contains the 8 expected terminal states (incl. READY_FLIP_FAILED + STUCK_NEEDS_ATTENTION)" {
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
# STUCK_NEEDS_ATTENTION joined when the wedge-escalation driver started flipping
# last_state to it for the one-shot maybe_notify toast (the stuck-escalation
# feature); the assertion was left at 7 and silently red until now.
expected = {'CI_FAIL', 'GH_API_DOWN', 'PR_CLOSED_OR_MERGED', 'PAGINATION_OVERFLOW', 'TIMEOUT', 'TRIAGE_BUDGET_EXHAUSTED', 'READY_FLIP_FAILED', 'STUCK_NEEDS_ATTENTION'}
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

# ---------- Sub-bug (a) — counter reset on CR-clear ----------
# 2026-05-22 P1 (docs/self-improvement/categories/tooling.md line 31):
# previously, handle_blocked_cr_triage early-exited on `not _looks_like_cr_finding_block`
# WITHOUT resetting triage_attempts. After a CR-finding round + auto-fix push that
# made cr_state clean but left cr_open > 0, the registry kept the stale per-PR-
# lifetime counter, appearing latched at TRIAGE_BUDGET_EXHAUSTED.

@test "handle_blocked_cr_triage resets stale counter on CR-clear status_line" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Pre-populate triage_attempts > 0 from a prior CR-finding round.
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._bump_triage_attempts(999, r'$(pwd)', 7, 'samehead2222222222222222222222222222222222')
"
    [ "$status" -eq 0 ]
    # Status line shows CR is no longer block-shaped (cr_open > 0 BUT review clean).
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'triage_attempts': 7,
         'triage_for_head_sha': 'samehead2222222222222222222222222222222222'}
status_line = 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (3 open)'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"triage_reset_on_cr_clear"* ]]
    [[ "$output" == *"prior_attempts=7 -> 0"* ]]
    [[ "$output" == *"skipped: BLOCKED but not CR-finding"* ]]
}

@test "handle_blocked_cr_triage CR-clear with already-zero counter is a no-op" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import sys, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)', 'triage_attempts': 0}
status_line = 'Poll 1/1 CI: 5/5 pass | CodeRabbit: STALE_RESOLVED'
extras = m.handle_blocked_cr_triage(entry, status_line)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    # No reset key when counter was already 0 (avoids spurious registry write).
    [[ "$output" != *"triage_reset_on_cr_clear"* ]]
    [[ "$output" == *"skipped: BLOCKED but not CR-finding"* ]]
}

# ---------- Sub-bug (b) — resolveReviewThread after auto-act push ----------
# 2026-05-22 P1 sub-bug (b): CR threads remain `isResolved:false` after auto-fix
# push lands, leaving cr_open > 0 and the merge gate permanently BLOCKED.

@test "maybe_resolve_stuck_cr_threads: no-op when env explicitly set to false" {
    # Default-on as of 2026-05-28 — env unset means the feature fires.
    # Opt-out still works via false/0/no.
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'false'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: STALE_RESOLVED'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}

@test "maybe_resolve_stuck_cr_threads: env unset -> default-on (2026-05-28 flip)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Env unset should behave like enabled. To make this discriminating
    # against a default-off regression, satisfy the remaining gates and
    # stub the GitHub-touching helpers so the resolver path actually
    # fires. Under the old opt-in default this would short-circuit on the
    # env check and return {} — proving the flip when extras is non-empty.
    run python -c "
import os, sys, importlib.util
os.environ.pop('MERGE_WATCH_RESOLVE_CR_THREADS', None)
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'newhead9999999999999999999999999999999999', ['PRT_kwDO1'])
m._resolve_review_threads = lambda ids, clone: (len(ids), 0)
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (1 open)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('resolve_action:', extras.get('resolve_action', 'NONE'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"resolve_action: resolved 1/1 CR threads (failed=0)"* ]]
}

@test "maybe_resolve_stuck_cr_threads: no-op when auto_act_for_head_sha absent" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)'}  # never auto-acted
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: STALE_RESOLVED'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}

@test "maybe_resolve_stuck_cr_threads: no-op when status_line still CR-block-shaped" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: STALE_WITH_FINDINGS (3 actionable on prior commit - block)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"extras: {}"* ]]
}

@test "maybe_resolve_stuck_cr_threads: skipped when head unchanged since auto-act" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
# Stub the GraphQL fetch to return same head + 1 stuck CR thread.
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'samehead1111111111111111111111111111111111', ['PRT_kwDOABC'])
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'samehead1111111111111111111111111111111111'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (1 open)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"skipped: head unchanged since auto-act"* ]]
}

@test "maybe_resolve_stuck_cr_threads: fires resolveReviewThread per stuck CR thread" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
# Head has advanced since the prior auto-act dispatch.
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'newhead3333333333333333333333333333333333',
    ['PRT_kwDO1', 'PRT_kwDO2', 'PRT_kwDO3'])
calls = []
def fake_resolve(ids, clone):
    calls.extend(ids)
    return (len(ids), 0)
m._resolve_review_threads = fake_resolve
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (3 open)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
print('calls:', calls)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"resolved 3/3 CR threads"* ]]
    [[ "$output" == *"newhead3"* ]]
    [[ "$output" == *"calls: ['PRT_kwDO1', 'PRT_kwDO2', 'PRT_kwDO3']"* ]]
}

@test "maybe_resolve_stuck_cr_threads: dedup suppresses re-resolve on same head" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'newhead4444444444444444444444444444444444', ['PRT_xyz'])
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000',
         'last_resolved_for_head_sha': 'newhead4444444444444444444444444444444444'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (1 open)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"suppressed (already resolved on this head)"* ]]
}

@test "maybe_resolve_stuck_cr_threads: partial failure does NOT persist same-head dedup (CR feedback PR #487)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'newhead6666666666666666666666666666666666',
    ['PRT_a', 'PRT_b', 'PRT_c'])
# 1 of 3 resolutions fails — partial-failure path.
m._resolve_review_threads = lambda ids, clone: (2, 1)
bump_calls = []
real_bump = m._bump_resolved_threads
def tracking_bump(*args, **kw):
    bump_calls.append(args)
    real_bump(*args, **kw)
m._bump_resolved_threads = tracking_bump
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable) (3 open)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
print('bump_calls:', bump_calls)
"
    [ "$status" -eq 0 ]
    # resolve_action still emitted, but _bump_resolved_threads NOT called →
    # next poll retries the still-unresolved 1/3 thread on the same head.
    [[ "$output" == *"resolved 2/3 CR threads"* ]]
    [[ "$output" == *"failed=1"* ]]
    [[ "$output" == *"bump_calls: []"* ]]
}

@test "maybe_resolve_stuck_cr_threads: noop result recorded when zero CR threads found" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['MERGE_WATCH_RESOLVE_CR_THREADS'] = 'true'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
m = importlib.util.module_from_spec(spec); sys.modules['mw']=m; spec.loader.exec_module(m)
m._gh_owner_repo = lambda _p: ('alexandrosk0', 'Smatchet')
m._fetch_unresolved_cr_threads = lambda o, r, p, c: (
    'newhead5555555555555555555555555555555555', [])
entry = {'pr': 999, 'clone_path': r'$(pwd)',
         'auto_act_for_head_sha': 'oldhead0000000000000000000000000000000000'}
state = {'last_state': 'BLOCKED',
         'last_status_line': 'Poll 1/1 CodeRabbit: COMMENTED (0 actionable)'}
extras = m.maybe_resolve_stuck_cr_threads(state, entry)
print('extras:', extras)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"noop: zero unresolved CR threads"* ]]
}

# ---------- CR-NONE grace wedge (cross-cycle grace driver) ----------
# Regression for the wedge where a PR whose only blocker is a skipped/absent
# CodeRabbit review (NONE + status-SUCCESS, no inline) never merges: merge-gates
# passes NONE+SUCCESS only after its in-process poll index reaches CR_GRACE_POLLS,
# unreachable under the daemon's MERGE_GATES_MAX_POLLS=1 driving. The watcher now
# counts the window across real cycles (maybe_pass_cr_none_grace).

@test "cr-none-grace: detector matches NONE-wait shapes only" {
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
f = mw._looks_like_cr_none_grace_wait
assert f('CodeRabbit: NONE+status-SUCCESS-waiting-for-inline (poll 1/10)') is True
assert f('CodeRabbit: NONE+pending (poll 2/10)') is True
assert f('CodeRabbit: NONE+status-SUCCESS+inline-evidence (2 CR comment(s) on head)') is False
assert f('CodeRabbit: COMMENTED (3 actionable - block)') is False
assert f('CodeRabbit: CHANGES_REQUESTED') is False
assert f('CodeRabbit: APPROVED') is False
assert f('CodeRabbit: NONE+grace-expired') is False
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "cr-none-grace: threshold honors env + floors at 1 + ignores garbage" {
    run python -c "
import os, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
os.environ.pop('MERGE_WATCH_CR_NONE_GRACE_CYCLES', None)
assert mw._cr_none_grace_cycles() == 10
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='3'; assert mw._cr_none_grace_cycles()==3
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='0'; assert mw._cr_none_grace_cycles()==1
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='nan'; assert mw._cr_none_grace_cycles()==10
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "cr-none-grace: below threshold increments per-head counter + stays BLOCKED" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    # Inherit the (MSYS2-converted) LOCALAPPDATA from the env — do NOT re-set it
    # from the git-bash string, which pathlib reads as a drive-relative path and
    # would miss the registry the CLI process just wrote. gh is monkeypatched via
    # _gh_json so no PATH stub is needed.
    run python -c "
import os, importlib.util
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='5'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'headRefOid':'aaaa1111bbbb2222cccc3333dddd4444eeee5555'}
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+status-SUCCESS-waiting-for-inline (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('count:', res.get('cr_none_grace_polls'))
print('action:', res.get('cr_none_grace_action'))
print('flipped:', res.get('last_state'))
reg = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
print('persisted:', reg.get('cr_none_grace_polls'), reg.get('cr_none_grace_head','')[:8])
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"count: 1"* ]]
    [[ "$output" == *"waiting out CR-NONE grace [code] (1/5 cycles)"* ]]
    [[ "$output" == *"flipped: None"* ]]
    [[ "$output" == *"persisted: 1 aaaa1111"* ]]
}

@test "cr-none-grace: a new HEAD restarts the grace window" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, importlib.util
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='5'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'headRefOid':'99990000999900009999000099990000abcd9999'}
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
entry['cr_none_grace_polls']=4
entry['cr_none_grace_head']='0000oldoldoldoldoldoldoldoldoldoldold000'
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+pending (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('count:', res.get('cr_none_grace_polls'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"count: 1"* ]]
}

@test "cr-none-grace: at threshold re-polls with grace=0 and flips to GATES_PASSED" {
    run watch_cli register 999
    run python -c "
import os, importlib.util
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='3'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'headRefOid':'headheadheadheadheadheadheadheadhead0001'}
captured = {}
def fake_poll(entry, extra_gates_env=None):
    captured['env'] = extra_gates_env
    return {'pr':999,'clone_path':entry['clone_path'],'last_state':'GATES_PASSED','last_status_line':'forced pass'}
mw.poll_one = fake_poll
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
entry['cr_none_grace_polls']=2
entry['cr_none_grace_head']='headheadheadheadheadheadheadheadhead0001'
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+status-SUCCESS-waiting-for-inline (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('flipped:', res.get('last_state'))
print('env:', captured.get('env'))
print('action:', res.get('cr_none_grace_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"flipped: GATES_PASSED"* ]]
    [[ "$output" == *"env: {'MERGE_GATES_CR_GRACE_POLLS': '0'}"* ]]
    [[ "$output" == *"forced MERGE_GATES_CR_GRACE_POLLS=0 -> GATES_PASSED"* ]]
}

@test "cr-none-grace: HEAD fetch failure does NOT force a pass (fail-closed)" {
    run watch_cli register 999
    run python -c "
import os, importlib.util
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='1'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
def boom(args, cwd=None, timeout=30):
    raise RuntimeError('gh down')
mw._gh_json = boom
def fake_poll(entry, extra_gates_env=None):
    raise AssertionError('poll_one must NOT be called when HEAD fetch fails')
mw.poll_one = fake_poll
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+status-SUCCESS-waiting-for-inline (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('flipped:', res.get('last_state'))
print('action:', res.get('cr_none_grace_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"flipped: None"* ]]
    [[ "$output" == *"HEAD fetch failed"* ]]
}

@test "cr-none-grace: leaving the NONE-wait state resets a stale counter" {
    run watch_cli register 999
    run python -c "
import os, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
def fake_poll(entry, extra_gates_env=None):
    raise AssertionError('poll_one must NOT be called on a non-wait reset')
mw.poll_one = fake_poll
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
entry['cr_none_grace_polls']=4
entry['cr_none_grace_head']='someheadsomeheadsomeheadsomeheadsomehead'
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: COMMENTED (2 actionable - block)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('count:', res.get('cr_none_grace_polls'))
print('action:', res.get('cr_none_grace_action'))
reg = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
print('persisted:', reg.get('cr_none_grace_polls'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"count: 0"* ]]
    [[ "$output" == *"reset (CR left NONE-grace-wait state)"* ]]
    [[ "$output" == *"persisted: 0"* ]]
}

# ---------- cr-none-grace: pure-docs fast window ----------

@test "cr-none-grace pure-docs: threshold honors env + floors at 1 + ignores garbage" {
    run python -c "
import os, importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
os.environ.pop('MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS', None)
assert mw._cr_none_grace_cycles_pure_docs() == 1
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS']='4'; assert mw._cr_none_grace_cycles_pure_docs()==4
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS']='0'; assert mw._cr_none_grace_cycles_pure_docs()==1
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS']='nan'; assert mw._cr_none_grace_cycles_pure_docs()==1
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "cr-none-grace pure-docs: detector allow-list parity with is-pure-docs-diff.sh" {
    # _PURE_DOCS_ALLOW must accept exactly the four classes is-pure-docs-diff.sh
    # accepts (docs/, backlog/, agents/scripts/, any *.md) and reject the rest.
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
m = mw._PURE_DOCS_ALLOW.match
for p in ['docs/x.md','backlog/2026/p1.md','agents/scripts/core/x.sh','README.md',
          'Source/Core/src/Grid/AGENTS.md','CONTEXT-MAP.md']:
    assert m(p), p
for p in ['Source/Core/src/Foo.cpp','scripts/dev/build.sh','.github/workflows/ci.yml',
          'docs.md.cpp','mydocs/x.txt']:
    assert not m(p), p
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "cr-none-grace pure-docs: _pr_diff_is_pure_docs all-docs True, mixed False, gh-down False" {
    run python -c "
import importlib.util
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'files':[{'path':'docs/a.md'},{'path':'backlog/b.md'}]}
assert mw._pr_diff_is_pure_docs(999, '/x') is True
mw._gh_json = lambda args, cwd=None, timeout=30: {'files':[{'path':'docs/a.md'},{'path':'Source/Core/src/X.cpp'}]}
assert mw._pr_diff_is_pure_docs(999, '/x') is False
mw._gh_json = lambda args, cwd=None, timeout=30: {'files':[]}
assert mw._pr_diff_is_pure_docs(999, '/x') is False
def boom(args, cwd=None, timeout=30):
    raise RuntimeError('gh down')
mw._gh_json = boom
assert mw._pr_diff_is_pure_docs(999, '/x') is False
print('ok')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}

@test "cr-none-grace pure-docs: at default threshold 1, first cycle forces the pass" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, importlib.util
os.environ.pop('MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS', None)
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='10'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'headRefOid':'docsheaddocsheaddocsheaddocsheaddocs0001'}
mw._pr_diff_is_pure_docs = lambda pr, clone_path: True
captured = {}
def fake_poll(entry, extra_gates_env=None):
    captured['env'] = extra_gates_env
    return {'pr':999,'clone_path':entry['clone_path'],'last_state':'GATES_PASSED','last_status_line':'forced pass'}
mw.poll_one = fake_poll
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+status-SUCCESS-waiting-for-inline (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('flipped:', res.get('last_state'))
print('env:', captured.get('env'))
print('action:', res.get('cr_none_grace_action'))
"
    [ "$status" -eq 0 ]
    # One cycle (count 1) reaches the pure-docs threshold of 1 -> forced pass,
    # even though the code-diff window (10) would still be waiting.
    [[ "$output" == *"flipped: GATES_PASSED"* ]]
    [[ "$output" == *"env: {'MERGE_GATES_CR_GRACE_POLLS': '0'}"* ]]
    [[ "$output" == *"CR-NONE grace elapsed [pure-docs] (1 cycles"* ]]
}

@test "cr-none-grace pure-docs: code diff still waits the full window (no early pass)" {
    run watch_cli register 999
    [ "$status" -eq 0 ]
    run python -c "
import os, importlib.util
os.environ['MERGE_WATCH_CR_NONE_GRACE_CYCLES']='10'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); spec.loader.exec_module(mw)
mw._gh_json = lambda args, cwd=None, timeout=30: {'headRefOid':'codeheadcodeheadcodeheadcodeheadcode0001'}
mw._pr_diff_is_pure_docs = lambda pr, clone_path: False
def fake_poll(entry, extra_gates_env=None):
    raise AssertionError('poll_one must NOT fire on cycle 1 of a 10-cycle code window')
mw.poll_one = fake_poll
entry = next(e for e in mw._CLI.read_registry() if int(e['pr'])==999)
state = {'pr':999,'last_state':'BLOCKED','last_status_line':'CodeRabbit: NONE+pending (poll 1/10)'}
res = mw.maybe_pass_cr_none_grace(entry, state)
print('count:', res.get('cr_none_grace_polls'))
print('action:', res.get('cr_none_grace_action'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"count: 1"* ]]
    [[ "$output" == *"waiting out CR-NONE grace [code] (1/10 cycles)"* ]]
}

# ---------- watch-register-if-enabled.sh (ship-time opt-in auto-register) ----------

@test "watch-register: SMATCHET_WATCH_ALL_PRS unset -> no-op, PR not registered" {
    run env -u SMATCHET_WATCH_ALL_PRS bash "$SCRIPTS_DIR/watch-register-if-enabled.sh" 951
    [ "$status" -eq 0 ]
    [[ "$output" == *"not set"* ]]
    # Registry stays empty — the flag-off path must not touch it.
    run watch_cli list
    [ "$status" -eq 0 ]
    [[ "$output" == *"[]"* ]]
}

@test "watch-register: SMATCHET_WATCH_ALL_PRS=1 -> registers the PR" {
    run env SMATCHET_WATCH_ALL_PRS=1 bash "$SCRIPTS_DIR/watch-register-if-enabled.sh" 951
    [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('cli', r'$SCRIPTS_DIR/merge-watcher-cli.py')
cli = importlib.util.module_from_spec(spec); sys.modules['cli']=cli; spec.loader.exec_module(cli)
print('prs:', sorted(int(e['pr']) for e in cli.read_registry()))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"prs: [951]"* ]]
}

@test "watch-register: flag on + already-registered -> exit 0 (benign)" {
    run watch_cli register 951; [ "$status" -eq 0 ]
    run env SMATCHET_WATCH_ALL_PRS=true bash "$SCRIPTS_DIR/watch-register-if-enabled.sh" 951
    [ "$status" -eq 0 ]
    [[ "$output" == *"already registered"* ]]
}

@test "watch-register: missing <pr> arg -> exit 2" {
    run bash "$SCRIPTS_DIR/watch-register-if-enabled.sh"
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage:"* ]]
}

# ---------- Reconcile-on-poll auto-unregister (PR_CLOSED_OR_MERGED short-circuit) ----------
#
# These monkeypatch mw._pr_lifecycle_state (the gh I/O seam) rather than
# stubbing a `gh` binary on PATH: native-Windows Python's shutil.which skips
# extensionless PATH stubs (it requires a PATHEXT extension), so the absolute
# GH_BIN the daemon resolves at import would run the REAL gh and the stub would
# be silently ignored. Patching the helper keeps the test deterministic and
# offline on every platform.

@test "poll_one short-circuits a MERGED PR to PR_CLOSED_OR_MERGED (reconcile precheck)" {
    # A merged PR must leave the registry. Before this precheck, poll_one's
    # ensure_pr_ready_for_review() flip failed on a merged PR (you cannot
    # `gh pr ready` a merged PR) -> READY_FLIP_FAILED every cycle, never the
    # exit-4 PR_CLOSED_OR_MERGED branch that auto-unregisters. Assert the
    # short-circuit (state + rc 4) fires when the lifecycle helper reports MERGED.
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); sys.modules['mw']=mw; spec.loader.exec_module(mw)
mw._pr_lifecycle_state = lambda pr, clone_path: 'MERGED'
state = mw.poll_one({'pr': 528, 'clone_path': r'$CLONE_PATH'})
print('last_state:', state.get('last_state'))
print('rc:', state.get('gates_return_code'))
print('line:', state.get('last_status_line'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"last_state: PR_CLOSED_OR_MERGED"* ]]
    [[ "$output" == *"rc: 4"* ]]
    [[ "$output" == *"MERGED"* ]]
}

@test "poll_one short-circuits a CLOSED PR to PR_CLOSED_OR_MERGED (reconcile precheck)" {
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); sys.modules['mw']=mw; spec.loader.exec_module(mw)
mw._pr_lifecycle_state = lambda pr, clone_path: 'CLOSED'
state = mw.poll_one({'pr': 777, 'clone_path': r'$CLONE_PATH'})
print('last_state:', state.get('last_state'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"last_state: PR_CLOSED_OR_MERGED"* ]]
}

@test "poll_one does NOT short-circuit an OPEN PR (reconcile precheck passes through)" {
    # An OPEN PR must proceed to the normal poll path, not be reconciled away.
    # Force the downstream gh repo-view to FileNotFoundError (bogus GH_BIN) so
    # the open path returns a GH_* state fast/offline — enough to prove the
    # reconcile short-circuit did NOT fire.
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('mw', r'$SCRIPTS_DIR/merge-watcher.py')
mw = importlib.util.module_from_spec(spec); sys.modules['mw']=mw; spec.loader.exec_module(mw)
mw._pr_lifecycle_state = lambda pr, clone_path: 'OPEN'
mw.GH_BIN = 'smatchet-no-such-gh-binary-xyz'
state = mw.poll_one({'pr': 999, 'clone_path': r'$CLONE_PATH'})
print('last_state:', state.get('last_state'))
"
    [ "$status" -eq 0 ]
    [[ "$output" != *"PR_CLOSED_OR_MERGED"* ]]
}

# ---------- prune subcommand (registry janitor) ----------
#
# Monkeypatch cli._pr_lifecycle_state (the gh I/O seam) so classification is
# deterministic + offline — the absolute _GH_BIN resolved at import would hit
# real gh, and native-Windows shutil.which can't see extensionless PATH stubs.

@test "prune unregisters MERGED/CLOSED, keeps OPEN + unknown" {
    run watch_cli register 901; [ "$status" -eq 0 ]
    run watch_cli register 902; [ "$status" -eq 0 ]
    run watch_cli register 903; [ "$status" -eq 0 ]
    run watch_cli register 904; [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('cli', r'$SCRIPTS_DIR/merge-watcher-cli.py')
cli = importlib.util.module_from_spec(spec); sys.modules['cli']=cli; spec.loader.exec_module(cli)
states = {901: 'MERGED', 902: 'OPEN', 903: 'CLOSED', 904: ''}  # 904 = gh-unknown
cli._pr_lifecycle_state = lambda pr, clone_path: states.get(int(pr), '')
rc = cli.main(['prune'])
print('rc:', rc)
print('remaining:', sorted(int(e['pr']) for e in cli.read_registry()))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rc: 0"* ]]
    [[ "$output" == *"pruned #901 (MERGED)"* ]]
    [[ "$output" == *"pruned #903 (CLOSED)"* ]]
    [[ "$output" == *"remaining: [902, 904]"* ]]
}

@test "prune --dry-run reports but does not mutate the registry" {
    run watch_cli register 901; [ "$status" -eq 0 ]
    run python -c "
import os, sys, importlib.util
os.environ['LOCALAPPDATA'] = r'$LOCALAPPDATA'
spec = importlib.util.spec_from_file_location('cli', r'$SCRIPTS_DIR/merge-watcher-cli.py')
cli = importlib.util.module_from_spec(spec); sys.modules['cli']=cli; spec.loader.exec_module(cli)
cli._pr_lifecycle_state = lambda pr, clone_path: 'MERGED'
rc = cli.main(['prune', '--dry-run'])
print('rc:', rc)
print('remaining:', sorted(int(e['pr']) for e in cli.read_registry()))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"would prune #901 (MERGED)"* ]]
    [[ "$output" == *"remaining: [901]"* ]]
}

# ---------- wedge escalation: STUCK_NEEDS_ATTENTION (merge-watcher-stuck-escalation.md) ----------

@test "_parse_poll_ci_counts parses a real Poll line; None on non-Poll garbage" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
line = "Poll 1/1 — CI: 28/29 pass (1 fail, 0 pending, 0 warn-downgraded, 2 req-missing) | CodeRabbit: APPROVED (0 open) | User: 3 | reviewDecision: APPROVED"
assert m._parse_poll_ci_counts(line) == {"fail":1,"pending":0,"req_missing":2,"cr_open":0,"user":3}
assert m._parse_poll_ci_counts("STDERR: gh down") is None
assert m._parse_poll_ci_counts("") is None
assert m._stuck_cycles() == 3
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "_classify_pr_wedge returns the right reason per gh state; fail-closed None" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
e = {"pr":1,"clone_path":"/x"}
fail  = "Poll 1/1 — CI: 28/29 pass (1 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: APPROVED (0 open) | User: 0 | reviewDecision: APPROVED"
green = "Poll 1/1 — CI: 29/29 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: APPROVED (0 open) | User: 0 | reviewDecision: APPROVED"
pend  = "Poll 1/1 — CI: 20/29 pass (0 fail, 5 pending, 0 warn-downgraded, 4 req-missing) | CodeRabbit: APPROVED (0 open) | User: 0 | reviewDecision: APPROVED"
m._gh_json = lambda *a,**k: {"mergeStateStatus":"DIRTY","mergeable":"CONFLICTING","headRefOid":"abc"}
assert m._classify_pr_wedge(e, {"last_status_line":fail,"last_state":"GATES_PASSED"}) == ("CONFLICT","abc")
m._gh_json = lambda *a,**k: {"mergeStateStatus":"BLOCKED","mergeable":"MERGEABLE","headRefOid":"d"}
assert m._classify_pr_wedge(e, {"last_status_line":pend,"last_state":"BLOCKED"}) == (None,"d")   # pending = transient
assert m._classify_pr_wedge(e, {"last_status_line":fail,"last_state":"BLOCKED"}) == ("CI_FAILING","d")
m._gh_owner_repo = lambda cp: ("o","r")
m._fetch_unresolved_cr_threads = lambda *a,**k: ("d", ["T1"])
assert m._classify_pr_wedge(e, {"last_status_line":green,"last_state":"BLOCKED"}) == ("UNRESOLVED_THREADS","d")
m._fetch_unresolved_cr_threads = lambda *a,**k: ("d", [])
assert m._classify_pr_wedge(e, {"last_status_line":green,"last_state":"BLOCKED"}) == ("REVIEW_REQUIRED","d")
m._gh_json = lambda *a,**k: {"mergeStateStatus":"BEHIND","mergeable":"MERGEABLE","headRefOid":"bh"}
assert m._classify_pr_wedge(e, {"last_status_line":green,"last_state":"BLOCKED"}) == ("BEHIND","bh")
def boom(*a,**k): raise RuntimeError("gh down")
m._gh_json = boom
assert m._classify_pr_wedge(e, {"last_status_line":fail,"last_state":"BLOCKED"}) == (None,"")
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "maybe_escalate_stuck_pr accrues a head-pinned streak; escalates at threshold; resets on progress/head-change" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m._bump_stuck_streak = lambda *a, **k: None   # isolate from registry I/O
st = {"last_state":"BLOCKED","last_status_line":"Poll 1/1 — CI: 1/1 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: x (0 open) | User: 0 | reviewDecision: x"}
# not BLOCKED / not dirty-pass -> no-op
assert m.maybe_escalate_stuck_pr({"last_state":"GATES_PASSED","merge_action":"merged"}, {"pr":1,"clone_path":"/x"}) == {}
# CR-finding block is owned by triage -> gated out
m._classify_pr_wedge = lambda e,s: ("CI_FAILING","h")
assert m.maybe_escalate_stuck_pr({"last_state":"BLOCKED","last_status_line":"COMMENTED (2 actionable — block)"}, {"pr":1,"clone_path":"/x"}) == {}
# transient (None) with a prior streak -> reset
m._classify_pr_wedge = lambda e,s: (None,"h")
assert m.maybe_escalate_stuck_pr(dict(st), {"pr":1,"clone_path":"/x","stuck_streak":2})["stuck_streak"] == 0
# threshold accrual (default 3)
m._classify_pr_wedge = lambda e,s: ("CONFLICT","hZ")
r1 = m.maybe_escalate_stuck_pr(dict(st), {"pr":9,"clone_path":"/x","stuck_streak":0,"stuck_head":""})
assert r1["stuck_streak"] == 1 and "last_state" not in r1
r2 = m.maybe_escalate_stuck_pr(dict(st), {"pr":9,"clone_path":"/x","stuck_streak":1,"stuck_head":"hZ"})
assert r2["stuck_streak"] == 2 and "last_state" not in r2
r3 = m.maybe_escalate_stuck_pr(dict(st), {"pr":9,"clone_path":"/x","stuck_streak":2,"stuck_head":"hZ"})
assert r3["last_state"] == "STUCK_NEEDS_ATTENTION" and r3["stuck_reason"] == "CONFLICT"
# head change restarts the streak even from a high prior
assert m.maybe_escalate_stuck_pr(dict(st), {"pr":9,"clone_path":"/x","stuck_streak":9,"stuck_head":"OLD"})["stuck_streak"] == 1
# resolve-threads made progress this cycle -> gated out
st2 = dict(st); st2["resolve_action"] = "resolved 2/3 CR threads (failed=0) on head ab"
assert m.maybe_escalate_stuck_pr(st2, {"pr":9,"clone_path":"/x","stuck_streak":2,"stuck_head":"hZ"}) == {}
# DIRTY-pass (merge_failed) path is in scope
r5 = m.maybe_escalate_stuck_pr(dict(st, last_state="GATES_PASSED", merge_action="merge_failed: dirty"), {"pr":9,"clone_path":"/x","stuck_streak":2,"stuck_head":"hZ"})
assert r5["last_state"] == "STUCK_NEEDS_ATTENTION"
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "maybe_auto_update_behind: opt-in + green-only; dispatches via cascade_update_child; dedups + budget-caps (infra.md :223)" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
green    = "Poll 1/1 — CI: 5/5 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: APPROVED (0 open) | User: 0 | reviewDecision: APPROVED"
notgreen = "Poll 1/1 — CI: 4/5 pass (1 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: APPROVED (0 open) | User: 0 | reviewDecision: APPROVED"
cropen   = "Poll 1/1 — CI: 5/5 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: COMMENTED (2 open) | User: 0 | reviewDecision: APPROVED"
usercmt  = "Poll 1/1 — CI: 5/5 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: APPROVED (0 open) | User: 1 | reviewDecision: APPROVED"
e = {"pr":7,"clone_path":"/x"}
m._gh_owner_repo = lambda cp: ("o","r")
m._bump_stuck_streak = lambda *a, **k: None   # isolate from registry I/O
calls = []
m.cascade_update_child = lambda o,r,pr: (calls.append(pr) or (True, "update-branch dispatched"))
# disabled by default -> {}
os.environ.pop("MERGE_WATCH_AUTO_UPDATE_BEHIND", None)
assert m.maybe_auto_update_behind(e, green, "h1") == {}
os.environ["MERGE_WATCH_AUTO_UPDATE_BEHIND"] = "true"
# not-green / cr-open / unresolved-user-comment / unparseable -> {} (never a non-clean PR)
assert m.maybe_auto_update_behind(e, notgreen, "h1") == {}
assert m.maybe_auto_update_behind(e, cropen, "h1") == {}
assert m.maybe_auto_update_behind(e, usercmt, "h1") == {}   # User: 1 blocks (Cursor #1393)
assert m.maybe_auto_update_behind(e, "gh request failed", "h1") == {}
assert calls == []
# green + reserve OK -> dispatch once; streak reset in the returned delta
m._atomic_reserve_auto_update = lambda pr,cp,h,b: ("ok", 1)
d = m.maybe_auto_update_behind(e, green, "h1")
assert "dispatched" in d["auto_update_action"] and d["stuck_streak"] == 0
assert calls == [7]
# dedup (same head already advanced) -> {} (no second dispatch)
m._atomic_reserve_auto_update = lambda pr,cp,h,b: ("dedup", None)
assert m.maybe_auto_update_behind(e, green, "h1") == {}
# budget exhausted -> {} (falls through to the human STUCK escalation)
m._atomic_reserve_auto_update = lambda pr,cp,h,b: ("budget", 2)
assert m.maybe_auto_update_behind(e, green, "h2") == {}
assert calls == [7]   # still exactly one dispatch
# FAILURE PATHS must also return {} so escalation still fires (CodeRabbit #1393)
m._atomic_reserve_auto_update = lambda pr,cp,h,b: ("ok", 1)
m._gh_owner_repo = lambda cp: None                      # gh repo view fails
assert m.maybe_auto_update_behind(e, green, "h3") == {}
m._gh_owner_repo = lambda cp: ("o","r")
m.cascade_update_child = lambda o,r,pr: (False, "update-branch failed")  # dispatch fails
assert m.maybe_auto_update_behind(e, green, "h4") == {}
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "maybe_escalate_stuck_pr: a BEHIND wedge short-circuits to auto-update-behind, else falls through to streak (infra.md :223)" {
    run python - <<'PY'
import importlib.util, os, sys
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
sys.path.insert(0, sd)
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m._bump_stuck_streak = lambda *a, **k: None
st = {"last_state":"BLOCKED","last_status_line":"Poll 1/1 — CI: 1/1 pass (0 fail, 0 pending, 0 warn-downgraded, 0 req-missing) | CodeRabbit: x (0 open) | User: 0 | reviewDecision: x"}
m._classify_pr_wedge = lambda e,s: ("BEHIND","bh")
# auto-update fires -> escalate returns the auto-update delta, NOT a streak/escalation
m.maybe_auto_update_behind = lambda e,sl,h: {"auto_update_action":"update-branch dispatched","stuck_streak":0,"stuck_action":"auto-update ok"}
r = m.maybe_escalate_stuck_pr(dict(st), {"pr":3,"clone_path":"/x","stuck_streak":0,"stuck_head":""})
assert r.get("auto_update_action") and "last_state" not in r, r
# auto-update declines ({}) -> normal streak accrual resumes
m.maybe_auto_update_behind = lambda e,sl,h: {}
r2 = m.maybe_escalate_stuck_pr(dict(st), {"pr":3,"clone_path":"/x","stuck_streak":0,"stuck_head":""})
assert r2["stuck_streak"] == 1, r2
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "merge-watcher-stuck-nudge.sh: silent on empty registry; emits a block for an escalated PR" {
    # empty registry -> silent + exit 0
    run bash "$SCRIPTS_DIR/merge-watcher-stuck-nudge.sh" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # seed a STUCK registry + state file via the CLI module's own paths
    python - <<'PY'
import importlib.util, os, json
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("cli", os.path.join(sd, "merge-watcher-cli.py"))
cli = importlib.util.module_from_spec(spec); spec.loader.exec_module(cli)
sdir = cli.state_dir(); sdir.mkdir(parents=True, exist_ok=True)
cli.write_registry([{"pr":42,"clone_path":"/c/clones/Smatchet","stuck_reason":"CONFLICT","stuck_streak":4,"stuck_head":"abc"}])
(sdir/"42.json").write_text(json.dumps({"pr":42,"last_state":"STUCK_NEEDS_ATTENTION","last_poll_unix":0}), encoding="utf-8")
PY
    run bash "$SCRIPTS_DIR/merge-watcher-stuck-nudge.sh" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"STUCK_NEEDS_ATTENTION"* ]]
    [[ "$output" == *"PR #42"* ]]
    [[ "$output" == *"CONFLICT"* ]]
}

# ---------- gate-logic self-resync (#1428 residual) ----------
# The daemon's throughput-safe complement to merge-gates.sh's fail-closed freshness
# guard: detect when this checkout drifted behind origin/develop on a gate-logic file
# and, on a SAFE fast-forward, pull develop (+ re-exec on POSIX when the daemon's own
# code changed). The security-critical drift-detection + safety gate run against REAL
# git (a throwaway repo+remote); the orchestration branches run against mocked seams.

@test "self-resync: detect_gate_logic_drift + _resync_safety vs real git (fresh/dirty/feature/diverged)" {
    run python - <<'PY'
import importlib.util, os, subprocess, tempfile, pathlib
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
tmp = tempfile.mkdtemp()
def g(*a, cwd): subprocess.run(["git","-C",cwd,*a], check=True, capture_output=True, text=True)
remote = os.path.join(tmp, "remote.git")
subprocess.run(["git","init","--bare","-b","develop",remote], check=True, capture_output=True)
work = os.path.join(tmp, "work")
subprocess.run(["git","clone",remote,work], check=True, capture_output=True)
g("config","user.email","t@t",cwd=work); g("config","user.name","t",cwd=work)
rel = "agents/scripts/core/merge-gates.sh"
p = pathlib.Path(work, rel); p.parent.mkdir(parents=True, exist_ok=True); p.write_text("v1\n")
g("add","-A",cwd=work); g("commit","-m","init",cwd=work); g("push","origin","develop",cwd=work)
# fresh + clean develop -> no drift, safe
assert m._git_fetch_develop(work) is True
assert m.detect_gate_logic_drift(work) == [], m.detect_gate_logic_drift(work)
ok, why = m._resync_safety(work); assert ok, why
# uncommitted on-disk drift -> detected + unsafe(dirty)
p.write_text("v2-local\n")
assert rel in m.detect_gate_logic_drift(work)
ok, why = m._resync_safety(work); assert (not ok) and "dirty" in why, why
# committed locally -> clean but AHEAD of origin/develop (not a fast-forward)
g("add","-A",cwd=work); g("commit","-m","local v2",cwd=work)
ok, why = m._resync_safety(work); assert (not ok) and "diverged" in why, why
# feature branch -> unsafe(not on develop)
g("checkout","-b","feat/x",cwd=work)
ok, why = m._resync_safety(work); assert (not ok) and "not on develop" in why, why
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: _ff_pull_develop fast-forwards a behind develop, reports moved; re-pull no-move" {
    run python - <<'PY'
import importlib.util, os, subprocess, tempfile, pathlib
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
tmp = tempfile.mkdtemp()
def g(*a, cwd): subprocess.run(["git","-C",cwd,*a], check=True, capture_output=True, text=True)
remote = os.path.join(tmp, "remote.git")
subprocess.run(["git","init","--bare","-b","develop",remote], check=True, capture_output=True)
rel = "agents/scripts/core/merge-gates.sh"
# work1 seeds v1, work2 advances origin to v2, work1 (behind) must fast-forward to v2.
work = os.path.join(tmp, "work"); subprocess.run(["git","clone",remote,work], check=True, capture_output=True)
g("config","user.email","t@t",cwd=work); g("config","user.name","t",cwd=work)
p = pathlib.Path(work, rel); p.parent.mkdir(parents=True, exist_ok=True); p.write_text("v1\n")
g("add","-A",cwd=work); g("commit","-m","init",cwd=work); g("push","origin","develop",cwd=work)
work2 = os.path.join(tmp, "work2"); subprocess.run(["git","clone",remote,work2], check=True, capture_output=True)
g("config","user.email","t2@t",cwd=work2); g("config","user.name","t2",cwd=work2)
pathlib.Path(work2, rel).write_text("v2-remote\n")
g("add","-A",cwd=work2); g("commit","-m","remote v2",cwd=work2); g("push","origin","develop",cwd=work2)
# work is now behind origin/develop by one commit.
assert m._git_fetch_develop(work) is True
assert rel in m.detect_gate_logic_drift(work)
ok, why = m._resync_safety(work); assert ok, why
pulled, moved = m._ff_pull_develop(work)
assert pulled and moved, (pulled, moved)
assert pathlib.Path(work, rel).read_text() == "v2-remote\n", "file not refreshed by ff"
assert m.detect_gate_logic_drift(work) == [], "post-pull should be fresh"
pulled2, moved2 = m._ff_pull_develop(work)
assert pulled2 and (not moved2), ("re-pull must be a no-op move", pulled2, moved2)
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: maybe_self_resync disabled / not-a-checkout / fetch-fail / fresh" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "off"
assert m.maybe_self_resync(0)["resync_action"].startswith("disabled"), m.maybe_self_resync(0)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "on"
m._repo_root = lambda: None
assert "not in a git checkout" in m.maybe_self_resync(0)["resync_action"]
m._repo_root = lambda: "/fake/root"
m._git_fetch_develop = lambda root: False
assert "git fetch origin develop failed" in m.maybe_self_resync(0)["resync_action"]
m._git_fetch_develop = lambda root: True
m.detect_gate_logic_drift = lambda root: []
assert m.maybe_self_resync(0)["resync_action"] == "fresh"
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: unsafe drift -> needs_human, never pulls or re-execs (#1428 feature-branch park)" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "on"
m._repo_root = lambda: "/fake/root"
m._git_fetch_develop = lambda root: True
m.detect_gate_logic_drift = lambda root: ["agents/scripts/core/merge-gates.sh"]
m._resync_safety = lambda root: (False, "not on develop (on 'feat/tsan')")
calls = {"pull": 0, "reexec": 0}
m._ff_pull_develop = lambda root: (calls.__setitem__("pull", calls["pull"] + 1), (True, True))[1]
m._reexec_daemon = lambda d: calls.__setitem__("reexec", calls["reexec"] + 1)
r = m.maybe_self_resync(0)
assert r.get("resync_needs_human") is True, r
assert "unsafe to auto-resync" in r["resync_action"], r
assert calls == {"pull": 0, "reexec": 0}, ("must not mutate", calls)
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: safe drift in merge-gates.sh only -> resync on-disk, NO re-exec" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "on"
m._repo_root = lambda: "/fake/root"
m._git_fetch_develop = lambda root: True
m.detect_gate_logic_drift = lambda root: ["agents/scripts/core/merge-gates.sh"]
m._resync_safety = lambda root: (True, "ok")
m._ff_pull_develop = lambda root: (True, True)
calls = {"reexec": 0}
m._reexec_daemon = lambda d: calls.__setitem__("reexec", calls["reexec"] + 1)
r = m.maybe_self_resync(0)
assert "no restart needed" in r["resync_action"], r
assert calls["reexec"] == 0, "merge-gates.sh is re-read live; no daemon restart"
assert not r.get("resync_needs_restart") and not r.get("resync_needs_human"), r
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: safe daemon-code drift -> re-exec on POSIX, restart-warn on Windows" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "on"
m._repo_root = lambda: "/fake/root"
m._git_fetch_develop = lambda root: True
m.detect_gate_logic_drift = lambda root: ["agents/scripts/core/merge-watcher.py"]
m._resync_safety = lambda root: (True, "ok")
m._ff_pull_develop = lambda root: (True, True)
rec = {"reexec": 0, "drifted": None}
m._reexec_daemon = lambda d: (rec.__setitem__("reexec", rec["reexec"] + 1), rec.__setitem__("drifted", d))
# POSIX → re-exec invoked with the daemon-code drift list
m.os.name = "posix"
m.maybe_self_resync(0)
assert rec["reexec"] == 1, rec
assert rec["drifted"] == ["agents/scripts/core/merge-watcher.py"], rec
# Windows → NO re-exec (would detach from the Scheduled Task); needs_restart flagged
m.os.name = "nt"
rec["reexec"] = 0
r = m.maybe_self_resync(0)
assert rec["reexec"] == 0, "must not os.execv on Windows"
assert r.get("resync_needs_restart") is True, r
assert "no auto-reexec on Windows" in r["resync_action"], r
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: ff-pull no-move -> needs_human (re-exec loop guard); pull-fail -> needs_human" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ["MERGE_WATCH_AUTO_RESYNC"] = "on"
m._repo_root = lambda: "/fake/root"
m._git_fetch_develop = lambda root: True
m.detect_gate_logic_drift = lambda root: ["agents/scripts/core/merge-watcher.py"]
m._resync_safety = lambda root: (True, "ok")
calls = {"reexec": 0}
m._reexec_daemon = lambda d: calls.__setitem__("reexec", calls["reexec"] + 1)
# pulled but HEAD didn't move → would loop → refuse re-exec, flag human
m._ff_pull_develop = lambda root: (True, False)
r = m.maybe_self_resync(0)
assert r.get("resync_needs_human") is True and "avoids loop" in r["resync_action"], r
assert calls["reexec"] == 0
# pull failed outright → flag human, no re-exec
m._ff_pull_develop = lambda root: (False, False)
r = m.maybe_self_resync(0)
assert r.get("resync_needs_human") is True and "ff-pull failed" in r["resync_action"], r
assert calls["reexec"] == 0
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "self-resync: _resync_every_cycles default 30, env override, floor 1, invalid->default" {
    run python - <<'PY'
import importlib.util, os
sd = os.path.join(os.environ["REPO_ROOT"], "agents", "scripts", "core")
spec = importlib.util.spec_from_file_location("mw", os.path.join(sd, "merge-watcher.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
os.environ.pop("MERGE_WATCH_RESYNC_EVERY_CYCLES", None)
assert m._resync_every_cycles() == 30, m._resync_every_cycles()
os.environ["MERGE_WATCH_RESYNC_EVERY_CYCLES"] = "5"
assert m._resync_every_cycles() == 5
os.environ["MERGE_WATCH_RESYNC_EVERY_CYCLES"] = "0"
assert m._resync_every_cycles() == 1, "floored at 1"
os.environ["MERGE_WATCH_RESYNC_EVERY_CYCLES"] = "not-an-int"
assert m._resync_every_cycles() == 30, "invalid falls back to default"
print("OK")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}
