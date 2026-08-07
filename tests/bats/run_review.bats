#!/usr/bin/env bats
# run_review.bats — regression suite for the review-panel launcher:
# agents/scripts/core/run-review.sh (port of Whip-Process Tools/run-review.ps1;
# test semantics ported from Tools/test-run-review.ps1).
#
# Strategy: a throwaway git repo carries a minimal roster + one work item; the
# harness CLIs are STUBS in a PATH-prepended fake bin dir that parse the
# launcher's real prompt for `out=<file>` and write it — so template shape,
# leg fan-out, watcher landing, and the write guard are all exercised without
# any real harness installed. PATH is RESTRICTED to the fake bin + the dirs of
# the tools the launcher itself needs (git/bash/python/coreutils), so a real
# `claude` on the dev box can never leak into a missing-CLI case.

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    FIX="$(mktemp -d)"
    export FIX

    mkdir -p "$FIX/agents/scripts/core/lib" "$FIX/docs/work/items/07-test-item"
    cp "$ROOT/agents/scripts/core/run-review.sh" "$FIX/agents/scripts/core/"
    cp "$ROOT/agents/scripts/core/lib/review-guard.sh" "$FIX/agents/scripts/core/lib/"
    echo "spec" > "$FIX/docs/work/items/07-test-item/1-specification.md"

    # Minimal roster: one model per harness keeps the assertions compact.
    cat > "$FIX/project.config.json" <<'EOF'
{
  "review-panel": {
    "harnesses": {
      "claude":   { "flag": "--model", "models": [{ "tag": "opus", "id": "opus" }] },
      "codex":    { "flag": "-m", "models": [{ "tag": "sol", "id": "gpt-5.6-sol" }] },
      "cursor":   { "flag": "--model", "start_dwell_secs": 25,
                    "models": [{ "tag": "composer", "id": "composer-2.5" }] },
      "opencode": { "flag": "-m", "models": [{ "tag": "deepseek-pro", "id": "opencode-go/deepseek-v4-pro" }] }
    }
  }
}
EOF

    git init --quiet -b develop "$FIX"
    git -C "$FIX" config user.email t@t
    git -C "$FIX" config user.name t
    git -C "$FIX" add -A
    git -C "$FIX" commit --quiet -m base

    FAKE_BIN="$(mktemp -d)"
    export FAKE_BIN

    # Restricted PATH: fake bin first, then only the dirs hosting the tools
    # run-review.sh itself needs. Anything else (a real harness CLI on the dev
    # box) is invisible, which makes the missing-CLI cases deterministic.
    RESTRICTED_PATH="$FAKE_BIN"
    local t d
    for t in git bash python python3 sort; do
        d="$(command -v "$t" 2>/dev/null)" || continue
        RESTRICTED_PATH="$RESTRICTED_PATH:$(dirname "$d")"
    done
    RESTRICTED_PATH="$RESTRICTED_PATH:/usr/bin:/bin"
    export RESTRICTED_PATH

    # /usr/bin:/bin is load-bearing (cut/mktemp/date/stat/seq/touch), so the
    # PATH is not fully sealed. Fail loudly if a real harness lives there —
    # otherwise the missing-CLI cases fail with an unrelated-looking message.
    local real
    for real in claude codex opencode cursor-agent; do
        if PATH="/usr/bin:/bin" command -v "$real" >/dev/null 2>&1; then
            skip "a real '$real' is in /usr/bin or /bin; the missing-CLI cases cannot be isolated"
        fi
    done
}

teardown() {
    rm -rf "${FIX:-}" "${FAKE_BIN:-}"
}

# make_stub <name> [stray] — install a fake harness CLI that extracts
# `out=<path>` from the launcher's prompt and writes that file (plus an
# unexpected one, for the stray case). No-op body via `make_stub <name> noop`.
make_stub() {
    local name="$1" mode="${2:-write}"
    {
        echo '#!/usr/bin/env bash'
        echo 'out=""'
        echo 'for a in "$@"; do case "$a" in *out=*) out="${a##*out=}";; esac; done'
        case "$mode" in
            write)
                echo '[ -n "$out" ] || exit 1'
                echo 'printf "stub review\n" > "$out"' ;;
            stray)
                echo '[ -n "$out" ] || exit 1'
                echo 'printf "stub review\n" > "$out"'
                echo 'printf "scribble\n" > leg-scribble.txt' ;;
            noop)
                echo 'exit 0' ;;
        esac
    } > "$FAKE_BIN/$name"
    chmod +x "$FAKE_BIN/$name"
}

all_stubs() {
    make_stub claude
    make_stub codex
    make_stub opencode
    make_stub cursor-agent
}

rr() {
    ( cd "$FIX" && env PATH="$RESTRICTED_PATH" bash agents/scripts/core/run-review.sh "$@" )
}

@test "dry-run pre gate: every roster leg listed with the gate's 4- filename" {
    all_stubs
    run rr --gate pre --subject 07 --round 2 --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"docs/work/items/07-test-item/4-review-2-claude-opus.md"* ]]
    [[ "$output" == *"4-review-2-codex-sol.md"* ]]
    [[ "$output" == *"4-review-2-cursor-composer.md"* ]]
    [[ "$output" == *"4-review-2-opencode-deepseek-pro.md"* ]]
    [[ "$output" == *"/pre-implementation-review 07 out="* ]]
}

@test "dry-run post gate: 5- prefix and the adversarial-code-review skill" {
    all_stubs
    # Subject accepts the full folder name as well as the bare number.
    run rr --gate post --subject 07-test-item --round 1 --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"5-review-1-claude-opus.md"* ]]
    [[ "$output" == *"/adversarial-code-review 07 out="* ]]
}

@test "codex leg carries the unattended flags (-a never -s workspace-write)" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"codex exec -a never -s workspace-write '-m' 'gpt-5.6-sol'"* ]]
}

@test "cursor leg is a brandless prose ask citing the shared skill file" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"cursor-agent --force -p '--model' 'composer-2.5'"* ]]
    [[ "$output" == *"agents/_shared/skills/pre-implementation-review/SKILL.md"* ]]
    # No slash command in the cursor ask: cursor has no skill loader.
    cursor_line="$(grep 'cursor-agent --force' <<< "$output")"
    [[ "$cursor_line" != *"/pre-implementation-review 07"* ]]
}

@test "--brief is rejected (generic gate not ported)" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --brief --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"--brief applies only to the 'generic' gate"* ]]
}

@test "--legs subset launches only those harnesses" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --legs claude,codex --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"claude-opus"* ]]
    [[ "$output" == *"codex-sol"* ]]
    [[ "$output" != *"cursor-composer"* ]]
    [[ "$output" != *"opencode-deepseek-pro"* ]]
}

@test "--legs with an unknown harness errors, listing the roster" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --legs claude,nonesuch --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown harness 'nonesuch'"* ]]
    [[ "$output" == *"claude"* ]]
}

@test "missing CLI: that leg is skipped and reported, the rest still run" {
    make_stub claude
    make_stub opencode
    make_stub cursor-agent
    # no codex stub
    run rr --gate pre --subject 07 --round 1 --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"SKIPPED - codex-sol"* ]]
    [[ "$output" == *"claude-opus"* ]]
    [[ "$output" == *"cursor-composer"* ]]
}

@test "every CLI missing: No legs selected, exit 2" {
    # empty fake bin
    run rr --gate pre --subject 07 --round 1 --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"No legs selected."* ]]
}

@test "--round is required and validated" {
    all_stubs
    run rr --gate pre --subject 07 --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"--round is required"* ]]
    run rr --gate pre --subject 07 --round zero --dry-run
    [ "$status" -eq 2 ]
}

@test "subject that resolves to no item folder errors" {
    all_stubs
    run rr --gate pre --subject 99 --round 1 --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"did not resolve to exactly one"* ]]
}

@test "--smoke-test replaces leg commands with the echo shim" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --smoke-test --dry-run
    [ "$status" -eq 0 ]
    [[ "$output" == *"SMOKE-TEST [claude-opus]"* ]]
    [[ "$output" != *"--dangerously-skip-permissions"* ]]
}

@test "e2e: stub legs land their files, guard clean, exit 0" {
    all_stubs
    run rr --gate pre --subject 07 --round 1 --legs claude,codex
    [ "$status" -eq 0 ]
    [ -f "$FIX/docs/work/items/07-test-item/4-review-1-claude-opus.md" ]
    [ -f "$FIX/docs/work/items/07-test-item/4-review-1-codex-sol.md" ]
    [[ "$output" == *"2/2 legs wrote a file"* ]]
    [[ "$output" == *"Write guard: clean"* ]]
    [[ "$output" != *"clean SO FAR"* ]]
}

@test "e2e: a leg writing outside its review file is a stray, exit 1" {
    make_stub claude stray
    run rr --gate pre --subject 07 --round 1 --legs claude
    [ "$status" -eq 1 ]
    [[ "$output" == *"WRITE GUARD:"* ]]
    [[ "$output" == *"leg-scribble.txt"* ]]
    [[ "$output" == *"Review these before judging the round"* ]]
}

@test "e2e: leg that never writes its file fails the round (exit 1)" {
    make_stub claude noop
    # timeout 0: the deadline is already past, so the watcher exits at once —
    # a fast stand-in for the 45-minute wait with an unlanded leg.
    run rr --gate pre --subject 07 --round 1 --legs claude --timeout-minutes 0
    [ "$status" -eq 1 ]
    [[ "$output" == *"No file from: claude-opus"* ]]
}

@test "e2e refill: files from an earlier round tick do not count as landed" {
    all_stubs
    # Pre-existing review file from a previous run of the SAME round.
    echo "old" > "$FIX/docs/work/items/07-test-item/4-review-1-claude-opus.md"
    # Backdate it so an mtime-vs-launch check cannot mistake it for a landing.
    touch -d "2020-01-01 00:00:00" "$FIX/docs/work/items/07-test-item/4-review-1-claude-opus.md"
    run rr --gate pre --subject 07 --round 1 --legs claude,codex
    [ "$status" -eq 0 ]
    # The claude stub re-wrote its file fresh; both legs must be counted from
    # THIS launch, not seeded by the stale file.
    [[ "$output" == *"2/2 legs wrote a file"* ]]
    [[ "$output" != *"No file from:"* ]]
    grep -q "stub review" "$FIX/docs/work/items/07-test-item/4-review-1-claude-opus.md"
}
