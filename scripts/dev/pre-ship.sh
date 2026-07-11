#!/usr/bin/env bash
# scripts/dev/pre-ship.sh — one-shot pre-push gate for a feature branch.
#
# Closes the recurring "build-green != lint-green" gap (self-improvement
# process/P2): a targeted `cmake --build` / `ctest` does NOT run the comment-noise,
# function-size, or strict-zone delta gates — only test-all.sh and CI do. Authors
# (and decomposition subagents) repeatedly shipped build-green branches that failed
# CI on a bare-// comment-blank-run or a clang-format-reflowed comment.
#
# This wrapper enforces the correct ORDER: format FIRST, then run the exact delta
# gate CI runs — so a comment that clang-format reflows into a flagged shape is
# caught locally, not three minutes later in CI.
#
# Steps, over the first-party C++ files changed vs the base ref:
#   0. git add --intent-to-add untracked files   (so diff/grep gates see them)
#   1. clang-format -i   (apply formatting in place)
#   2. test-lint-rules.sh --diff <base>   (comment-noise + function-size + strict-zone)
#   3. md_lint / test-list consistency / test-docs.sh (doc-validation CI mirror)
#
# It deliberately does NOT build or run tests — that is the caller's job and is
# already covered once-per-slice. This is purely the lint half that CI gates on
# but local builds skip.
#
# Usage:
#   bash scripts/dev/pre-ship.sh [<base-ref>]   # default base: origin/develop
#   bash scripts/dev/pre-ship.sh --help
#
# Exit codes:
#   0 — formatting applied (if any) and the delta lint gate passed.
#   1 — the delta lint gate reported a failure (fix before pushing).
#   2 — usage error, or a required tool (git / clang-format) is unavailable.
#
# selftest: asserts-failure
set -euo pipefail

usage() {
    cat <<'USAGE'
pre-ship.sh — format changed C++ then run the CI delta lint gate locally.

  bash scripts/dev/pre-ship.sh [<base-ref>]              default base: origin/develop
  bash scripts/dev/pre-ship.sh --ack-review [<base-ref>] record a code-review ack for the diff
  bash scripts/dev/pre-ship.sh --help

Runs, over first-party C++ changed vs <base-ref>:
  1. clang-format -i
  2. agents/scripts/project/test-lint-rules.sh --diff <base-ref>
  3. markdown lint + test-list consistency + doc-validation suite
  4. code-review gate — a SUBSTANTIVE C++ diff (strict-zone touch or
     >= REVIEW_LINE_THRESHOLD changed lines, default 60) must have a current review
     ack. Run the code-review skill/agent, then `--ack-review` to record it; any later
     edit re-arms the gate. Bypass: SMATCHET_SKIP_REVIEW_GATE=1 (logged).
USAGE
}

# --selftest: prove the review gate BLOCKS (asserts at least one failure case), acks,
# re-arms on edit, and bypasses — in a throwaway git repo, with the lint stages skipped
# (SMATCHET_PRESHIP_GATE_ONLY=1 exercises ONLY the review gate; the lint stages have
# their own gates/selftests). Auto-enrolled by agents/scripts/core/test-gate-selftests.sh.
run_selftest() {
    local script_path tmp tmp2
    script_path="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    tmp="$(mktemp -d)"
    tmp2="$(mktemp -d)"
    # shellcheck disable=SC2064  # expand $tmp/$tmp2 now
    trap "rm -rf '$tmp' '$tmp2'" RETURN
    (
        set -e
        cd "$tmp"
        git init -q -b base .
        git config user.email selftest@local
        git config user.name selftest
        printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > project.config.json
        mkdir -p Source/Core/src/Sync
        echo "// base" > Source/Core/src/Sync/SelfTest.cpp
        git add -A && git commit -qm base
        git checkout -qb feature
        printf '// edit\nint self_test_fn() { return 1; }\n' >> Source/Core/src/Sync/SelfTest.cpp
        git commit -aqm edit
    )
    # 1. Substantive (strict-zone) diff, no ack -> MUST FAIL (the asserted failure case).
    if (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — review gate passed a strict-zone diff with NO ack" >&2
        return 1
    fi
    # 2. Ack -> MUST PASS.
    if ! (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" --ack-review base >/dev/null 2>&1 &&
        SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — gate still blocks after --ack-review" >&2
        return 1
    fi
    # 3. Edit after ack -> fingerprint stale -> MUST FAIL again (re-arm).
    (cd "$tmp" && echo "// post-ack edit" >> Source/Core/src/Sync/SelfTest.cpp)
    if (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — gate did not re-arm after a post-ack edit" >&2
        return 1
    fi
    # 4. Documented bypass -> MUST PASS (and is the operator's emergency escape).
    if ! (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 SMATCHET_SKIP_REVIEW_GATE=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — SMATCHET_SKIP_REVIEW_GATE=1 bypass broken" >&2
        return 1
    fi
    # 5. #1116 fail-CLOSED — strict-zone detection needs a WORKING python; without one it must
    #    not silently skip and let a strict-zone diff slip the gate. Use a fresh repo with a
    #    SMALL NON-strict C++ diff (normally N/A-passes) and prove FORCE_NO_PY flips it to block.
    (
        set -e
        cd "$tmp2"
        git init -q -b base .
        git config user.email selftest@local
        git config user.name selftest
        printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > project.config.json
        mkdir -p Source/Core/src/Ui
        echo "// base" > Source/Core/src/Ui/SelfTestUi.cpp
        git add -A && git commit -qm base
        git checkout -qb feature
        printf '// edit\nint self_ui_fn() { return 2; }\n' >> Source/Core/src/Ui/SelfTestUi.cpp
        git commit -aqm edit
    )
    # 5a. baseline — WITH python this small non-strict diff is NOT substantive -> MUST PASS.
    if ! (cd "$tmp2" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — a small non-strict diff should N/A-pass the review gate" >&2
        return 1
    fi
    # 5b. #1116 — the SAME diff with NO working python must fail CLOSED (strict membership
    #     undetectable -> require review), not N/A-pass. This is the exact fail-open the old
    #     `command -v python3` + swallowed-exit path allowed on the Windows py-stub.
    if (cd "$tmp2" && SMATCHET_PRESHIP_GATE_ONLY=1 SMATCHET_PRESHIP_FORCE_NO_PY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — #1116 fail-open: strict detection skipped and the diff N/A-passed with no working python" >&2
        return 1
    fi
    echo "pre-ship --selftest: PASS — gate blocks unacked substantive diffs, acks, re-arms on edit, bypass works, #1116 fail-closed on no-python."
    return 0
}

base_ref="origin/develop"
ack_review=0
case "${1:-}" in
    --help | -h)
        usage
        exit 0
        ;;
    --selftest)
        run_selftest
        exit $?
        ;;
    --ack-review)
        ack_review=1
        [ -n "${2:-}" ] && base_ref="$2"
        ;;
    --ack-review=*)
        ack_review=1
        base_ref="${1#--ack-review=}"
        if [ -z "$base_ref" ]; then
            echo "pre-ship: --ack-review= requires a non-empty base ref (or use bare --ack-review for origin/develop)" >&2
            exit 2
        fi
        ;;
    "") ;;
    *) base_ref="$1" ;;
esac

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "pre-ship: not inside a git work tree" >&2
    exit 2
}
cd "$repo_root"

# --- Code-review gate helpers (process self-improvement 2026-06-10) -------------------
# A substantive C++ diff must be code-reviewed before push. We cannot verify an LLM/human
# review actually ran, but we CAN pin an explicit acknowledgement to the EXACT diff content:
# `--ack-review` records a fingerprint of the changed first-party C++ vs <base-ref>; the gate
# then fails unless the current fingerprint still matches. Any later edit changes the
# fingerprint, invalidating the ack and forcing a conscious re-review of what will be pushed.
review_marker="$repo_root/.review-ack"
# The C++ trees whose diff content the fingerprint covers (mirrors the format-target globs).
review_cpp_globs=(
    'Source/Core/*.cpp' 'Source/Core/*.h'
    'Source/Plugins/*.cpp' 'Source/Plugins/*.h'
    'Source/Standalone/*.cpp' 'Source/Standalone/*.h'
    'tests/*.cpp' 'tests/*.h'
)
review_fingerprint() {
    # Committed-on-branch diff + working-tree diff, restricted to first-party C++. Hashing the
    # diff (not just file list) means any content change — incl. a clang-format reflow — re-arms.
    {
        git diff "$base_ref"...HEAD -- "${review_cpp_globs[@]}" 2>/dev/null || true
        git diff HEAD -- "${review_cpp_globs[@]}" 2>/dev/null || true
    } | sha256sum | cut -d' ' -f1
}

# Resolve a WORKING python interpreter (empty if none). `command -v python3` alone is
# insufficient on Windows: the python3 "App Execution Alias" stub passes `command -v` but
# exits 49 ("Python was not found") when actually run — so probe each candidate by executing
# it. Mirror of resolve_python in agents/scripts/project/lint-rules.d/00-common.sh (kept local
# so this top-level script stays independent of the lint-rules internals).
resolve_python() {
    local cand p
    for cand in python3 python py; do
        p="$(command -v "$cand" 2>/dev/null)" || continue
        if "$p" -c "" >/dev/null 2>&1; then printf '%s\n' "$p"; return 0; fi
    done
    return 1
}
PRESHIP_PY="$(resolve_python || true)"
# Selftest hook: force the "no working python" branch so the #1116 fail-closed path is
# testable in an environment that DOES have python. Never set this in normal use.
[ "${SMATCHET_PRESHIP_FORCE_NO_PY:-0}" = "1" ] && PRESHIP_PY=""

# SMATCHET_PRESHIP_GATE_ONLY=1 (selftest hook): skip the lint stages and run ONLY the
# code-review gate. Never set this in normal use — the lint stages are the point.
gate_only="${SMATCHET_PRESHIP_GATE_ONLY:-0}"

if [ "$gate_only" != "1" ]; then

    if ! command -v clang-format >/dev/null 2>&1; then
        echo "pre-ship: clang-format not found on PATH" >&2
        exit 2
    fi

# Untracked files are INVISIBLE to every git-diff- and git-grep-based gate below
# (`git diff <base>` skips them; `git grep` scans tracked only). Running pre-ship
# before the first `git add` therefore false-passed comment-noise and
# plan-ref-integrity on brand-new files (PR #953 — two CI-only failures). Intent-
# to-add registers them in the index (content stays unstaged) so the gates see
# exactly what CI will see.
mapfile -t untracked < <(git ls-files --others --exclude-standard)
if [ "${#untracked[@]}" -gt 0 ]; then
    echo "pre-ship: git add --intent-to-add ${#untracked[@]} untracked file(s) so gates can see them"
    git add --intent-to-add -- "${untracked[@]}"
    # Undo the ita registrations on EVERY exit (pass or fail) — leaving them
    # would make scratch files commit-eligible via a later `git commit -a` and
    # flip their `git status` bucket from untracked to modified (CR-964 review).
    # shellcheck disable=SC2064  # expand ${untracked[@]} NOW, not at trap time
    trap "git restore --staged -- $(printf '%q ' "${untracked[@]}") 2>/dev/null || true" EXIT
fi

# First-party C++ changed vs the merge-base with <base-ref> (staged, unstaged, and
# committed-on-branch). Restrict to the trees the gate scans; skip deletions.
mapfile -t changed < <(
    git diff --name-only --diff-filter=d "$base_ref"...HEAD -- \
        'Source/Core/*.cpp' 'Source/Core/*.h' \
        'Source/Plugins/*.cpp' 'Source/Plugins/*.h' \
        'Source/Standalone/*.cpp' 'Source/Standalone/*.h' \
        'tests/*.cpp' 'tests/*.h' 2>/dev/null
    git diff --name-only --diff-filter=d -- \
        'Source/Core/*.cpp' 'Source/Core/*.h' \
        'Source/Plugins/*.cpp' 'Source/Plugins/*.h' \
        'Source/Standalone/*.cpp' 'Source/Standalone/*.h' \
        'tests/*.cpp' 'tests/*.h' 2>/dev/null
)

# Deduplicate while preserving order, keeping only paths that still exist.
declare -A seen
fmt_targets=()
for f in "${changed[@]}"; do
    [ -n "$f" ] || continue
    [ -n "${seen[$f]:-}" ] && continue
    seen[$f]=1
    [ -f "$f" ] && fmt_targets+=("$f")
done

if [ "${#fmt_targets[@]}" -eq 0 ]; then
    echo "pre-ship: no changed first-party C++ vs $base_ref — formatting skipped"
else
    echo "pre-ship: clang-format -i on ${#fmt_targets[@]} changed file(s)"
    clang-format -i "${fmt_targets[@]}"
fi

# Auto-strip the mechanically-removable comment-noise this change added (blank-comment runs +
# decorative banners) BEFORE the gate — and AFTER clang-format, which can itself reflow a comment
# into a flagged shape. Commented-out-code is NOT auto-deleted (needs a human reword); it's
# reported and still blocks the gate below. This kills the recurring comment-blank-run footgun
# (postmortem: 6 PRs in one session re-tripped it on bare `//` header-doc separators).
echo "pre-ship: auto-stripping new blank-run/decorative comment-noise vs $base_ref"
if [ -n "$PRESHIP_PY" ]; then
    "$PRESHIP_PY" agents/scripts/core/comment_audit.py --fix "$base_ref" \
        || echo "pre-ship: WARN — comment-noise auto-strip errored; the gate below still enforces." >&2
else
    echo "pre-ship: WARN — no working python; comment-noise auto-strip skipped (the gate below still enforces)." >&2
fi

echo "pre-ship: running delta lint gate vs $base_ref"
if ! bash agents/scripts/project/test-lint-rules.sh --diff "$base_ref"; then
    echo "pre-ship: FAIL — fix the delta lint findings above before pushing." >&2
    exit 1
fi

# Markdown style lint (MD028 etc.) — docs are not covered by the C++ delta gate,
# so without this a markdown issue only surfaced as a post-push CodeRabbit finding.
echo "pre-ship: running markdown lint (md_lint.py --all)"
if [ -z "$PRESHIP_PY" ]; then
    echo "pre-ship: WARN — no working python; markdown lint skipped (CI still enforces it)." >&2
elif ! "$PRESHIP_PY" agents/scripts/core/md_lint.py --all; then
    echo "pre-ship: FAIL — fix the markdown findings above before pushing." >&2
    exit 1
fi

# Test-list consistency (build-quality-velocity-hardening #5): a new
# tests/Core/*.test.cpp not added to tests/CMakeLists.txt is silently uncompiled
# (false green). The configure-time assert in tests/CMakeLists.txt catches it in
# CI; run the same check here so it surfaces before push, not at configure time.
echo "pre-ship: running test-list consistency check"
if ! bash agents/scripts/core/check-test-list.sh --check; then
    echo "pre-ship: FAIL — add the unreferenced test(s) to tests/CMakeLists.txt before pushing." >&2
    exit 1
fi

# Orphan-bats (self-improvement tooling/P2): a new tests/bats/*.bats with no
# test-*.sh wrapper naming it never runs — test-all.sh discovers wrappers by the
# test-*.sh glob, never a bare .bats. The gate lives in test-all.sh / CI, but not
# this fast pre-push path, so a wrapper-less suite (mutation_smoke.bats, #1698)
# only reddened the Agentic self-tests lane a merge later on an unrelated PR.
# Near-instant (a glob + grep over wrappers, no build) — run it here so the gap
# is caught before push.
echo "pre-ship: running orphan-bats check (every tests/bats/*.bats needs a wrapper)"
if ! bash agents/scripts/core/test-orphan-bats.sh; then
    echo "pre-ship: FAIL — add a test-*.sh wrapper that runs the bats suite(s) above before pushing." >&2
    exit 1
fi

# Doc-validation mirror — the "Doc anchors + agent contract" CI check (plan-ref
# integrity, plan index, doc anchors, markdown links, …). Cheap (~10 s) and the
# only local stage that catches a docs/plans git-mv leaving stale refs in source
# comments (PR #953). Runs the same suite CI runs.
echo "pre-ship: running doc-validation suite (test-docs.sh)"
if ! bash scripts/dev/test-docs.sh; then
    echo "pre-ship: FAIL — fix the doc-validation findings above before pushing." >&2
    exit 1
fi

fi # end of the lint stages skipped under SMATCHET_PRESHIP_GATE_ONLY=1

# --- PR-burst advisory ----------------------------------------------------------------
# Non-blocking nudge: if the author already has >= threshold open PRs, opening
# another risks exhausting CodeRabbit's hourly review quota (the whole burst then
# merges with cr-out-of-band — lost review coverage). Advisory only; never fails
# pre-ship. Skipped under the gate-only selftest.
if [ "$gate_only" != "1" ] && [ -f "$repo_root/scripts/dev/pr-burst-guard.sh" ]; then
    bash "$repo_root/scripts/dev/pr-burst-guard.sh" || true
fi

# --- Code-review gate -----------------------------------------------------------------
# Closes the "pushed a substantive diff without a code review" oversight (build+lint green
# != reviewed; a correctness review catches what no static gate can — dead interface
# surface, cross-PR build hazards, behaviour drift). A diff is SUBSTANTIVE when it touches a
# strict zone (project.config.json `lint.zones.strict`) or changes >= REVIEW_LINE_THRESHOLD
# first-party C++ lines. Substantive ⇒ a fingerprint-pinned review ack is required.
review_threshold="${REVIEW_LINE_THRESHOLD:-60}"
review_changed_cpp=()
mapfile -t review_changed_cpp < <(
    git diff --name-only --diff-filter=d "$base_ref"...HEAD -- "${review_cpp_globs[@]}" 2>/dev/null
    git diff --name-only --diff-filter=d -- "${review_cpp_globs[@]}" 2>/dev/null
)
review_strict_hit=""
if [ "${#review_changed_cpp[@]}" -gt 0 ]; then
    # tr -d '\r': native Windows python3 prints CRLF; an un-stripped \r in the zone string
    # silently defeats the `case "$f" in "$z"*` prefix match (caught by --selftest).
    review_strict_zones=()
    if [ -n "$PRESHIP_PY" ]; then
        mapfile -t review_strict_zones < <(
            "$PRESHIP_PY" -c "import json;print('\n'.join(json.load(open('project.config.json'))['lint']['zones']['strict']))" \
                2>/dev/null | tr -d '\r' || true
        )
        for f in "${review_changed_cpp[@]}"; do
            [ -n "$f" ] || continue
            for z in "${review_strict_zones[@]:-}"; do
                [ -n "$z" ] || continue
                case "$f" in "$z"*) review_strict_hit="$f" ;; esac
            done
        done
    else
        # #1116 fail-CLOSED: no WORKING python (the Windows py-stub passes `command -v` but
        # exits 49) means the strict-zone list is unreadable. The old code just skipped
        # detection here, so a sub-threshold strict-zone diff N/A-passed the review gate
        # (fail-OPEN). Instead, treat the changed C++ as strict-requiring so the gate engages.
        echo "pre-ship: WARN — no working python; strict-zone list unreadable → requiring review conservatively (fail-closed, #1116)." >&2
        review_strict_hit="(strict-zone detection unavailable — no working python)"
    fi
fi
review_lines=$(
    {
        git diff --numstat "$base_ref"...HEAD -- "${review_cpp_globs[@]}" 2>/dev/null || true
        git diff --numstat -- "${review_cpp_globs[@]}" 2>/dev/null || true
    } | awk '{a+=($1=="-"?0:$1); d+=($2=="-"?0:$2)} END {print a+d+0}'
)
review_substantive=0
if [ -n "$review_strict_hit" ] || [ "${review_lines:-0}" -ge "$review_threshold" ]; then
    review_substantive=1
fi

if [ "$ack_review" -eq 1 ]; then
    review_fingerprint > "$review_marker"
    echo "pre-ship: review ACK recorded for the current diff vs $base_ref (.review-ack)."
    echo "pre-ship: PASS (ack) — gates clean + review acknowledged. Safe to push."
    exit 0
fi

if [ "${SMATCHET_SKIP_REVIEW_GATE:-0}" = "1" ]; then
    echo "pre-ship: WARN — code-review gate bypassed (SMATCHET_SKIP_REVIEW_GATE=1)."
elif [ "$review_substantive" -eq 1 ]; then
    have_fp=""
    [ -f "$review_marker" ] && have_fp="$(cat "$review_marker" 2>/dev/null || true)"
    want_fp="$(review_fingerprint)"
    if [ "$have_fp" != "$want_fp" ]; then
        reason="${review_strict_hit:+strict-zone touch ($review_strict_hit)}"
        reason="${reason:-$review_lines changed C++ lines >= $review_threshold}"
        cat >&2 <<EOF
pre-ship: FAIL — substantive C++ diff ($reason) requires a code review before push.
  No current review ack found (.review-ack ${have_fp:+is stale}${have_fp:-missing}).
  1) ensure the change is committed / final,
  2) run the code-review skill or agent on the diff vs $base_ref,
  3) record it:  bash scripts/dev/pre-ship.sh --ack-review${base_ref:+ $base_ref}
  (Trivial/emergency bypass: SMATCHET_SKIP_REVIEW_GATE=1 — logged, discouraged.)
EOF
        exit 1
    fi
    echo "pre-ship: code-review ack current for this diff (.review-ack matches)."
else
    echo "pre-ship: code-review gate N/A — diff is not substantive ($review_lines C++ lines, no strict-zone touch)."
fi

echo "pre-ship: PASS — formatted + delta lint gate + markdown lint + test-list + doc suite + review gate clean. Safe to push."
exit 0
