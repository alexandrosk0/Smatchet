#!/bin/bash
# test-pre-push-merged-pr-guard: bucket-A unit test for the pre-push hook
# at scripts/git-hooks/pre-push. Mocks `gh` via PATH and `git rev-parse`
# via a shim function. Auto-enrolled by scripts/dev/test-all.sh.
#
# Covers:
#   1. develop branch -> exit 0 (allow)
#   2. main branch -> exit 0
#   3. HEAD detached -> exit 0
#   4. branch with no PR -> exit 0
#   5. branch with OPEN PR -> exit 0
#   6. branch with MERGED PR -> exit 1 + recovery banner
#   7. branch with CLOSED PR -> exit 1 + recovery banner
#   8. branch with MERGED PR + SMATCHET_ALLOW_MERGED_PR_PUSH=1 -> exit 0
#   9. gh not on PATH -> exit 0 (silent skip)
#  10. MERGED PR + refs/locks/<slug> DELETE -> exit 0 (lock-release.sh path)
#  11. MERGED PR + refs/heads/<branch> content push -> exit 1 (guard still bites)
#  12. MERGED PR + empty stdin -> exit 1 (fail-closed on unparseable records)
#  13. MERGED PR + refs/heads/<branch> DELETE -> exit 0 (post-merge branch delete)
#
# Plan: docs/plans/shipped/process-backlog-tighten-1-2-3-9-11-12.md § Slice 3
# Plan: docs/plans/shipped/pre-push-refspec-scope.md (cases 10-13, refspec scoping)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
HOOK="$REPO_ROOT/scripts/git-hooks/pre-push"

if [ ! -x "$HOOK" ]; then
    echo "FAIL: $HOOK is not executable"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Mock-gh helper: write a gh stub to a sandbox bin dir, prepend to PATH.
make_gh_stub() {
    local state="$1"   # OPEN | MERGED | CLOSED | NONE
    local stub_dir="$TMP/bin"
    mkdir -p "$stub_dir"
    cat > "$stub_dir/gh" <<EOF
#!/bin/bash
# Mocked gh — returns canned --json state response.
case "\$*" in
    *"--json state"*)
        if [ "$state" = "NONE" ]; then
            exit 1
        fi
        echo "$state"
        exit 0
        ;;
    *"--json number"*)
        echo "999"
        exit 0
        ;;
esac
echo "stub-gh: unhandled args: \$*" >&2
exit 1
EOF
    chmod +x "$stub_dir/gh"
    echo "$stub_dir"
}

# Mock-git-branch: run the hook with `git rev-parse --abbrev-ref HEAD`
# overridden via a custom git wrapper in PATH.
make_git_stub() {
    local branch="$1"
    local stub_dir="$TMP/git-bin"
    mkdir -p "$stub_dir"
    # Find the real git so the wrapper can delegate non-rev-parse calls.
    local real_git
    real_git="$(command -v git)"
    cat > "$stub_dir/git" <<EOF
#!/bin/bash
if [ "\$1" = "rev-parse" ] && [ "\$2" = "--abbrev-ref" ] && [ "\$3" = "HEAD" ]; then
    echo "$branch"
    exit 0
fi
exec "$real_git" "\$@"
EOF
    chmod +x "$stub_dir/git"
    echo "$stub_dir"
}

# Run the hook with the given gh-state + branch. Echoes "EXIT=$?" and any
# stderr to stdout for assertion.
#
# $4 (stdin_records) is the pre-push ref-update block git feeds on stdin —
# "<local_ref> <local_sha> <remote_ref> <remote_sha>" per line. It defaults to an
# ordinary content push of the branch under test, which is what every pre-case-10
# assertion means by "a push". Pass the literal string NONE for empty stdin.
run_hook() {
    local gh_state="$1"
    local branch="$2"
    local extra_env="${3:-}"
    local stdin_records="${4-}"

    local head_sha="1111111111111111111111111111111111111111"
    if [ -z "${4+set}" ]; then
        stdin_records="refs/heads/$branch $head_sha refs/heads/$branch $head_sha"
    elif [ "$stdin_records" = "NONE" ]; then
        stdin_records=""
    fi

    local gh_bin git_bin
    if [ "$gh_state" = "MISSING_GH" ]; then
        gh_bin=""
    else
        gh_bin="$(make_gh_stub "$gh_state")"
    fi
    git_bin="$(make_git_stub "$branch")"

    local sandbox_path="$git_bin"
    if [ -n "$gh_bin" ]; then
        sandbox_path="$gh_bin:$sandbox_path"
    fi
    # Append a minimal real PATH so basic utils still resolve.
    sandbox_path="$sandbox_path:/usr/bin:/bin"

    # Isolate the merged-PR guard (the unit under test) from the hook's two
    # sibling pre-gh probes, both of which depend on environment state this
    # stripped `env -i` sandbox does not provide and which therefore mis-fire
    # here (they own their own tests):
    #   - gate (D) local-CI delta-gate runs test-lint-rules.sh, which exits
    #     non-zero when its tooling (python/clang-format/...) is absent from the
    #     minimal PATH -> misread as a violation -> hook refuses early. Only
    #     present on a merge-ref checkout (refs/pull/N/merge), so the failure
    #     surfaces in PR CI but not on a head-only local run. -> SKIP_PRESHIP_GATE.
    #   - plan-lock probe (C) reads the *real* branch via `git symbolic-ref`
    #     (not the stubbed `git rev-parse`), so a real feature-branch checkout
    #     with plan-locked changed files can refuse before the guard runs.
    #     -> ALLOW_UNLOCKED_PUSH.
    #   - protected-branch stop (A) is stdin-driven and DOES fire on the
    #     develop/main cases below, whose default records push those refs;
    #     neutralised per the stage-neutraliser lint
    #     (test-pre-push-stage-neutralisers.sh). -> ALLOW_DEVELOP_PUSH.
    #   - review-verdict marker (E) keys on the REAL repo's HEAD, which
    #     legitimately has no marker during a test run. -> SKIP_REVIEW_MARKER.
    local out exit_code
    set +e
    out=$(env -i \
        PATH="$sandbox_path" \
        HOME="$HOME" \
        SMATCHET_ALLOW_DEVELOP_PUSH=1 \
        SMATCHET_SKIP_PRESHIP_GATE=1 \
        SMATCHET_ALLOW_UNLOCKED_PUSH=1 \
        SMATCHET_SKIP_REVIEW_MARKER=1 \
        SMATCHET_ALLOW_MERGED_PR_PUSH="${extra_env#SMATCHET_ALLOW_MERGED_PR_PUSH=}" \
        bash "$HOOK" <<<"$stdin_records" 2>&1)
    exit_code=$?
    set -e
    echo "$out"
    echo "EXIT=$exit_code"
}

# ----------------------------------------------------------------------------

PASS=0
FAIL=0

assert_exit() {
    local label="$1"
    local expected="$2"
    local got_output="$3"
    local actual
    actual=$(echo "$got_output" | grep -oE 'EXIT=[0-9]+' | tail -1 | cut -d= -f2)
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $label (exit=$actual)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label (expected exit=$expected, got exit=$actual)"
        echo "--- output ---"
        echo "$got_output"
        echo "--- end ---"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local label="$1"
    local needle="$2"
    local hay="$3"
    if echo "$hay" | grep -qF "$needle"; then
        echo "PASS: $label (contains \"$needle\")"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label (expected substring \"$needle\")"
        echo "--- output ---"
        echo "$hay"
        echo "--- end ---"
        FAIL=$((FAIL + 1))
    fi
}

# Test 1: develop branch -> allow.
out=$(run_hook OPEN develop)
assert_exit "1. develop branch allowed" 0 "$out"

# Test 2: main branch -> allow.
out=$(run_hook OPEN main)
assert_exit "2. main branch allowed" 0 "$out"

# Test 3: HEAD detached -> allow.
out=$(run_hook OPEN HEAD)
assert_exit "3. HEAD detached allowed" 0 "$out"

# Test 4: branch with no PR -> allow.
out=$(run_hook NONE feature-xyz)
assert_exit "4. branch with no PR allowed" 0 "$out"

# Test 5: branch with OPEN PR -> allow.
out=$(run_hook OPEN feature-xyz)
assert_exit "5. branch with OPEN PR allowed" 0 "$out"

# Test 6: branch with MERGED PR -> refuse with banner.
out=$(run_hook MERGED feature-xyz)
assert_exit "6a. branch with MERGED PR refused" 1 "$out"
assert_contains "6b. MERGED refusal banner present" "REFUSING push" "$out"
assert_contains "6c. recovery instructions present" "git checkout -b" "$out"

# Test 7: branch with CLOSED PR -> refuse.
out=$(run_hook CLOSED feature-xyz)
assert_exit "7. branch with CLOSED PR refused" 1 "$out"

# Test 8: MERGED PR + override env -> allow.
out=$(run_hook MERGED feature-xyz "SMATCHET_ALLOW_MERGED_PR_PUSH=1")
assert_exit "8a. SMATCHET_ALLOW_MERGED_PR_PUSH=1 override allows" 0 "$out"
assert_contains "8b. override warning logged" "proceeding anyway" "$out"

# Test 9: gh not on PATH -> silent allow.
out=$(run_hook MISSING_GH feature-xyz)
assert_exit "9. missing gh CLI -> silent allow" 0 "$out"

# --- Refspec scoping (docs/plans/shipped/pre-push-refspec-scope.md) --------
# The guard's premise is "commits the PR will never pick up". Only a NON-DELETE
# update to refs/heads/* can do that, so what is being pushed decides, not which
# branch happens to be checked out.

ZERO="0000000000000000000000000000000000000000"
SHA="2222222222222222222222222222222222222222"

# Test 10: MERGED PR + a refs/locks/<slug> DELETE -> allow. This is the
# lock-release.sh push (`git push origin :refs/locks/<slug>`), which runs from a
# just-merged checkout by design and carries no commits.
out=$(run_hook MERGED feature-xyz "" "(delete) $ZERO refs/locks/some-slug $SHA")
assert_exit "10a. MERGED PR + refs/locks delete allowed" 0 "$out"
if echo "$out" | grep -qF "REFUSING push"; then
    echo "FAIL: 10b. lock delete printed the merged-PR refusal banner"
    echo "--- output ---"; echo "$out"; echo "--- end ---"
    FAIL=$((FAIL + 1))
else
    echo "PASS: 10b. lock delete printed no merged-PR refusal banner"
    PASS=$((PASS + 1))
fi

# Test 11: MERGED PR + an ordinary refs/heads content push -> still refused.
out=$(run_hook MERGED feature-xyz "" "refs/heads/feature-xyz $SHA refs/heads/feature-xyz $ZERO")
assert_exit "11a. MERGED PR + branch content push refused" 1 "$out"
assert_contains "11b. refusal banner present" "REFUSING push" "$out"

# Test 12: MERGED PR + empty stdin -> refused (fail-closed). A real `git push`
# always supplies records; only a synthetic caller reaches this, and guessing
# "allow" there would silently widen the guard's escape surface.
out=$(run_hook MERGED feature-xyz "" "NONE")
assert_exit "12. MERGED PR + empty stdin refused (fail-closed)" 1 "$out"

# Test 13: MERGED PR + a refs/heads DELETE -> allow. Deleting the merged branch
# post-merge pushes no commits either.
out=$(run_hook MERGED feature-xyz "" "(delete) $ZERO refs/heads/feature-xyz $SHA")
assert_exit "13. MERGED PR + branch delete allowed" 0 "$out"

echo
echo "================================================================"
echo "test-pre-push-merged-pr-guard: $PASS passed, $FAIL failed"
echo "================================================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
