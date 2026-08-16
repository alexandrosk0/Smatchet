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
#   1b. comment-noise auto-strip   (the second and LAST diff-mutating step)
#   2. test-lint-rules.sh --diff <base>   (comment-noise + function-size + strict-zone)
#   3. md_lint / test-list consistency / test-docs.sh (doc-validation CI mirror)
#
# Steps 0-1b MUTATE the diff; every stage after them is read-only w.r.t. the C++
# diff. That split is load-bearing, not cosmetic — it is what lets a code review
# run CONCURRENTLY with the read-only stages (`--format-only` first, then the gate
# and the reviewer dispatched together — ship-loops.md § pre-first-push gate). A
# reviewer dispatched alongside a mutating stage would review pre-format code and
# stamp a fingerprint that goes stale the moment clang-format lands.
#
# It deliberately does NOT build or run tests — that is the caller's job and is
# already covered once-per-slice. This is purely the lint half that CI gates on
# but local builds skip.
#
# Usage:
#   bash scripts/dev/pre-ship.sh [<base-ref>]   # default base: origin/develop
#   bash scripts/dev/pre-ship.sh --format-only [<base-ref>]
#   bash scripts/dev/pre-ship.sh --review-fingerprint [<base-ref>]
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
  bash scripts/dev/pre-ship.sh --format-only [<base-ref>]       run ONLY the diff-mutating steps
  bash scripts/dev/pre-ship.sh --review-fingerprint [<base-ref>] print the diff fingerprint, do nothing else
  bash scripts/dev/pre-ship.sh --help

Runs, over first-party C++ changed vs <base-ref>:
  1. clang-format -i + comment-noise auto-strip   (the diff-MUTATING steps; `--format-only`
     stops here so a code review can run CONCURRENTLY with steps 2-3 against a stable diff)
  2. agents/scripts/project/test-lint-rules.sh --diff <base-ref>
  3. markdown lint + test-list consistency + doc-validation suite
  4. code-review gate — a SUBSTANTIVE C++ diff (strict-zone touch or
     >= REVIEW_LINE_THRESHOLD changed lines, default 60) must have a current review
     ack AND a .review-findings.json artifact whose "fingerprint" matches the diff (so a
     bare --ack-review with no review behind it cannot pass). Run the code-review
     skill/agent — it writes the artifact — then `--ack-review` to record it; any later
     edit re-arms the gate. Bypass: SMATCHET_SKIP_REVIEW_GATE=1 (logged).
  5. verifier verdict (optional) — when VERIFIER_BASE_URL + VERIFIER_MODEL name an
     INDEPENDENT backend, `--ack-review` also scores the diff through
     verifier-produce.py | verifier-sidecar.py and attaches the verdict to the ack.
     A recorded hard_veto blocks every run; the score is advisory until calibrated
     (verifier-produce.py never emits hard_veto, so the produced verdict alone cannot
     veto — even a worst-case score does not block). Unset = no verdict
     (never a self-score). Backend down = WARN + presence-only ack, never a block.
     Tunables: VERIFIER_MODE (logprobs|scalar), VERIFIER_REPEATS, VERIFIER_DIFF_MAX_BYTES.
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
    # 2a. Ack with NO review artifact -> MUST FAIL. This is the whole point of the
    #     artifact: a bare --ack-review is one keystroke and proves nothing was reviewed.
    if (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" --ack-review base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — --ack-review recorded an ack with no .review-findings.json" >&2
        return 1
    fi
    # 2b. Ack with a MISMATCHED artifact -> MUST FAIL (a review of some other diff).
    (cd "$tmp" && printf '{"fingerprint":"%064d"}\n' 0 > .review-findings.json)
    if (cd "$tmp" && SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" --ack-review base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — --ack-review accepted a stale .review-findings.json" >&2
        return 1
    fi
    # 2c. Artifact stamped via --review-fingerprint, then ack -> MUST PASS (both the ack
    #     and a plain gate run, which re-checks the artifact independently).
    if ! (cd "$tmp" &&
        fp="$(SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" --review-fingerprint base)" &&
        printf '{"fingerprint":"%s","reviewer":"selftest"}\n' "$fp" > .review-findings.json &&
        SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" --ack-review base >/dev/null 2>&1 &&
        SMATCHET_PRESHIP_GATE_ONLY=1 bash "$script_path" base >/dev/null 2>&1); then
        echo "pre-ship --selftest: FAIL — gate still blocks after a stamped artifact + --ack-review" >&2
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
    echo "pre-ship --selftest: PASS — gate blocks unacked substantive diffs, refuses an ack with a missing/stale review artifact, acks with a stamped one, re-arms on edit, bypass works, #1116 fail-closed on no-python."
    return 0
}

base_ref="origin/develop"
ack_review=0
format_only=0
print_fingerprint=0
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
    --format-only)
        format_only=1
        [ -n "${2:-}" ] && base_ref="$2"
        ;;
    --format-only=*)
        format_only=1
        base_ref="${1#--format-only=}"
        if [ -z "$base_ref" ]; then
            echo "pre-ship: --format-only= requires a non-empty base ref (or use bare --format-only for origin/develop)" >&2
            exit 2
        fi
        ;;
    --review-fingerprint)
        print_fingerprint=1
        [ -n "${2:-}" ] && base_ref="$2"
        ;;
    --review-fingerprint=*)
        print_fingerprint=1
        base_ref="${1#--review-fingerprint=}"
        if [ -z "$base_ref" ]; then
            echo "pre-ship: --review-fingerprint= requires a non-empty base ref (or use bare --review-fingerprint for origin/develop)" >&2
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
#
# The fingerprint / substantive-diff / marker implementation moved to
# agents/scripts/core/lib/review-ack.sh so the COMMIT-side gate
# (scripts/git-hooks/pre-commit, via agents/scripts/core/review-ack.sh) shares exactly
# one implementation with this PUSH-side gate. This script keeps mode `branch`
# (<base-ref>...HEAD + working tree); the commit hook uses mode `staged`.
#
# Sourced from THIS script's directory, never "$repo_root": --selftest runs the script
# against a throwaway work tree that has no agents/ tree of its own.
preship_script_dir="$(cd "$(dirname "$0")" && pwd)"
preship_review_lib="$preship_script_dir/../../agents/scripts/core/lib/review-ack.sh"
if [ ! -r "$preship_review_lib" ]; then
    # rc 2 is this script's "required tool unavailable" code — not rc 1, which
    # means "a gate found a real violation".
    echo "pre-ship: cannot read $preship_review_lib (incomplete checkout?)" >&2
    exit 2
fi
# shellcheck source=agents/scripts/core/lib/review-ack.sh
. "$preship_review_lib"

# Self-staleness detector (gate-tooling-run-from-stale-session-branch, process P1).
# This gate runs from whatever checkout the author happens to be on, and
# `claude/<id>/*` session branches live for days with nothing pulling
# `agents/scripts/**` forward. A stale pre-ship is worse than a stale merge poller:
# the poller fails LOUD (a phantom BLOCK someone investigates), while this one fails
# SILENT — it prints "Safe to push" using lint rules `develop` has since tightened,
# and nobody re-examines a green. Advisory only; a freshness note must never be the
# reason a lint gate fails. Missing lib → skip (an incomplete checkout already trips
# the review-ack guard above; this must not become a second hard dependency).
preship_freshness_lib="$preship_script_dir/../../agents/scripts/core/lib/script-freshness.sh"
if [ -r "$preship_freshness_lib" ]; then
    # shellcheck source=agents/scripts/core/lib/script-freshness.sh
    . "$preship_freshness_lib"
fi

# SMATCHET_PRESHIP_FORCE_NO_PY is honoured inside ra_resolve_python (selftest hook that
# forces the #1116 fail-closed path in an environment that DOES have python).
PRESHIP_PY="$(ra_resolve_python || true)"

# preship_ita_untracked [quiet] — register untracked files with `git add --intent-to-add`.
#
# Untracked files are INVISIBLE to every git-diff- and git-grep-based gate below
# (`git diff <base>` skips them; `git grep` scans tracked only). Running pre-ship
# before the first `git add` therefore false-passed comment-noise and
# plan-ref-integrity on brand-new files (PR #953 — two CI-only failures). Intent-
# to-add registers them in the index (content stays unstaged) so the gates see
# exactly what CI will see — and so `ra_fingerprint` covers a brand-new .cpp
# instead of stamping a diff that silently omits it.
# `quiet` suppresses the stdout note so --review-fingerprint stays machine-readable.
preship_ita_untracked() {
    local quiet="${1:-}"
    local untracked=()
    mapfile -t untracked < <(git ls-files --others --exclude-standard)
    [ "${#untracked[@]}" -gt 0 ] || return 0
    [ "$quiet" = "quiet" ] ||
        echo "pre-ship: git add --intent-to-add ${#untracked[@]} untracked file(s) so gates can see them"
    git add --intent-to-add -- "${untracked[@]}"
    # Undo the ita registrations on EVERY exit (pass or fail) — leaving them
    # would make scratch files commit-eligible via a later `git commit -a` and
    # flip their `git status` bucket from untracked to modified (CR-964 review).
    # shellcheck disable=SC2064  # expand ${untracked[@]} NOW, not at trap time
    trap "git restore --staged -- $(printf '%q ' "${untracked[@]}") 2>/dev/null || true" EXIT
}

# --review-fingerprint: print the fingerprint of the current diff and stop. This is the
# reviewer's stamp — the code-review agent embeds it in .review-findings.json so the ack
# can prove the review read THIS diff. Read-only and lint-free on purpose: it is called
# from inside a review that is running concurrently with the gate, and a second mutating
# pass there would move the very diff it is fingerprinting.
if [ "$print_fingerprint" -eq 1 ]; then
    preship_ita_untracked quiet
    ra_fingerprint branch "$base_ref"
    exit 0
fi

# --- Verifier verdict production (docs/plans/verifier-scored-code-review-gate.md, slice 2) ---
# Drives the INDEPENDENT verifier over the branch diff and attaches the verdict to the ack,
# so the gate can act on review quality and not merely on review presence.
#
# THE INDEPENDENCE RULE IS THE WHOLE POINT. A score is only meaningful if it comes from a
# model other than the one under review; a reviewing agent scoring its own work is
# self-certification with extra steps, and strictly worse than the honest binary it would
# replace. So this runs ONLY when an external OpenAI-compatible backend is configured, and
# is silently absent otherwise — never a self-scored fallback.
#
# FAIL-OPEN, LOUDLY. A backend that is down, slow, or misconfigured must never block a push:
# production failure degrades to a presence-only ack with a WARN. That is the opposite of
# review-ack.sh's `--verdict <file>` contract (rc 2, refuse) — deliberately, because there
# the CALLER named a verdict file and would otherwise believe it got a scored ack, whereas
# here the backend is best-effort infrastructure. The WARN is what keeps the degrade honest.
preship_verifier_ready() {
    [ -n "${VERIFIER_BASE_URL:-}" ] && [ -n "${VERIFIER_MODEL:-}" ]
}

# preship_produce_verdict <out.json> — write a verifier-sidecar aggregate result for the
# current branch diff. rc 0 = usable verdict at <out.json>; rc 1 = could not produce one.
preship_produce_verdict() {
    local out="$1" jobf tracef rc
    [ -n "$PRESHIP_PY" ] || return 1
    jobf="$(mktemp)" || return 1
    mkdir -p "$repo_root/.verifier-traces" 2>/dev/null || true
    # The trace is the calibration evidence: verifier-calibrate.py needs recorded runs
    # paired with outcomes before the score can ever graduate from advisory to blocking
    # (verifier-sidecar.md § Calibration). Producing verdicts without recording them would
    # leave the score permanently un-promotable.
    tracef="$repo_root/.verifier-traces/$(git rev-parse --abbrev-ref HEAD 2>/dev/null | tr '/' '-')-$(date -u +%Y%m%dT%H%M%SZ).json"

    # Cap the candidate text: a large branch diff would blow the context window and the
    # per-criterion cost. Truncation is disclosed IN the prompt so the model scores what it
    # actually saw rather than silently judging a fragment as if it were the whole change.
    "$PRESHIP_PY" - "$jobf" "$base_ref" "${VERIFIER_DIFF_MAX_BYTES:-60000}" <<'PY' || { rm -f "$jobf"; return 1; }
import json, subprocess, sys
job_path, base_ref, cap = sys.argv[1], sys.argv[2], int(sys.argv[3])
diff = subprocess.run(["git", "diff", f"{base_ref}...HEAD"], capture_output=True, text=True,
                      errors="replace").stdout
note = ""
if len(diff) > cap:
    diff, note = diff[:cap], f"\n\n[TRUNCATED at {cap} bytes — this is a PREFIX of the diff, not the whole change.]"
if not diff.strip():
    sys.exit(1)
json.dump({
    "problem": ("Review this branch diff for a C++14 desktop app. Judge the CHANGE itself: "
                "correctness, regression risk, security, project invariants, scope discipline, "
                "and whether its verification is proportional to its risk." + note),
    "candidates": [{"id": "branch-diff", "text": diff + note}],
    "repeats": int(__import__("os").environ.get("VERIFIER_REPEATS", "1")),
}, open(job_path, "w"))
PY

    set +e
    "$PRESHIP_PY" "$repo_root/scripts/dev/verifier-produce.py" "$jobf" \
        --mode "${VERIFIER_MODE:-logprobs}" --record "$tracef" 2>/dev/null \
        | "$PRESHIP_PY" "$repo_root/scripts/dev/verifier-sidecar.py" aggregate - > "$out" 2>/dev/null
    rc=$?
    set -e
    rm -f "$jobf"
    if [ "$rc" -ne 0 ] || [ ! -s "$out" ]; then
        return 1
    fi
    echo "pre-ship: verifier trace recorded at ${tracef#"$repo_root/"} (calibration evidence)."
    return 0
}

# SMATCHET_PRESHIP_GATE_ONLY=1 (selftest hook): skip the lint stages and run ONLY the
# code-review gate. Never set this in normal use — the lint stages are the point.
gate_only="${SMATCHET_PRESHIP_GATE_ONLY:-0}"

# --- Group A: the diff-MUTATING stages ------------------------------------------------
# Everything below CHANGES the C++ diff. It must finish BEFORE a code review starts, or
# the reviewer reads pre-format code and stamps a fingerprint that is stale on arrival.
# `--format-only` runs exactly this block and stops, which is what lets the review run
# concurrently with Group B (ship-loops.md § pre-first-push gate).
if [ "$gate_only" != "1" ]; then

    if ! command -v clang-format >/dev/null 2>&1; then
        echo "pre-ship: clang-format not found on PATH" >&2
        exit 2
    fi

preship_ita_untracked

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

fi # end of Group A (the diff-mutating stages)

# --- t=0 code-review notice -----------------------------------------------------------
# The diff is FINAL here: both mutating stages are behind us and nothing after this point
# rewrites C++. So this is the EARLIEST moment the fingerprint a reviewer must stamp is
# knowable — and printing the "review required" verdict here, above the multi-minute
# read-only stages instead of after them, is what turns the review from serial into
# concurrent. The gate at the bottom still recomputes everything: formatting can shift
# line counts across REVIEW_LINE_THRESHOLD, so this notice is advisory, never the verdict.
if ra_is_substantive branch "$base_ref"; then
    cat <<EOF
pre-ship: CODE REVIEW REQUIRED — substantive diff ($RA_SUBSTANTIVE_REASON).
  fingerprint: $(ra_fingerprint branch "$base_ref")
  Dispatch the code-review agent NOW, concurrently with the stages below. It writes
  .review-findings.json stamped with that fingerprint; the ack then refuses without it.
  Record afterwards:  bash scripts/dev/pre-ship.sh --ack-review $base_ref
EOF
fi

if [ "$format_only" -eq 1 ]; then
    echo "pre-ship: PASS (--format-only) — diff-mutating stages done, diff is stable."
    echo "pre-ship: now run the lint stages and the code review CONCURRENTLY:"
    echo "  bash scripts/dev/pre-ship.sh $base_ref   ∥   code-review agent"
    exit 0
fi

# --- Group B: the READ-ONLY stages ----------------------------------------------------
# Nothing below mutates the C++ diff, so a code review may run alongside all of it.
if [ "$gate_only" != "1" ]; then

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

fi # end of Group B (the lint stages skipped under SMATCHET_PRESHIP_GATE_ONLY=1)

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
review_substantive=0
if ra_is_substantive branch "$base_ref"; then
    review_substantive=1
fi
# The #1116 fail-closed branch lives in ra_strict_hit; surface it here as before.
case "$RA_SUBSTANTIVE_REASON" in
    *"no working python"*)
        echo "pre-ship: WARN — no working python; strict-zone list unreadable → requiring review conservatively (fail-closed, #1116)." >&2
        ;;
esac

# --- Review ARTIFACT requirement ------------------------------------------------------
# A fingerprint-pinned ack proves the diff did not move after someone typed --ack-review.
# It does NOT prove a review happened — an empty ack is one keystroke. So the reviewer
# must also leave an artifact: .review-findings.json, carrying the fingerprint of the diff
# it actually read. Ack and gate both refuse without a matching one.
#
# Honest about the limit: this closes the "forgot / rubber-stamped" hole, not the
# adversarial one. A hand-written JSON still passes, as does the logged bypass. The point
# is that skipping the review now takes a deliberate forgery rather than an omission.
review_findings_path="$repo_root/.review-findings.json"

# preship_findings_fingerprint — echo the "fingerprint" field of the artifact; rc 1 if
# absent/unparseable. Parsed with grep, NOT python or jq, deliberately: this gate already
# has a documented fail-closed path for a missing python (#1116), and a second interpreter
# dependency would either widen that surface or invite a WARN-degrade that re-opens the
# very hole the artifact closes. A 64-hex field needs no JSON parser.
preship_findings_fingerprint() {
    [ -r "$review_findings_path" ] || return 1
    local fp
    fp="$(grep -oE '"fingerprint"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' "$review_findings_path" 2>/dev/null |
        head -n 1 | grep -oE '[0-9a-f]{64}' || true)"
    [ -n "$fp" ] || return 1
    printf '%s\n' "$fp"
}

# preship_require_findings <want-fingerprint> — rc 0 when a matching artifact exists,
# rc 1 (with the operator instructions) when it is missing or stale.
preship_require_findings() {
    local want="$1" have
    have="$(preship_findings_fingerprint || true)"
    if [ -z "$have" ]; then
        cat >&2 <<EOF
pre-ship: FAIL — no code-review artifact found (.review-findings.json).
  An ack with no review behind it is a keystroke, not a review.
  1) run the code-review agent on the diff vs $base_ref,
  2) it writes .review-findings.json stamped with the fingerprint from
     bash scripts/dev/pre-ship.sh --review-fingerprint $base_ref
  (Emergency bypass: SMATCHET_SKIP_REVIEW_GATE=1 — logged, discouraged.)
EOF
        return 1
    fi
    if [ "$have" != "$want" ]; then
        cat >&2 <<EOF
pre-ship: FAIL — the code-review artifact is stale (.review-findings.json).
  artifact fingerprint: $have
  current diff:         $want
  The diff moved after the review. Re-review the current diff and re-write the artifact.
  (Emergency bypass: SMATCHET_SKIP_REVIEW_GATE=1 — logged, discouraged.)
EOF
        return 1
    fi
    return 0
}

if [ "$ack_review" -eq 1 ]; then
    preship_fp="$(ra_fingerprint branch "$base_ref")"
    # Refuse to record an ack for a substantive diff no review artifact covers. Checked
    # HERE rather than only at gate time so the failure lands on the person acking, while
    # they still have the review in front of them.
    if [ "$review_substantive" -eq 1 ]; then
        if [ "${SMATCHET_SKIP_REVIEW_GATE:-0}" = "1" ]; then
            echo "pre-ship: WARN — review-artifact requirement bypassed (SMATCHET_SKIP_REVIEW_GATE=1)." >&2
        elif ! preship_require_findings "$preship_fp"; then
            exit 1
        fi
    fi
    preship_verdict=""
    if preship_verifier_ready; then
        preship_vfile="$(mktemp)"
        if preship_produce_verdict "$preship_vfile"; then
            preship_verdict="$(ra_verdict_from_json "$preship_vfile" || true)"
        fi
        rm -f "$preship_vfile"
        if [ -z "$preship_verdict" ]; then
            echo "pre-ship: WARN — verifier backend configured but produced no usable verdict; recording a presence-only ack." >&2
        fi
    fi
    if [ -n "$preship_verdict" ]; then
        IFS=$'\t' read -r pv_score pv_rec pv_veto pv_reason <<< "$preship_verdict"
        ra_write_marker branch "$preship_fp" "$pv_score" "$pv_rec" "$pv_veto" "$pv_reason"
        echo "pre-ship: review ACK + verifier verdict recorded (score=$pv_score recommendation=$pv_rec hard_veto=$pv_veto)."
        if [ "$pv_veto" = "true" ]; then
            # Rule 2: veto beats average. Unlike the score, this is categorical and blocks
            # without waiting on calibration.
            echo "pre-ship: FAIL — HARD VETO: ${pv_reason:-no reason recorded}. Fix it and re-run the review." >&2
            exit 1
        fi
        echo "pre-ship: the score is ADVISORY — it does not gate until verifier-calibrate.py justifies promoting it."
    else
        ra_write_marker branch "$preship_fp"
        echo "pre-ship: review ACK recorded for the current diff vs $base_ref (.review-ack)."
    fi
    echo "pre-ship: PASS (ack) — gates clean + review acknowledged. Safe to push."
    exit 0
fi

if [ "${SMATCHET_SKIP_REVIEW_GATE:-0}" = "1" ]; then
    echo "pre-ship: WARN — code-review gate bypassed (SMATCHET_SKIP_REVIEW_GATE=1)."
elif [ "$review_substantive" -eq 1 ]; then
    have_fp="$(ra_read_marker branch)"
    want_fp="$(ra_fingerprint branch "$base_ref")"
    if [ "$have_fp" != "$want_fp" ]; then
        reason="$RA_SUBSTANTIVE_REASON"
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
    # Re-checked at gate time, not just at ack time: the artifact is an ordinary
    # (gitignored) file, and an ack whose evidence has since been deleted is back to being
    # a bare keystroke.
    if ! preship_require_findings "$want_fp"; then
        exit 1
    fi
    echo "pre-ship: code-review artifact present and matching (.review-findings.json)."
    # A recorded veto must block EVERY run, not just the --ack-review invocation that
    # produced it. Without this the veto is trivially bypassed: `--ack-review` writes the
    # ack and THEN exits 1 on the veto, so a plain re-run finds a matching fingerprint and
    # reports "ack current → safe to push". The commit gate already honours the veto
    # (review-ack.sh --check rc 3); the push gate must not be the weaker of the two.
    preship_gate_verdict="$(ra_read_verdict branch)"
    if [ -n "$preship_gate_verdict" ]; then
        IFS=$'\t' read -r gv_score gv_rec gv_veto gv_reason <<< "$preship_gate_verdict"
        if [ "$gv_veto" = "true" ]; then
            echo "pre-ship: FAIL — the recorded review verdict carries a HARD VETO: ${gv_reason:-no reason recorded}." >&2
            echo "  Fix the vetoed issue, re-review, and re-record the ack. (Emergency bypass: SMATCHET_SKIP_REVIEW_GATE=1.)" >&2
            exit 1
        fi
        echo "pre-ship: recorded verifier score=$gv_score recommendation=$gv_rec (ADVISORY — not a gate until calibrated)."
    fi
else
    echo "pre-ship: code-review gate N/A — diff is not substantive ($RA_SUBSTANTIVE_REASON)."
fi

# Staleness caveat is emitted HERE, immediately before the PASS line, not at
# startup: a caveat printed ahead of several minutes of gate output has scrolled
# away by the time the verdict lands, and the verdict is the thing it qualifies.
# The declared set is every file whose content can change this script's verdict —
# the entry point, the delta-lint gate it shells out to plus that gate's rule
# modules, the shared review-ack logic, and the detector itself (a stale detector
# is the one blind spot that would hide all the others).
# The rules glob stays QUOTED on purpose: the detector expands it against the
# repo root. Unquoted, the shell expands it against the invoking CWD, and from
# any subdirectory it matches nothing, passes the literal pattern through, and
# silently degrades the whole check to `unverifiable` — which prints nothing.
if command -v warn_if_script_stale >/dev/null 2>&1; then
    warn_if_script_stale "pre-ship gate logic" \
        "scripts/dev/pre-ship.sh" \
        "agents/scripts/project/test-lint-rules.sh" \
        "agents/scripts/core/lib/review-ack.sh" \
        "agents/scripts/core/lib/script-freshness.sh" \
        "agents/scripts/project/lint-rules.d/*.sh"
fi

echo "pre-ship: PASS — formatted + delta lint gate + markdown lint + test-list + doc suite + review gate clean. Safe to push."
exit 0
