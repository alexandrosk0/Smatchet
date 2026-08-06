#!/usr/bin/env bash
# review-ack.sh — record / check the code-review acknowledgement for a diff.
# ----------------------------------------------------------------------------
# The commit-time half of docs/agent-rules/process-rules.md § Code-review before
# every commit. `scripts/git-hooks/pre-commit` refuses a SUBSTANTIVE staged C++
# diff whose fingerprint is not acknowledged here; the reviewer records the ack
# after running the `code-review` agent / adversarial-code-review skill:
#
#     bash agents/scripts/core/review-ack.sh --record --staged
#
# This is deliberately separate from `scripts/dev/pre-ship.sh --ack-review`,
# which records the same kind of ack for the PUSH gate but first runs the whole
# lint suite (clang-format, delta lint, markdown, doc-validation) — far too slow
# to sit in front of every commit. Both share agents/scripts/core/lib/review-ack.sh,
# so there is exactly one fingerprint implementation.
#
# Recording an ack you did not earn defeats the gate: the fingerprint proves the
# diff has not changed since SOMETHING was acknowledged, never that a review ran.
#
# Usage:
#   bash agents/scripts/core/review-ack.sh --record [--staged|--branch] [--verdict <f>] [<base-ref>]
#   bash agents/scripts/core/review-ack.sh --check  [--staged|--branch] [<base-ref>]
#   bash agents/scripts/core/review-ack.sh --selftest
#   bash agents/scripts/core/review-ack.sh --help
#
# Modes: --staged (the index — the commit gate, default) · --branch (<base>...HEAD
# plus the working tree — the push gate). <base-ref> defaults to origin/develop
# and is ignored by --staged.
#
# --verdict <file> attaches a verifier verdict (a `verifier-sidecar.py aggregate`
# result) to the ack — see docs/agent-rules/verifier-sidecar.md and
# docs/plans/verifier-scored-code-review-gate.md. ONLY `hard_veto` blocks; the
# continuous score is advisory until verifier-calibrate.py justifies promoting it
# (verifier-sidecar.md § Operating rules, rule 5).
#
# Exit codes:
#   0 — ack recorded, or (--check) the diff is acked / not substantive.
#   1 — (--check) a substantive diff has no current ack.
#   2 — usage error, not inside a git work tree, or an unreadable --verdict file.
#   3 — (--check) the ack is current but carries a HARD VETO.
#
# selftest: asserts-failure
set -euo pipefail

usage() {
    cat <<'USAGE'
review-ack.sh — record / check the code-review ack that gates a commit.

  bash agents/scripts/core/review-ack.sh --record [--staged|--branch] [<base-ref>]
  bash agents/scripts/core/review-ack.sh --check  [--staged|--branch] [<base-ref>]
  bash agents/scripts/core/review-ack.sh --selftest
  bash agents/scripts/core/review-ack.sh --help

Record the ack ONLY after actually running the code-review pass on that diff
(docs/agent-rules/process-rules.md § Code-review before every commit). Any later
edit changes the fingerprint and re-arms the gate.

Modes:  --staged  the index — what `git commit` will land (default)
        --branch  <base-ref>...HEAD plus the working tree — the push gate

Bypass for the commit gate (logged, discouraged): SMATCHET_SKIP_REVIEW_GATE=1
USAGE
}

# --selftest: prove --check BLOCKS an unacked substantive staged diff (the asserted
# failure case), that --record clears it, that a post-ack edit re-arms it, and that a
# trivial diff N/A-passes. Auto-enrolled by agents/scripts/core/test-gate-selftests.sh.
run_selftest() {
    local script_path lib_path tmp
    script_path="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    lib_path="$(cd "$(dirname "$0")" && pwd)/lib/review-ack.sh"
    if [ ! -f "$lib_path" ]; then
        echo "review-ack --selftest: FAIL — lib/review-ack.sh missing next to the script" >&2
        return 1
    fi
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064  # expand $tmp now, not at trap time
    trap "rm -rf '$tmp'" RETURN
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
        # Stage a strict-zone edit — substantive regardless of line count.
        printf '// edit\nint self_test_fn() { return 1; }\n' >> Source/Core/src/Sync/SelfTest.cpp
        git add Source/Core/src/Sync/SelfTest.cpp
    )
    # 1. Substantive staged diff, no ack -> MUST FAIL (the asserted failure case).
    if (cd "$tmp" && bash "$script_path" --check --staged >/dev/null 2>&1); then
        echo "review-ack --selftest: FAIL — --check passed a strict-zone staged diff with NO ack" >&2
        return 1
    fi
    # 2. Record -> MUST PASS.
    if ! (cd "$tmp" && bash "$script_path" --record --staged >/dev/null 2>&1 &&
        bash "$script_path" --check --staged >/dev/null 2>&1); then
        echo "review-ack --selftest: FAIL — --check still blocks after --record" >&2
        return 1
    fi
    # 3. Stage a further edit -> fingerprint stale -> MUST FAIL again (re-arm).
    (
        cd "$tmp"
        echo "// post-ack edit" >> Source/Core/src/Sync/SelfTest.cpp
        git add Source/Core/src/Sync/SelfTest.cpp
    )
    if (cd "$tmp" && bash "$script_path" --check --staged >/dev/null 2>&1); then
        echo "review-ack --selftest: FAIL — gate did not re-arm after a post-ack staged edit" >&2
        return 1
    fi
    # 4. A non-C++ staged diff is never substantive -> MUST PASS with no ack.
    (
        set -e
        cd "$tmp"
        git checkout -q -- . 2>/dev/null || true
        git reset -q
        echo "notes" > docs-note.md
        git add docs-note.md
    )
    if ! (cd "$tmp" && bash "$script_path" --check --staged >/dev/null 2>&1); then
        echo "review-ack --selftest: FAIL — a docs-only staged diff should N/A-pass the gate" >&2
        return 1
    fi
    echo "review-ack --selftest: PASS — blocks unacked substantive staged diffs, records, re-arms on edit, N/A on docs-only."
    return 0
}

mode="staged"
base_ref="origin/develop"
action=""

case "${1:-}" in
    --help | -h)
        usage
        exit 0
        ;;
    --selftest)
        run_selftest
        exit $?
        ;;
    --record) action="record" ;;
    --check) action="check" ;;
    *)
        usage >&2
        exit 2
        ;;
esac
shift

verdict_file=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --staged) mode="staged" ;;
        --branch) mode="branch" ;;
        --verdict)
            verdict_file="${2:-}"
            if [ -z "$verdict_file" ]; then
                echo "review-ack: --verdict requires a file (verifier-sidecar.py aggregate output)" >&2
                exit 2
            fi
            shift
            ;;
        --verdict=*) verdict_file="${1#--verdict=}" ;;
        --*)
            echo "review-ack: unknown flag '$1'" >&2
            exit 2
            ;;
        *) base_ref="$1" ;;
    esac
    shift
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "review-ack: not inside a git work tree" >&2
    exit 2
}

# Source the library from THIS script's directory, never from the target repo's
# root: --selftest (and any cross-repo use) runs the script from inside a
# throwaway work tree that has no agents/ tree of its own.
script_dir="$(cd "$(dirname "$0")" && pwd)"
# A missing/unreadable library is INFRA (rc 2 — the caller warns and allows), never
# rc 1 (which pre-commit renders as "you skipped the review" and blocks). Without
# this guard `set -e` turns a failed source into a bare rc 1, so a half-installed
# checkout would refuse even a docs-only commit with a message blaming the author.
if [ ! -r "$script_dir/lib/review-ack.sh" ]; then
    echo "review-ack: cannot read $script_dir/lib/review-ack.sh (incomplete checkout?)" >&2
    exit 2
fi
# shellcheck source=agents/scripts/core/lib/review-ack.sh
if ! . "$script_dir/lib/review-ack.sh"; then
    echo "review-ack: failed to source $script_dir/lib/review-ack.sh" >&2
    exit 2
fi

cd "$repo_root"

if [ "$action" = "record" ]; then
    fp="$(ra_fingerprint "$mode" "$base_ref")"
    if [ -n "$verdict_file" ]; then
        # A malformed / unreadable verdict must NOT silently degrade to a
        # presence-only ack: the caller asked for a scored ack and would
        # otherwise believe it got one.
        if ! verdict="$(ra_verdict_from_json "$verdict_file")"; then
            echo "review-ack: cannot read a verdict from '$verdict_file' (needs jq + a verifier-sidecar aggregate result)." >&2
            exit 2
        fi
        IFS=$'\t' read -r v_score v_rec v_veto v_reason <<< "$verdict"
        ra_write_marker "$mode" "$fp" "$v_score" "$v_rec" "$v_veto" "$v_reason"
        echo "review-ack: recorded a $mode review ack + verdict (score=$v_score recommendation=$v_rec hard_veto=$v_veto)."
        exit 0
    fi
    ra_write_marker "$mode" "$fp"
    echo "review-ack: recorded a $mode review ack for the current diff (.review-ack)."
    exit 0
fi

if ra_is_substantive "$mode" "$base_ref"; then
    have="$(ra_read_marker "$mode")"
    want="$(ra_fingerprint "$mode" "$base_ref")"
    if [ "$have" != "$want" ]; then
        echo "review-ack: NO current $mode ack — $RA_SUBSTANTIVE_REASON (.review-ack ${have:+is stale}${have:-missing})." >&2
        exit 1
    fi

    # The ack is current. Now consider the verdict, if one was recorded.
    #
    # ONLY `hard_veto` blocks. `recommendation` is NOT a blocking signal: the
    # sidecar returns "block" for a low SCORE as well as for a veto, and gating
    # on a score before verifier-calibrate.py clears its Brier/ECE/AUC
    # thresholds is exactly what verifier-sidecar.md § Operating rules rule 5
    # forbids. `escalate` is the ordinary single-sample outcome (a 0.82/0.7
    # sample already returns it), so treating it as blocking would wedge nearly
    # every commit.
    #
    # A veto is trusted even though it may be self-reported by the reviewing
    # agent, because a veto is a CONFESSION, not a claim of quality: an agent
    # reporting "I found a security hole in my own change" argues against its
    # own interest in a way a high score never does. Trust self-reported bad
    # news; distrust self-reported good news. That asymmetry is why the score
    # stays advisory while the veto blocks.
    verdict="$(ra_read_verdict "$mode")"
    if [ -n "$verdict" ]; then
        IFS=$'\t' read -r v_score v_rec v_veto v_reason <<< "$verdict"
        if [ "$v_veto" = "true" ]; then
            echo "review-ack: HARD VETO on this $mode diff — ${v_reason:-no reason recorded} (score=$v_score recommendation=$v_rec)." >&2
            exit 3
        fi
        echo "review-ack: $mode ack current for this diff (.review-ack matches)."
        echo "review-ack: verifier score=$v_score recommendation=$v_rec (ADVISORY — not a gate until calibrated)."
        exit 0
    fi

    echo "review-ack: $mode ack current for this diff (.review-ack matches)."
    exit 0
fi

echo "review-ack: N/A — $mode diff is not substantive ($RA_SUBSTANTIVE_REASON)."
exit 0
