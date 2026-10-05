#!/usr/bin/env bats
# tests/bats/worktree_prune.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/worktree-prune.sh. The pure prune_decision guard is
# covered by --selftest; here we prove the real mutation on a temp repo + linked
# worktree, with `gh` stubbed to report a chosen branch MERGED.
# ----------------------------------------------------------------------------

setup() {
    # This suite's subject is host content (scripts/), the scripts it pairs with
    # are layer content (agents/). Once the layer is the host's agent-layer/
    # submodule the suite lives in the layer, so the host comes from
    # project-config.sh (the superproject) and the layer from this suite's own tree.
    # Before the flip both are this checkout.
    LAYER_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    REPO_ROOT="$(PC_ROOTS_ONLY=1 . "$LAYER_ROOT/scripts/dev/project-config.sh" >/dev/null 2>&1; printf '%s' "${PROJECT_ROOT:-$LAYER_ROOT}")"
    SCRIPT="$REPO_ROOT/scripts/dev/worktree-prune.sh"
    MAIN="$(mktemp -d)/main"
    git init -q -b develop "$MAIN"
    git -C "$MAIN" config user.email t@local
    git -C "$MAIN" config user.name t
    ( cd "$MAIN" && echo seed > s && git add -A && git commit -qm seed )
    WT="$(mktemp -d)/wt-x"
    git -C "$MAIN" worktree add -q -b feat/x "$WT" >/dev/null 2>&1
    # A just-created worktree is "active" under the default 24 h idle guard; age
    # it so the reap paths below exercise the default threshold.
    age_worktree "$WT" 48
    STUB="$(mktemp -d)"
    # The stub PR is MERGED at the worktree's current tip (headRefOid), the shape
    # a reap requires; the moved-tip case below commits past it.
    PR_HEAD_OID="$(git -C "$WT" rev-parse HEAD)"
    printf '#!/usr/bin/env bash\nprintf "feat/x\\tMERGED\\t%s\\n"\n' "$PR_HEAD_OID" > "$STUB/gh"
    chmod +x "$STUB/gh"
    # Pin the protected-branch config to a fixture (rung 0 of project-config.sh)
    # so the real project.config.json never leaks into these assertions.
    printf '{"vcs": {"protected_branches": ["asset-store"]}}\n' > "$STUB/project.config.json"
    export PC_CONFIG_FILE="$STUB/project.config.json"
}
teardown() { rm -rf "$MAIN" "$WT" "$STUB" 2>/dev/null || true; }

# Run the script in $MAIN with gh stubbed (real PATH preserved for git/awk/etc).
prune() { ( cd "$MAIN" && PATH="$STUB:$PATH" bash "$SCRIPT" "$@" ); }

# Backdate a worktree's HEAD, index and HEAD reflog by <hours>.
age_worktree() {  # <worktree> <hours>
    local gd; gd="$(git -C "$1" rev-parse --absolute-git-dir)"
    touch -d "@$(( $(date +%s) - $2 * 3600 ))" "$gd/HEAD" "$gd/index" "$gd/logs/HEAD"
}

@test "--selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]; [[ "$output" == *PASS* ]]
}

# NB: assert via filesystem existence ([ -d ]), not by grepping `git worktree
# list` paths — on Windows git reports C:/... while $WT is the MSYS /tmp/... form,
# so a path grep is unreliable (and an absent-grep would trivially pass).

@test "dry-run (default) does NOT remove the merged worktree" {
    run prune
    [ "$status" -eq 0 ]
    [[ "$output" == *"would-reap"*"feat/x"* ]]
    [ -d "$WT" ]
}

@test "--apply removes a MERGED + clean worktree and its branch" {
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"reaped"*"feat/x"* ]]
    [ ! -d "$WT" ]
    run git -C "$MAIN" branch --list feat/x
    [ -z "$output" ]
}

@test "--apply SKIPS a dirty merged worktree (preserves uncommitted work)" {
    echo dirty > "$WT/uncommitted.txt"
    git -C "$WT" add -A
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(dirty)"*"feat/x"* ]]
    [ -d "$WT" ]
}

@test "--apply SKIPS a merged worktree holding only UNTRACKED files (dirty, not FAILED)" {
    # `git worktree remove` refuses untracked files, so before they counted as
    # dirty this surfaced as a FAILED reap with rc=1.
    echo scratch > "$WT/untracked.txt"
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(dirty)"*"feat/x"* ]]
    [[ "$output" != *"FAILED"* ]]
    [ -f "$WT/untracked.txt" ]
}

@test "a merged worktree touched inside the idle threshold is skipped as active" {
    age_worktree "$WT" 2
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(active)"*"feat/x"* ]]
    [ -d "$WT" ]
    # A tighter threshold makes the same worktree idle enough to reap.
    run prune --apply --idle-hours 1
    [ "$status" -eq 0 ]
    [[ "$output" == *"reaped"*"feat/x"* ]]
    [ ! -d "$WT" ]
}

# selftest: asserts-failure — the data-loss case: a clean, idle, MERGED worktree
# whose branch carries a committed-but-unpushed follow-up commit. Reaping ran
# `git branch -D` and destroyed that commit; the tip guard must skip it.
@test "--apply SKIPS a merged worktree whose HEAD moved past the merged PR head" {
    ( cd "$WT" && echo follow-up > f && git add f && git commit -qm "follow-up after merge" )
    local follow; follow="$(git -C "$WT" rev-parse HEAD)"
    [ "$follow" != "$PR_HEAD_OID" ]
    age_worktree "$WT" 48
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(moved)"*"feat/x"* ]]
    [[ "$output" == *"reaped=0  skipped=1"* ]]
    [ -d "$WT" ]
    [ "$(git -C "$MAIN" rev-parse feat/x)" = "$follow" ]
    # Dry-run reports the same skip, never a would-reap.
    run prune
    [[ "$output" == *"skip(moved)"*"feat/x"* ]]
    [[ "$output" == *"would-reap=0  skipped=1"* ]]
}

@test "a merged worktree is skipped as moved when the PR head is unknown" {
    printf '#!/usr/bin/env bash\nprintf "feat/x\\tMERGED\\n"\n' > "$STUB/gh"
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"skip(moved)"*"feat/x"* ]]
    [ -d "$WT" ]
}

@test "--idle-hours 0 turns the idle guard off" {
    age_worktree "$WT" 0
    run prune --idle-hours 0
    [ "$status" -eq 0 ]
    [[ "$output" == *"would-reap"*"feat/x"* ]]
    run prune --idle-hours=0
    [ "$status" -eq 0 ]
    [[ "$output" == *"would-reap"*"feat/x"* ]]
}

@test "--idle-hours rejects a non-number" {
    run prune --idle-hours soon
    [ "$status" -eq 2 ]
    [[ "$output" == *"whole number"* ]]
}

@test "a branch in project.config.json vcs.protected_branches is never reaped" {
    printf '{"vcs": {"protected_branches": ["feat/x"]}}\n' > "$PC_CONFIG_FILE"
    run prune --apply
    [ "$status" -eq 0 ]
    [[ "$output" != *"reaped"*"feat/x"* ]]
    [ -d "$WT" ]
    run git -C "$MAIN" branch --list feat/x
    [ -n "$output" ]
}

@test "the develop integration tree is never reaped" {
    run prune --apply
    [ "$status" -eq 0 ]
    [ -d "$MAIN/.git" ]
}

# --- --branches: mid-session local-branch prune (tooling 2026-05-30) ----------
# One branch of each kind next to the worktree-held feat/x: merged + free
# (the only deletable one), OPEN, no PR, protected by config, and merged but
# carrying a commit made after the merge.
branches_fixture() {
    local seed later b
    seed="$(git -C "$MAIN" rev-parse develop)"
    for b in feat/done feat/open feat/nopr asset-store; do git -C "$MAIN" branch "$b"; done
    later="$(git -C "$MAIN" commit-tree "develop^{tree}" -p develop -m after-merge)"
    git -C "$MAIN" branch feat/moved "$later"
    {
        printf '#!/usr/bin/env bash\ncat <<"PRS"\n'
        printf '%s\tMERGED\t%s\n' feat/x "$seed" feat/done "$seed" feat/moved "$seed" asset-store "$seed"
        printf 'feat/open\tOPEN\t%s\n' "$seed"
        printf 'PRS\n'
    } > "$STUB/gh"
}
has_branch() { [ -n "$(git -C "$MAIN" branch --list "$1")" ]; }

@test "--branches dry-run names only the merged branch no worktree holds" {
    branches_fixture
    run prune --branches
    [ "$status" -eq 0 ]
    [[ "$output" == *"would-delete  feat/done"* ]]
    [[ "$output" == *"skip(moved)   feat/moved"* ]]
    [[ "$output" != *"feat/x"* ]]
    [[ "$output" != *"feat/open"* ]]
    [[ "$output" != *"feat/nopr"* ]]
    [[ "$output" != *"asset-store"* ]]
    [[ "$output" == *"would-delete=1  skip-moved=1"* ]]
    for b in feat/done feat/moved feat/x feat/open feat/nopr asset-store; do has_branch "$b"; done
}

@test "--branches --apply deletes it and leaves held / OPEN / no-PR / protected / moved alone" {
    branches_fixture
    run prune --branches --apply
    [ "$status" -eq 0 ]
    [[ "$output" == *"deleted       feat/done"* ]]
    run has_branch feat/done
    [ "$status" -ne 0 ]
    for b in feat/moved feat/x feat/open feat/nopr asset-store develop; do has_branch "$b"; done
    [ -d "$WT" ]   # branch mode never touches a worktree
}

# --- cmd_resync self-filter (finding #1958) ---------------------------------
# resync used to rewrite EVERY registry entry unconditionally, so running it in
# the shared integration tree silently re-baselined other LIVE sessions and blinded
# guard-head-drift.sh for them. These pin the three modes; the safety property is
# that a LIVE sibling is never clobbered without --all.

# worktree.sh derives REPO_ROOT from its OWN directory ($SCRIPT_DIR/../..), not
# from cwd — so invoking the repo copy from inside a temp tree would target the
# REAL repo and rewrite the developer's live session registry. Install a copy
# INSIDE the fixture so REPO_ROOT resolves to it. (Caught when the first cut of
# these tests did exactly that.)
resync_script() {  # <tree> — path to a worktree.sh whose REPO_ROOT is <tree>
    mkdir -p "$1/scripts/dev" "$1/agents/scripts/core"
    cp "$REPO_ROOT/scripts/dev/worktree.sh" "$1/scripts/dev/worktree.sh"
    cp "$LAYER_ROOT/agents/scripts/core/session-registry-lib.sh" "$1/agents/scripts/core/" 2>/dev/null || true
    printf '%s/scripts/dev/worktree.sh' "$1"
}

resync_seed() {   # <tree>  — own entry + a live sibling + a dead-and-stale sibling
    # Pin the POSIX liveness branch (as session_registry / plan_lock_gate /
    # guard_plan_lock all do). `live-sib` carries ppid=$$ — an AUTHORITATIVE pid
    # (>4), so sr_entry_is_live trusts the pid and ignores the fresh ts. On the
    # Windows branch that pid is checked against a `claude.exe` tasklist
    # snapshot, which a bats bash never appears in, so on git-bash the live
    # sibling would read DEAD, get rewritten, and fail the #1958 safety
    # assertion. `kill -0 $$` is unambiguously live everywhere.
    export SMATCHET_REGISTRY_OS=posix
    local d="$1/.claude/.active-sessions" now; now="$(date +%s)"
    mkdir -p "$d"
    printf 'branch=old\nsha=dead\nppid=%s\nts=%s\n' "$$" "$now"           > "$d/mine"
    printf 'branch=old\nsha=dead\nppid=%s\nts=%s\n' "$$" "$now"           > "$d/live-sib"
    printf 'branch=old\nsha=dead\nppid=999999\nts=%s\n' "$((now - 99999))" > "$d/dead-sib"
}
resync_branch_of() { sed -n 's/^branch=//p' "$1/.claude/.active-sessions/$2" | head -1; }

@test "resync with a known session id rewrites only the caller's own entry" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    ( cd "$MAIN" && CLAUDE_SESSION_ID=mine bash "$sc" resync ) >/dev/null 2>&1
    [ "$(resync_branch_of "$MAIN" mine)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "old" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "old" ]
}

# selftest: asserts-failure — a LIVE sibling must survive a no-session-id resync;
# the pre-fix code rewrote it, blinding the drift guard for that session.
@test "resync without a session id never clobbers a live sibling" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    run env -u CLAUDE_SESSION_ID -u SMATCHET_JANITOR_SELF_SESSION \
        bash -c "cd '$MAIN' && bash '$sc' resync"
    [ "$status" -eq 0 ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "old" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "develop" ]   # dead+stale is safe to take
    [[ "$output" == *"skipped (live sibling"* ]]
    [[ "$output" == *"--all"* ]]                             # names the escape hatch
}

@test "resync --all rewrites siblings and names each one" {
    local sc; sc="$(resync_script "$MAIN")"; resync_seed "$MAIN"
    run bash -c "cd '$MAIN' && CLAUDE_SESSION_ID=mine bash '$sc' resync --all"
    [ "$status" -eq 0 ]
    [ "$(resync_branch_of "$MAIN" mine)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" live-sib)" = "develop" ]
    [ "$(resync_branch_of "$MAIN" dead-sib)" = "develop" ]
    [[ "$output" == *"overwriting sibling entry"* ]]
}

# --- cmd_sync + the `new` slug cap (submodule provisioning) -----------------
# `sync` exists because a long-lived worktree only ever gets `git pull`, which
# never advances a submodule: once the agent surface lives in one, an established
# session would keep running yesterday's agent definitions. The re-wire is the
# half that must not be skippable — on Windows the adapter is hardlinked, so a
# submodule update alone leaves the OLD content visible through the link.

@test "sync <slug> refuses a slug with no worktree" {
    local sc; sc="$(resync_script "$MAIN")"
    run env SMATCHET_TREES_ROOT="$MAIN/trees" bash "$sc" sync no-such-tree
    [ "$status" -ne 0 ]
    [[ "$output" == *"No worktree at"* ]]
}

@test "sync fails loudly when the harness re-wire cannot run" {
    local sc; sc="$(resync_script "$MAIN")"
    # resync_script installs worktree.sh but no setup-harness.sh, so the re-wire
    # step has nothing to invoke. A silent skip here is the stale-definitions bug.
    run bash "$sc" sync
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refreshing submodules"* ]]   # submodules first, always
    [[ "$output" == *"setup-harness failed"* ]]
}

@test "new rejects an over-long slug before creating anything" {
    local sc long; sc="$(resync_script "$MAIN")"
    long="$(printf 'a%.0s' $(seq 41))"
    run env SMATCHET_TREES_ROOT="$MAIN/trees" bash "$sc" new "$long"
    [ "$status" -ne 0 ]
    [[ "$output" == *"41 characters"* ]]
    [ ! -d "$MAIN/trees/$long" ]
}
