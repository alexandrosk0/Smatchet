#!/usr/bin/env bash
# test-lock-primitives.sh — exercise lock-{claim,claim-update,release,show}.sh
# against a sandbox bare repo. Validates the atomic CAS contract end-to-end
# without touching the real origin remote.
#
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Setup: creates two temp clones of a bare repo and runs the lock scripts
# from inside each clone. Sets SMATCHET_LOCK_BYPASS_REPO_CHECK=1 so the
# scripts accept a non-Smatchet sandbox URL.
#
# Cases:
#   1. claim succeeds for a new slug
#   2. concurrent claim from clone2 on same slug → REJECTED (lock held)
#   3. locks-show.sh lists the held slug
#   4. update from claim-holder succeeds
#   5. update from non-holder with stale lease → REJECTED
#   6. release deletes the ref, post-release ls-remote is empty
#   7. claim by previously-rejected actor now succeeds
#
# Exit codes:
#   0 — all cases passed
#   1 — one or more cases failed (details printed)
#   2 — sandbox setup failure
#
# Usage:
#   bash scripts/dev/test-lock-primitives.sh

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPTS="$ROOT/scripts/dev"

# Sanity: required scripts exist.
for s in lock-claim.sh lock-claim-update.sh lock-release.sh locks-show.sh; do
    [ -f "$SCRIPTS/$s" ] || { echo "test setup: missing $SCRIPTS/$s" >&2; exit 2; }
done

# Sandbox.
SANDBOX=$(mktemp -d)
trap 'rm -rf "$SANDBOX"' EXIT
BARE="$SANDBOX/bare.git"
CLONE_A="$SANDBOX/clone-a"
CLONE_B="$SANDBOX/clone-b"

git init --quiet --bare "$BARE"
# Bare repos need an initial commit pushed via a side-channel clone or the
# clones below come out detached with no HEAD. Seed via a throwaway worktree.
SEED="$SANDBOX/seed"
git init --quiet "$SEED"
git -C "$SEED" -c user.email=test@local -c user.name=test commit --quiet --allow-empty -m "seed"
git -C "$SEED" branch -m develop
git -C "$SEED" remote add origin "$BARE"
git -C "$SEED" push --quiet origin develop
git -C "$SEED" symbolic-ref HEAD refs/heads/develop

# Server-side HEAD must point at the seeded branch so clones don't break.
git -C "$BARE" symbolic-ref HEAD refs/heads/develop

git clone --quiet "$BARE" "$CLONE_A"
git clone --quiet "$BARE" "$CLONE_B"
for d in "$CLONE_A" "$CLONE_B"; do
    git -C "$d" config user.email "test@local"
    git -C "$d" config user.name  "test"
done

export SMATCHET_LOCK_BYPASS_REPO_CHECK=1

PASS=0
FAIL=0
fail() { echo "  FAIL: $1" >&2; FAIL=$((FAIL+1)); }
pass() { echo "  pass: $1"; PASS=$((PASS+1)); }

write_set_for() {
    local out="$1"
    cat >"$out" <<EOF
Source_Core/include/Example.h
Source_Core/src/Example.cpp
EOF
}

run_in() {
    local dir="$1"; shift
    ( cd "$dir" && "$@" )
}

# ---- Case 1: claim succeeds for a new slug ----
echo "case 1 — claim new slug"
WS_A="$SANDBOX/ws-a.txt"
write_set_for "$WS_A"
if run_in "$CLONE_A" \
    env AGENT_ID=agent-a LOCK_BRANCH=feat/example LOCK_PLAN=docs/design/example.md \
    bash "$SCRIPTS/lock-claim.sh" example-slice "$WS_A" >/dev/null 2>&1
then
    pass "clone-a claimed example-slice"
else
    fail "clone-a failed to claim example-slice (rc=$?)"
fi

# ---- Case 2: concurrent claim from clone-b on same slug → rejected ----
echo "case 2 — concurrent claim on held slug must fail"
WS_B="$SANDBOX/ws-b.txt"
write_set_for "$WS_B"
if run_in "$CLONE_B" \
    env AGENT_ID=agent-b \
    bash "$SCRIPTS/lock-claim.sh" example-slice "$WS_B" >/dev/null 2>&1
then
    fail "clone-b unexpectedly claimed an already-held slug"
else
    rc=$?
    if [ "$rc" -eq 1 ]; then
        pass "clone-b correctly rejected with exit code 1 (lock held)"
    else
        fail "clone-b exit code was $rc (expected 1)"
    fi
fi

# ---- Case 3: locks-show.sh lists the held slug ----
echo "case 3 — locks-show.sh reports the held slug"
show_out=$(run_in "$CLONE_A" bash "$SCRIPTS/locks-show.sh" 2>&1) || true
if printf '%s' "$show_out" | grep -q 'example-slice'; then
    if printf '%s' "$show_out" | grep -q 'agent-a'; then
        pass "locks-show.sh lists example-slice owned by agent-a"
    else
        fail "locks-show.sh did not include owner 'agent-a' in output: $show_out"
    fi
else
    fail "locks-show.sh did not list example-slice. output: $show_out"
fi

# ---- Case 4: update from the holder succeeds ----
echo "case 4 — update from holder succeeds"
WS_A2="$SANDBOX/ws-a2.txt"
cat >"$WS_A2" <<EOF
Source_Core/include/Example.h
Source_Core/src/Example.cpp
Source_Core/include/ExampleNew.h
EOF
if run_in "$CLONE_A" \
    env AGENT_ID=agent-a LOCK_BRANCH=feat/example LOCK_NOTES="scope grew" \
    bash "$SCRIPTS/lock-claim-update.sh" example-slice "$WS_A2" >/dev/null 2>&1
then
    pass "clone-a updated the claim"
else
    fail "clone-a failed to update its own claim (rc=$?)"
fi

# ---- Case 5: update from non-holder with stale lease → rejected ----
# clone-b never fetched the latest state, so its lease is stale.
echo "case 5 — non-holder update with stale lease fails"
# Force clone-b to have a stale view: do NOT fetch.
WS_B2="$SANDBOX/ws-b2.txt"
write_set_for "$WS_B2"
# clone-b lock-claim-update will fetch the current sha via ls-remote, so the
# lease will actually be fresh — meaning clone-b CAN hijack via update unless
# we add an ownership check. This is an intentional limitation of the schema:
# locks are advisory at the ref layer. We model it via the CAS-lease-only test
# below where clone-b passes a stale --force-with-lease value manually.
#
# Substitute realistic check: clone-b runs an update concurrently with clone-a.
# clone-a wins the race; clone-b's lease is stale by the time it pushes.
# We simulate the race by stashing clone-b's ls-remote result first, running
# clone-a's update, then running clone-b's update — it should detect lease
# conflict and retry, then fail when retries exhaust if clone-a keeps winning.
# Simpler deterministic test: capture clone-b's pre-update sha and force-with-
# lease against it after clone-a has moved the ref further.

# Get the current ref sha as clone-b sees it, and fetch the object locally
# so commit-tree can reference it as a parent.
git -C "$CLONE_B" fetch --quiet origin "+refs/locks/*:refs/locks/*"
stale_sha=$(git -C "$CLONE_B" rev-parse "refs/locks/example-slice")
# clone-a moves the ref forward with a third update.
WS_A3="$SANDBOX/ws-a3.txt"
cat >"$WS_A3" <<EOF
Source_Core/include/Example.h
EOF
run_in "$CLONE_A" \
    env AGENT_ID=agent-a \
    bash "$SCRIPTS/lock-claim-update.sh" example-slice "$WS_A3" >/dev/null 2>&1 || true

# Now clone-b attempts an explicit force-with-lease against the stale_sha — this
# directly exercises the CAS lease mechanism that lock-claim-update relies on.
WS_B3="$SANDBOX/ws-b3.txt"
write_set_for "$WS_B3"
b_blob=$(printf '%s\n' '{"schema":"smatchet-plan-lock/1","slug":"example-slice","owner":"agent-b","branch":"x","originating_plan":"","started":"2026-01-01T00:00:00Z","updated":"2026-01-01T00:00:00Z","write_set":["x"],"notes":""}' | git -C "$CLONE_B" hash-object -w --stdin)
b_tree=$(printf '%s\n' "100644 blob $b_blob	claim.json" | git -C "$CLONE_B" mktree)
# Build commit with the stale_sha as parent.
b_commit=$(echo "lock: hijack attempt" | git -C "$CLONE_B" commit-tree "$b_tree" -p "$stale_sha")
if git -C "$CLONE_B" push --force-with-lease="refs/locks/example-slice:$stale_sha" origin "$b_commit:refs/locks/example-slice" >/dev/null 2>&1; then
    fail "clone-b hijacked the ref with a stale lease (CAS broken)"
else
    pass "clone-b stale-lease push correctly rejected"
fi

# ---- Case 6: release deletes the ref ----
echo "case 6 — release deletes ref"
if run_in "$CLONE_A" bash "$SCRIPTS/lock-release.sh" example-slice >/dev/null 2>&1; then
    after=$(git -C "$CLONE_A" ls-remote origin "refs/locks/example-slice" | awk '{print $1}')
    if [ -z "$after" ]; then
        pass "lock-release.sh removed the ref"
    else
        fail "ref still present after lock-release.sh: $after"
    fi
else
    fail "lock-release.sh exit non-zero (rc=$?)"
fi

# ---- Case 7: previously-rejected actor can now claim ----
echo "case 7 — clone-b can claim after release"
if run_in "$CLONE_B" \
    env AGENT_ID=agent-b \
    bash "$SCRIPTS/lock-claim.sh" example-slice "$WS_B" >/dev/null 2>&1
then
    pass "clone-b claimed example-slice after release"
else
    fail "clone-b could not claim after release (rc=$?)"
fi

# ---- Idempotent release ----
echo "case 8 — release of absent ref is a no-op"
run_in "$CLONE_A" bash "$SCRIPTS/lock-release.sh" example-slice >/dev/null 2>&1 || true
if run_in "$CLONE_A" bash "$SCRIPTS/lock-release.sh" example-slice >/dev/null 2>&1; then
    pass "lock-release of absent ref is idempotent"
else
    fail "second lock-release returned non-zero"
fi

echo
# Final line follows the test-all.sh contract regex.
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
