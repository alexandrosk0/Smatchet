#!/usr/bin/env bash
# test-all.sh — run every test-*.sh across the test roots and aggregate Passed/Failed.
#
# Discovers tests by glob across scripts/dev/ + agents/scripts/{core,project}/
# (each root included only if present); test-*.sh, excludes self.
# Each test script must follow the test-author bash conventions:
#   - set -euo pipefail
#   - exits 0 on all-pass, 1 on assertion failure, 2 on missing binary/build.
#   - prints a final "Passed: N  Failed: M" line (regex-extracted here).
#
# Env overrides forwarded to children: SMATCHET_EXE, SMATCHET_TEST_PORT, PYTHON.
#
# Exit codes:
#   0 — every individual script reported Failed: 0
#   1 — at least one script reported Failed: >0 OR exited non-zero
#   2 — no test scripts found OR a script exited 2 (missing binary)
#
# Usage:
#   bash scripts/dev/test-all.sh                     # all tests
#   bash scripts/dev/test-all.sh --filter lua        # only test-*lua*.sh

set -euo pipefail

cd "$(dirname "$0")/../.."

# UTF-8 ctype locale for the bats runners. bats derives an internal function name
# from each `@test` description; under a non-UTF-8 ctype (the default on the
# Windows/MSYS dev box) bash can't parse a name containing non-ASCII (→, —) and
# bats SILENTLY skips it as "unknown test name" — the skipped test emits no
# ok/not-ok line, so the runner's Passed/Failed tally UNDER-REPORTS without
# failing: a false-green (tooling.md windows-bats-silently-skips-unicode-test-
# names; supersedes the 2026-05-28 merge_gates.bats LC_ALL note). CI runs no bats
# (the windows runner lacks it), so this pre-push aggregator is the chokepoint.
# Detect an available UTF-8 locale (name varies: C.UTF-8 on Linux, C.utf8 on
# MSYS) and export it for every enrolled runner; -ix tolerates the case/dash
# spelling differences. No-op when one is already set.
if [ -z "${LC_ALL:-}" ]; then
    for _u in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8; do
        if locale -a 2>/dev/null | grep -qix "$_u"; then
            export LC_ALL="$_u" LANG="$_u"
            break
        fi
    done
fi

# Worktree detection: when running from a worktree under .claude/worktrees/<id>/,
# some test scripts (lint-hook splits, ui-test scripts with batched PATH) report
# false-positive failures that pass cleanly when re-run on main repo. Skip them
# with a CLEAR `SKIPPED (worktree)` line so CI signal doesn't erode.
# git-common-dir and git-dir agree when on main; diverge inside a worktree.
SMATCHET_IS_WORKTREE=0
if [ "$(git rev-parse --git-common-dir 2>/dev/null)" != "$(git rev-parse --git-dir 2>/dev/null)" ]; then
    SMATCHET_IS_WORKTREE=1
fi
# Scripts that are known to be worktree-incompatible (per
# docs/self-improvement/categories/tooling.md "test-all.sh baseline drift
# across worktrees"). Extend this list as new worktree-only failures surface.
#
# Re-audit 2026-06-14 (testing-surface.md Gap 7): trimmed 3 dead alternatives
# whose scripts no longer exist (test-lint-hook-split, test-ui-callstack-tooltip,
# test-ui-ai-assistant). The 3 survivors are bucket-E UI drivers needing a built
# UI binary + display under Mesa; their skip guards real worktree baseline-drift
# and is orthogonal to the #1166 GIT_EXEC_PATH cold-configure fix (so still
# justified — not removable by that fix).
WORKTREE_INCOMPATIBLE_RE='(test-ui-views-columns-reorder|test-ui-funcsize-window-render-smoke|test-ui-funcsize-grid-render)'

FILTER=""
if [ "${1:-}" = "--filter" ]; then
    FILTER="${2:-}"
    shift 2
fi

# --ci mode (also via SMATCHET_TESTALL_CI=1): the agentic-selftests CI lane runs
# this aggregator on a headless Linux runner with bats + shellcheck + python but
# NO built Smatchet.exe, no Perforce, no live `gh`/network. In that lane a script
# that exits 2 (missing binary/build) is a clean SKIP, not a fatal — so the
# deterministic agentic suite (locks / guards / merge-gate logic / session
# registry / plan + doc + contract gates) gates a PR while exe/service-bound
# scripts step aside. A documented denylist additionally skips suites that need a
# service unavailable in CI or are tracked as failing pending a fix (see
# docs/self-improvement for the open follow-ups). Default (pre-push) behaviour is
# unchanged: exit 2 stays fatal and nothing is denylisted.
TESTALL_CI="${SMATCHET_TESTALL_CI:-0}"
if [ "${1:-}" = "--ci" ]; then TESTALL_CI=1; shift; fi
# Suites skipped in the CI lane. Each entry needs a reason; trim as follow-ups land.
#   p4-dual-vcs — needs a p4d server (self-skips anyway; listed for clarity).
#   doctor — dev-env doctor demanding the Windows C++ toolchain (cl.exe/clang-cl).
#   plan-index / docs — need full git history (shallow-clone drifts); already
#     gated on full history by doc-validation.yml (no coverage loss here).
#   guard-head-drift-bats / guard-plan-lock-bats / session-registry-bats — one
#     assertion each is sensitive to the headless CI checkout (git worktree /
#     PID-liveness / run-as-non-root) in a way that does not reproduce locally
#     (retried: non-root user + fresh detached-HEAD clone still all-green);
#     tracked for hardening + un-skip (the rest of each suite passes).
# Retired (now green headless): merge-watcher-bats + merge-watcher-integration-bats
# (POSIX watcher-root sandboxing + seam stubs), pr-status-watch-bats (fixed by the
# block-on-any-red rework), lint-rules-bats (fixture made a genuine blank-run),
# bucket-lane-launch-smoke-bats (stub emits a real decodable PNG).
CI_SKIP_RE='(test-p4-dual-vcs|test-doctor|test-plan-index|test-docs|test-guard-head-drift-bats|test-guard-plan-lock-bats|test-session-registry-bats)'

# Collect test scripts across all roots that hold them. Product/build tests
# stay under scripts/dev/; the agentic test-* suites live under
# agents/scripts/{core,project}/ after the script-split (see
# docs/plans/shipped/split-scripts-build-vs-agentic.md). Each root is searched
# ONLY if it exists, so the code repo still runs standalone when the agents/
# tree is later extracted, and the agents repo can ship without a dangling
# scripts/dev reference.
TEST_ROOTS=(scripts/dev agents/scripts/core agents/scripts/project)
SEARCH_ROOTS=()
for root in "${TEST_ROOTS[@]}"; do
    [ -d "$root" ] && SEARCH_ROOTS+=("$root")
done
mapfile -t TESTS < <(find "${SEARCH_ROOTS[@]}" -maxdepth 1 -type f -name 'test-*.sh' ! -name 'test-all.sh' | sort)

if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "no tests found under {${TEST_ROOTS[*]}}/test-*.sh" >&2
    exit 2
fi

if [ -n "$FILTER" ]; then
    FILTERED=()
    for t in "${TESTS[@]}"; do
        case "$t" in
            *"$FILTER"*) FILTERED+=("$t") ;;
        esac
    done
    TESTS=("${FILTERED[@]}")
    if [ "${#TESTS[@]}" -eq 0 ]; then
        echo "no tests matched --filter $FILTER" >&2
        exit 2
    fi
fi

TOTAL_PASSED=0
TOTAL_FAILED=0
FAILED_SCRIPTS=()
MISSING_BINARY=0
SUSPECT_ZERO_SCRIPTS=()

for script in "${TESTS[@]}"; do
    # Skip worktree-incompatible scripts when running from a worktree (see
    # SMATCHET_IS_WORKTREE detection above). Emit a clear SKIPPED line so the
    # operator sees what was deliberately skipped rather than silently passed.
    if [ "$SMATCHET_IS_WORKTREE" -eq 1 ] && [[ "$script" =~ $WORKTREE_INCOMPATIBLE_RE ]]; then
        echo
        echo "##################################################"
        echo "# $script"
        echo "##################################################"
        echo "SKIPPED (worktree): this script is known to false-positive under worktree dispatch"
        echo "Passed: 0  Failed: 0  Skipped: 1"
        continue
    fi
    # CI-lane denylist: a suite needing a service unavailable on the headless
    # runner (Perforce, live gh) or tracked as failing pending a fix. Matched on the
    # EXACT basename (^name$) — the old unanchored substring match silently skipped any
    # future suite whose name merely embeds a denylist token (test-docs-foo.sh vs the
    # denylisted test-docs.sh; test-plan-index-robustness-bats.sh vs test-plan-index.sh — a
    # pure-logic bats that was wrongly skipped and now runs). Fail-open closed (#1774).
    script_base="${script##*/}"
    script_base="${script_base%.sh}"
    if [ "$TESTALL_CI" -eq 1 ] && [[ "$script_base" =~ ^$CI_SKIP_RE$ ]]; then
        echo
        echo "##################################################"
        echo "# $script"
        echo "##################################################"
        echo "SKIPPED (ci): denylisted — needs a service unavailable in CI or tracked-pending-fix (see CI_SKIP_RE)"
        echo "Passed: 0  Failed: 0  Skipped: 1"
        continue
    fi
    echo
    echo "##################################################"
    echo "# $script"
    echo "##################################################"

    # Reset RC before every iteration. Without this, the `|| RC=$?` on the
    # OUT capture line only sets RC on FAILURE — once any script fails, RC
    # stays at its last failing value and `RC="${RC:-0}"` is a no-op
    # (parameter expansion default only fires on UNSET, not on a stale
    # nonzero value). The result was every script after the first failure
    # being misreported as failed in the final summary.
    RC=0
    # Capture output but stream to terminal too.
    OUT=$(bash "$script" 2>&1) || RC=$?
    echo "$OUT"

    if [ "$RC" -eq 2 ]; then
        # In the CI lane a missing binary/build is a clean skip (no exe is built
        # on the agentic-selftests runner) — only genuine assertion failures
        # (exit 1) should red the lane. Pre-push keeps exit 2 fatal.
        if [ "$TESTALL_CI" -eq 1 ]; then
            echo "SKIPPED (ci): exit 2 — missing binary/build, not available on the agentic-selftests runner"
            continue
        fi
        MISSING_BINARY=1
        FAILED_SCRIPTS+=("$script (missing binary/build, exit=2)")
        continue
    fi

    # Extract Passed: / Failed: from the summary line. Accept a leading prefix
    # (`test-foo — Passed: 27  Failed: 0`) as well as the bare `Passed: …` form —
    # several conforming scripts label their summary, and anchoring to `^Passed:`
    # mis-flagged them as failures (surfaced once the aggregator started running
    # in CI; they exit 0 and are genuinely passing).
    SUMMARY=$(echo "$OUT" | grep -E 'Passed: [0-9]+ +Failed: [0-9]+' | tail -n 1 || true)
    if [ -z "$SUMMARY" ]; then
        # No parseable count. The test-author contract makes exit 0 the
        # authoritative all-pass signal (many checks print `… PASS — …` with no
        # numeric tally), so treat exit 0 as a pass and only a non-zero exit as a
        # real failure.
        if [ "$RC" -eq 0 ]; then
            continue
        fi
        FAILED_SCRIPTS+=("$script (no summary line, exit=$RC)")
        TOTAL_FAILED=$((TOTAL_FAILED + 1))
        continue
    fi

    P=$(echo "$SUMMARY" | sed -E 's/.*Passed: ([0-9]+).*/\1/')
    F=$(echo "$SUMMARY" | sed -E 's/.*Failed: ([0-9]+).*/\1/')
    TOTAL_PASSED=$((TOTAL_PASSED + P))
    TOTAL_FAILED=$((TOTAL_FAILED + F))
    # Suspicious zero-run guard (defence-in-depth, WARN-first — the
    # ci-falsepositive-hardening "per-runner unknown-test-name -> fail" item, deferred
    # there as a non-goal follow-up). A suite that RAN but reports 0 passed / 0 failed
    # with no `Skipped:` marker may have silently skipped all its cases (e.g. bats
    # "unknown test name" under a non-UTF-8 locale — tooling.md
    # windows-bats-silently-skips-unicode-test-names). Explicit worktree/CI skips
    # `continue` earlier WITH a `Skipped:` marker, so they never reach here. WARN only
    # (a delta-gate with no changed inputs legitimately reports 0/0); surfaced in the
    # aggregate for the operator to verify.
    if [ "$P" -eq 0 ] && [ "$F" -eq 0 ] && ! grep -q "Skipped:" <<<"$SUMMARY"; then
        echo "WARN (zero-run): reported 0 passed / 0 failed, no Skipped marker — verify it actually ran (possible silent skip)."
        SUSPECT_ZERO_SCRIPTS+=("$script")
    fi
    if [ "$F" -gt 0 ] || [ "$RC" -ne 0 ]; then
        FAILED_SCRIPTS+=("$script ($F assertion failure(s), exit=$RC)")
    fi
done

echo
echo "============================================================"
echo "AGGREGATE  Passed: $TOTAL_PASSED  Failed: $TOTAL_FAILED  Scripts: ${#TESTS[@]}"
echo "============================================================"

if [ "${#SUSPECT_ZERO_SCRIPTS[@]}" -gt 0 ]; then
    echo
    echo "WARN (zero-run, advisory): ${#SUSPECT_ZERO_SCRIPTS[@]} suite(s) reported 0/0 with no Skipped marker — possible silent skip; verify, then add an explicit skip marker or fix the cause:"
    printf '  - %s\n' "${SUSPECT_ZERO_SCRIPTS[@]}"
fi

if [ "$MISSING_BINARY" -eq 1 ]; then
    echo
    echo "Missing binary — rebuild and retry:"
    echo "  cmake --build --preset ninja-iter-msvc --target SmatchetStandalone"
    exit 2
fi

if [ "${#FAILED_SCRIPTS[@]}" -gt 0 ]; then
    echo
    echo "Failed scripts:"
    for f in "${FAILED_SCRIPTS[@]}"; do
        echo "  - $f"
    done
    exit 1
fi

exit 0
