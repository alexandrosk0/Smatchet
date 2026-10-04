#!/usr/bin/env bash
# agent-layer-sim.sh — run the agent layer's own CI lanes against a standalone
# image of the layer, before the layer repo exists (or on the rewritten seed
# clone, before it is pushed).
#
# Plan: docs/plans/agent-surface-extraction-repo.md — Phase B rows 8 and 9.
#
# WHY THIS EXISTS
#   The seed manifest (seed-agent-layer-repo.d/docs/seed-paths.txt) is an
#   allowlist, and every suite, fixture and script the layer's CI touches has to be
#   on it. Phase 2 of seed-agent-layer-repo.sh checks the manifest's SHAPE; it
#   cannot tell whether the seeded tree actually passes its own gates. The first
#   simulation (2026-10-03) found 45 bats suites red and 3 vacuously green in a
#   tree phase 2 had reviewed clean — orphan wrappers, unseeded fixtures, host
#   subjects-under-test. Only running the lanes finds that class, so this runs them.
#
# NOT named test-*.sh ON PURPOSE: test-all.sh auto-enrols that glob, and this
# script runs the whole of test-all.sh inside the image (~25 min) — enrolling it
# would recurse and double every agentic-selftests run.
#
# USAGE
#   agent-layer-sim.sh [--rev REV] [--dir DIR] [--keep] [--lane NAME]... [--help]
#   agent-layer-sim.sh --run-only DIR [--lane NAME]...
#   agent-layer-sim.sh --build-only [--rev REV] [--dir DIR]
#
#   --rev REV       Commit to image (default HEAD). The image is built from git
#                   objects, so uncommitted edits are NOT simulated.
#   --dir DIR       Where to build the image. Must not exist. Default: a mktemp dir.
#   --keep          Keep the image after a green run (a red run always keeps it).
#   --build-only    Build the image and stop: no lanes, the image is kept. The
#                   flip probe (agent-layer-flip-probe.sh) mounts it as a submodule.
#   --run-only DIR  Skip the build; run the lanes in an existing layer tree. Used
#                   by seed phase 4c on the rewritten clone. The tree must start
#                   clean, and anything the lanes leave behind — a modified tracked
#                   file, or a new file .gitignore does not cover — fails the run,
#                   because that tree is about to be published.
#   --lane NAME     Run only this lane (repeatable): bats | shell | docs.
#                   Default: all three, in that order.
#
# LANES — each mirrors one row-9 workflow job in the scaffold, and the run
# asserts the command text still appears in that workflow so the two cannot drift:
#   bats   .github/workflows/agentic-selftests.yml  "Agentic self-tests (bats)"
#   shell  .github/workflows/shell-lint.yml         "Shell lint (shellcheck)"
#   docs   .github/workflows/doc-validation.yml     "Doc anchors + agent contract"
#
# EXIT
#   0  every requested lane green
#   1  a lane red, lane/workflow drift, or the lanes changed or created files
#   2  usage error, tooling missing, the image could not be built, or the tree was
#      not clean before the lanes ran
set -uo pipefail

_SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$_SCRIPT_PATH")" && pwd)"
# The scaffold image rule (ALI_SCAFFOLD_REL, ali_extract_image), shared with the
# seed script so the simulated tree is the seeded tree.
# shellcheck source=agents/scripts/core/lib/agent-layer-image.sh
. "$SCRIPT_DIR/lib/agent-layer-image.sh"
MANIFEST_REL="$ALI_SCAFFOLD_REL/docs/seed-paths.txt"

LANE_ORDER=(bats shell docs)
declare -A LANE_WORKFLOW=(
    [bats]=".github/workflows/agentic-selftests.yml"
    [shell]=".github/workflows/shell-lint.yml"
    [docs]=".github/workflows/doc-validation.yml"
)
# Newline-separated commands, run in order; each line must also appear verbatim
# in that lane's workflow (the drift check).
# Tools each lane cannot run honestly without: test-all.sh and test-shell-lint.sh
# degrade to warn-only passes when they are missing, and a simulation that silently
# skips is worse than none.
declare -A LANE_TOOLS=(
    [bats]="bats shellcheck"
    [shell]="shellcheck"
    [docs]=""
)
declare -A LANE_COMMANDS=(
    [bats]=$'bash agents/scripts/core/setup-harness.sh claude-code\nbash scripts/dev/test-all.sh --ci'
    [shell]=$'bash agents/scripts/core/test-lint-bash.sh\nbash agents/scripts/core/test-shell-lint.sh'
    [docs]=$'bash scripts/dev/test-docs.sh'
)

REV="HEAD"
DIR=""
KEEP=0
RUN_ONLY=""
BUILD_ONLY=0
LANES=()
ADDED_ORIGIN=0
ORIGIN_MIRROR=""

usage() {
    sed -n '2,/^set -uo pipefail$/p' "$_SCRIPT_PATH" | sed -e '$d' -e 's/^# \{0,1\}//'
}

die() {
    local code="$1"; shift
    printf 'agent-layer-sim: %s\n' "$*" >&2
    exit "$code"
}

say() { printf '%s\n' "$*"; }

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --help|-h)     usage; exit 0 ;;
            --keep)        KEEP=1; shift ;;
            --build-only)  BUILD_ONLY=1; shift ;;
            --rev)         [ "$#" -ge 2 ] || die 2 "--rev needs a value"
                           REV="$2"; shift 2 ;;
            --rev=*)       REV="${1#--rev=}"; shift ;;
            --dir)         [ "$#" -ge 2 ] || die 2 "--dir needs a value"
                           DIR="$2"; shift 2 ;;
            --dir=*)       DIR="${1#--dir=}"; shift ;;
            --run-only)    [ "$#" -ge 2 ] || die 2 "--run-only needs a value"
                           RUN_ONLY="$2"; shift 2 ;;
            --run-only=*)  RUN_ONLY="${1#--run-only=}"; shift ;;
            --lane)        [ "$#" -ge 2 ] || die 2 "--lane needs a value"
                           LANES+=("$2"); shift 2 ;;
            --lane=*)      LANES+=("${1#--lane=}"); shift ;;
            *)             usage >&2; die 2 "unknown argument: $1" ;;
        esac
    done
    if [ "$BUILD_ONLY" -eq 1 ] && { [ -n "$RUN_ONLY" ] || [ "${#LANES[@]}" -gt 0 ]; }; then
        die 2 "--build-only runs no lanes; it cannot be combined with --run-only or --lane"
    fi
    if [ "${#LANES[@]}" -eq 0 ] && [ "$BUILD_ONLY" -eq 0 ]; then
        LANES=("${LANE_ORDER[@]}")
    fi
    local lane
    for lane in "${LANES[@]}"; do
        [ -n "${LANE_WORKFLOW[$lane]:-}" ] || die 2 "unknown lane: $lane (expected bats, shell or docs)"
    done
    if [ -n "$RUN_ONLY" ] && { [ -n "$DIR" ] || [ "$REV" != "HEAD" ] || [ "$KEEP" -eq 1 ]; }; then
        die 2 "--run-only takes an existing tree; it cannot be combined with --dir, --rev or --keep"
    fi
}

preflight() {
    local tool lane
    for tool in git tar; do
        command -v "$tool" >/dev/null 2>&1 || die 2 "$tool not on PATH"
    done
    # Only the selected lanes' tools: `--lane docs` needs neither bats nor shellcheck.
    for lane in "${LANES[@]}"; do
        for tool in ${LANE_TOOLS[$lane]}; do
            command -v "$tool" >/dev/null 2>&1 || die 2 "$tool not on PATH — the $lane lane cannot run"
        done
    done
}

# Build the layer image of $REV at $DIR: the manifest pathspecs plus the scaffold
# image, as one commit on `develop` with an `origin/develop` to diff against.
build_image() {
    local src
    src="$(git rev-parse --show-toplevel 2>/dev/null)" || die 2 "not inside a git repo"
    git -C "$src" rev-parse --verify --quiet "$REV^{commit}" >/dev/null || die 2 "not a commit: $REV"
    local sha
    sha="$(git -C "$src" rev-parse "$REV^{commit}")"

    if [ -z "$DIR" ]; then
        DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-layer-sim.XXXXXX")" || die 2 "mktemp failed"
    else
        [ ! -e "$DIR" ] || die 2 "--dir exists: $DIR (refusing to build into a non-empty path)"
        mkdir -p "$DIR" || die 2 "cannot create $DIR"
    fi
    DIR="$(cd "$DIR" && pwd)"

    local -a specs
    mapfile -t specs < <(git -C "$src" show "$sha:$MANIFEST_REL" | grep -vE '^[[:space:]]*(#|$)')
    [ "${#specs[@]}" -gt 0 ] || die 2 "manifest at $sha:$MANIFEST_REL has no pathspecs"

    git -C "$src" archive --format=tar "$sha" -- "${specs[@]}" | tar -x -C "$DIR" \
        || die 2 "git archive of the manifest pathspecs failed"

    # The scaffold image, by the same rule seed phase 4 copies it with.
    ali_extract_image "$src" "$sha" "$DIR" || die 2 "could not write the scaffold image of $sha"

    (
        cd "$DIR" || exit 2
        unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
        git init -q -b develop . \
            && git add -A \
            && git -c user.name=agent-layer-sim -c user.email=agent-layer-sim@invalid \
                   -c commit.gpgsign=false commit -q -m "sim: layer image of $sha" \
            && git clone -q --bare . "$DIR.origin.git" \
            && git remote add origin "$DIR.origin.git" \
            && git fetch -q origin \
            && git branch -q -u origin/develop
    ) || die 2 "could not initialise the image repo at $DIR"

    say "image: $DIR  (manifest + scaffold of $sha, $(git -C "$DIR" ls-files | wc -l | tr -d ' ') files)"
}

# The lanes diff against origin/develop. A rewritten seed clone has no origin
# (filter-repo removes it by design), so give it a throwaway one and take it away
# again before phase 5 re-points origin at the real target.
ensure_origin() {
    if git -C "$DIR" remote get-url origin >/dev/null 2>&1; then
        return 0
    fi
    ORIGIN_MIRROR="$(mktemp -d "${TMPDIR:-/tmp}/agent-layer-sim-origin.XXXXXX")" || die 2 "mktemp failed"
    rmdir "$ORIGIN_MIRROR" || die 2 "cannot prepare $ORIGIN_MIRROR"
    git clone -q --bare "$DIR" "$ORIGIN_MIRROR" || die 2 "cannot mirror $DIR as a throwaway origin"
    git -C "$DIR" remote add origin "$ORIGIN_MIRROR" || die 2 "cannot add throwaway origin"
    git -C "$DIR" fetch -q origin || die 2 "cannot fetch throwaway origin"
    ADDED_ORIGIN=1
}

drop_origin() {
    [ "$ADDED_ORIGIN" -eq 1 ] || return 0
    git -C "$DIR" remote remove origin >/dev/null 2>&1 || true
    rm -rf "$ORIGIN_MIRROR"
    ADDED_ORIGIN=0
}

# The repo commands a workflow runs, in order: every `bash ...` line of it, whether
# a one-line `run: bash ...` or a line inside a `run: |` block. Comment lines start
# with `#` and never match. Environment setup (apt-get, git fetch, --version
# probes) is not a repo command and is not simulated.
workflow_commands() {
    awk '{
        line = $0
        sub(/^[[:space:]]*(-[[:space:]]+)?(run:[[:space:]]*)?/, "", line)
        sub(/[[:space:]]+$/, "", line)
        if (line ~ /^bash /) print line
    }' "$1"
}

# The lane's commands and the workflow's repo commands must be IDENTICAL — same
# lines, same order, nothing extra on either side. A substring match would accept
# `test-all.sh --ci --new-flag` in CI while the simulation runs the old command and
# clears phase 4c on a gate CI no longer runs.
check_drift() {
    local lane="$1" wf="$DIR/${LANE_WORKFLOW[$1]}" delta
    if [ ! -f "$wf" ]; then
        printf '  FAIL  %s: workflow %s is missing from the image\n' "$lane" "${LANE_WORKFLOW[$lane]}" >&2
        return 1
    fi
    if ! delta="$(diff <(printf '%s\n' "${LANE_COMMANDS[$lane]}") <(workflow_commands "$wf"))"; then
        printf '  FAIL  %s: LANE_COMMANDS and the repo commands in %s differ (< simulator, > workflow):\n' \
            "$lane" "${LANE_WORKFLOW[$lane]}" >&2
        printf '%s\n' "$delta" | sed 's/^/          /' >&2
        return 1
    fi
}

# Run one lane in the image exactly as its workflow does: from the tree root, with
# both roots on the tree (the layer is its own host when it runs standalone), and
# stdin closed as on a runner — a command reading stdin must not swallow the rest
# of the lane's command list.
run_lane() {
    local lane="$1" log="$2" cmd rc=0
    : > "$log" || return 1
    while IFS= read -r cmd; do
        printf '\n$ %s\n' "$cmd" >> "$log"
        # shellcheck disable=SC2086  # cmd is a fixed, word-split command line
        ( cd "$DIR" && env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE \
              PROJECT_ROOT="$DIR" AGENT_LAYER_ROOT="$DIR" $cmd ) < /dev/null >> "$log" 2>&1 || { rc=$?; break; }
    done <<< "${LANE_COMMANDS[$lane]}"
    return "$rc"
}

# test-all.sh --ci turns a wrapper's exit 2 into "SKIPPED (ci): missing
# binary/build". In a standalone layer an exit 2 is almost always a file the
# manifest does not carry — the exact gap this simulation exists to find — so the
# bats lane may carry none. A suite that genuinely needs the consuming product
# names its host subject in test-all.sh LAYER_HOST_SUT_RE and skips explicitly.
check_no_exit2_skips() {
    local log="$1" hits
    hits="$(awk '/^# (agents|scripts)\//{s=$2} /^SKIPPED \(ci\): exit 2/{print s}' "$log")"
    [ -z "$hits" ] && return 0
    printf '  FAIL  bats: suite(s) skipped by exit 2 — a missing seeded file, not a missing tool:\n' >&2
    printf '%s\n' "$hits" | sed 's/^/          /' >&2
    printf '        Seed what each one needs, or name its host subject in test-all.sh LAYER_HOST_SUT_RE.\n' >&2
    return 1
}

main() {
    # An inherited GIT_DIR (a hook, a linked worktree) would aim every `git -C` below
    # at the SOURCE repository — git honours GIT_DIR over -C — so a checkout or an
    # update-ref meant for a scratch clone would move the source's HEAD and refs.
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
    parse_args "$@"
    preflight

    if [ -n "$RUN_ONLY" ]; then
        [ -d "$RUN_ONLY/.git" ] || die 2 "--run-only: not a git work tree: $RUN_ONLY"
        DIR="$(cd "$RUN_ONLY" && pwd)"
        KEEP=1
    else
        build_image
    fi
    if [ "$BUILD_ONLY" -eq 1 ]; then
        say "built — no lanes run (--build-only); image kept at $DIR"
        exit 0
    fi
    # The post-run check attributes every change in the tree to the lanes, so the
    # tree has to start clean: nothing modified, nothing untracked outside .gitignore.
    local before
    before="$(git -C "$DIR" status --porcelain --untracked-files=all)" \
        || die 2 "git status failed in $DIR"
    if [ -n "$before" ]; then
        printf '%s\n' "$before" | sed 's/^/          /' >&2
        die 2 "$DIR is not clean before the lanes run — commit or remove the paths above"
    fi
    trap drop_origin EXIT
    ensure_origin

    local logs="$DIR.lane-logs"
    mkdir -p "$logs" || die 2 "cannot create $logs"

    local lane failed=0 ran=0
    for lane in "${LANES[@]}"; do
        if ! check_drift "$lane"; then
            failed=1
            continue
        fi
        if run_lane "$lane" "$logs/$lane.log" \
            && { [ "$lane" != bats ] || check_no_exit2_skips "$logs/$lane.log"; }; then
            printf '  PASS  %-5s %s\n' "$lane" "${LANE_WORKFLOW[$lane]}"
        else
            printf '  FAIL  %-5s %s — log: %s\n' "$lane" "${LANE_WORKFLOW[$lane]}" "$logs/$lane.log" >&2
            tail -n 25 "$logs/$lane.log" | sed 's/^/          /' >&2
            failed=1
        fi
        ran=$((ran + 1))
    done

    # The lanes must leave the tree as they found it — no tracked file rewritten, no
    # new file outside .gitignore (a rotated applied-*.md, say): in --run-only mode
    # that tree is published next, and anywhere else it means a test reaches outside
    # its fixture. Ignored paths (the harness adapters, /build/) are runtime output.
    local changed
    if ! changed="$(git -C "$DIR" status --porcelain --untracked-files=all)"; then
        printf '  FAIL  git status failed in %s — cannot show the lanes left it untouched\n' "$DIR" >&2
        failed=1
    elif [ -n "$changed" ]; then
        printf '  FAIL  the lanes changed or created files outside .gitignore:\n' >&2
        printf '%s\n' "$changed" | sed 's/^/          /' >&2
        failed=1
    fi

    drop_origin
    if [ "$failed" -ne 0 ]; then
        say "RED — image kept at $DIR (lane logs: $logs)"
        exit 1
    fi
    [ "$ran" -gt 0 ] || die 1 "no lane ran — refusing to report green"
    if [ "$KEEP" -eq 1 ]; then
        say "GREEN — $ran lane(s); image kept at $DIR (lane logs: $logs)"
    else
        rm -rf "$DIR" "$DIR.origin.git" "$logs"
        say "GREEN — $ran lane(s)"
    fi
}

main "$@"
