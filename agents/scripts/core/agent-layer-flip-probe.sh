#!/usr/bin/env bash
# agent-layer-flip-probe.sh — run the host's agent-layer invocations in a simulated
# post-flip checkout and compare them with the same invocations today.
#
# Plan: docs/plans/agent-surface-extraction-repo.md — Phase C prep (row 11c's
# verification battery, runnable before the layer repo exists).
#
# WHY THIS EXISTS
#   After the Phase C flip the layer's scripts live under agent-layer/ while the
#   host keeps plans, backlog entries, Source/, agents/project/ and the workflows.
#   A layer script that finds its tree from its OWN location then reads the layer
#   where it meant the host: it fails, or worse, passes having checked nothing. The
#   first probe (2026-10-04) found both kinds in a tree every host gate passed on —
#   a nudge that went silent, link and shell-lint and workflow checks that quietly
#   scanned the layer instead of the host, a contract check that lost the project
#   agents. Only running the invocations in the post-flip layout finds that class.
#
# NOT named test-*.sh ON PURPOSE: it clones the repository twice and builds the
# layer image, and test-all.sh would enrol it into every agentic-selftests run.
#
# USAGE
#   agent-layer-flip-probe.sh [--rev REV] [--dir DIR] [--keep] [--only NAME]...
#   agent-layer-flip-probe.sh --list
#
#   --rev REV    Commit to probe (default HEAD). Built from git objects, so
#                uncommitted edits are NOT probed.
#   --dir DIR    Where to build. Must not exist. Default: a mktemp dir.
#   --keep       Keep DIR after a green run (a red run always keeps it).
#   --only NAME  Run only this probe (repeatable).
#   --list       Print the probes and exit.
#
# LAYOUT (under DIR)
#   today/   a clone of REV: the layout of today, the reference run
#   layer/   the layer image of REV (agent-layer-sim.sh --build-only)
#   host/    a clone of REV with every manifest path removed except the mirrored
#            files (docs/mirrored-paths.txt), and layer/ mounted as the agent-layer/
#            submodule: Phase C row 11's layout
#
# MODES — each probe runs three times:
#   today   in today/, with no root variables
#   local   in host/, with no root variables: a hook or a developer's shell, where
#           the roots must come from the superproject
#   ci      in host/, with the row-12 workflow values PROJECT_ROOT=. and
#           AGENT_LAYER_ROOT=agent-layer
#
# EXPECTATIONS — per probe, against the today run:
#   rc            local and ci exit as today does
#   same          ...and print the same last line (paths normalised)
#   match:ERE     ...and their output matches ERE (a scope check: proof the probe
#                 read the host, for checks whose counts legitimately change once the
#                 layer's files are no longer in the host tree)
#
# EXIT
#   0  every probe met its expectation in both post-flip modes
#   1  a probe did not
#   2  usage error, tooling missing, or the layout could not be built
set -uo pipefail

_SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$_SCRIPT_PATH")" && pwd)"
SIM_SCRIPT="$SCRIPT_DIR/agent-layer-sim.sh"
MANIFEST_REL="agents/scripts/core/seed-agent-layer-repo.d/docs/seed-paths.txt"
MIRRORS_REL="docs/mirrored-paths.txt"

# name <TAB> expectation <TAB> command. {L} is the layer prefix: empty today,
# `agent-layer/` post-flip — exactly how a host caller's path changes at the flip.
PROBES=(
    $'followup-due-nudge\tsame\tbash {L}agents/scripts/core/followup-due-nudge.sh'
    $'plan-archival-owed\tsame\tbash {L}agents/scripts/core/plan-archival-owed.sh --list'
    $'work-item-owed\tsame\tbash {L}agents/scripts/core/work-item-owed.sh --list'
    $'audit-doc-status-owed\tsame\tbash {L}agents/scripts/core/audit-doc-status-owed.sh --list'
    $'historical-ledger-reconcile\tsame\tbash {L}agents/scripts/core/historical-review-ledger-reconcile.sh'
    $'markdown-links\tmatch:scanned ([2-9][0-9]{2}|[1-9][0-9]{3,}) markdown\tbash {L}agents/scripts/core/test-markdown-links.sh --all'
    $'shell-lint\tmatch:scripts/dev/pre-ship\\.sh\tbash {L}agents/scripts/core/test-shell-lint.sh --list-targets'
    $'workflow-yaml\tsame\tbash {L}agents/scripts/core/test-workflow-yaml.sh'
    $'doc-anchors\tsame\tbash {L}agents/scripts/core/test-doc-anchors.sh'
    $'plan-doc-table-probe\tsame\tbash {L}agents/scripts/core/test-plan-doc-table-probe.sh'
    $'pre-push-merged-pr-guard\tsame\tbash {L}agents/scripts/core/test-pre-push-merged-pr-guard.sh'
    $'agent-contract\tsame\tbash {L}agents/scripts/core/test-agent-contract.sh'
    $'portable-agent-vexp\tsame\tbash {L}agents/scripts/core/test-portable-agent-vexp.sh'
    $'skill-vs-agent-parity\tsame\tbash {L}agents/scripts/core/test-skill-vs-agent-parity.sh'
    $'subsystem-docs\tsame\tbash {L}agents/scripts/project/test-subsystem-docs.sh'
    $'plan-staleness\tsame\tbash {L}agents/scripts/project/test-plan-staleness.sh'
    $'required-context-parity\tsame\tbash {L}agents/scripts/core/test-required-context-parity.sh'
    $'workflow-job-mask\tsame\tbash {L}agents/scripts/core/test-workflow-job-mask.sh'
    $'plan-naming\tsame\tbash {L}agents/scripts/core/test-plan-naming.sh'
    $'plan-index\tsame\tbash {L}agents/scripts/core/test-plan-index.sh'
    $'plan-claim-anchors\tsame\tbash {L}agents/scripts/core/test-plan-claim-anchors.sh --all'
    $'lint-rules-scope\tmatch:Source/\tbash {L}agents/scripts/project/test-lint-rules.sh --scan-offline'
    $'mirrored-paths\trc\tbash scripts/dev/test-mirrored-paths.sh'
)

REV="HEAD"
DIR=""
KEEP=0
ONLY=()

usage() {
    sed -n '2,/^set -uo pipefail$/p' "$_SCRIPT_PATH" | sed -e '$d' -e 's/^# \{0,1\}//'
}

die() {
    local code="$1"; shift
    printf 'agent-layer-flip-probe: %s\n' "$*" >&2
    exit "$code"
}

say() { printf '%s\n' "$*"; }

probe_field() { # probe_field <entry> <1|2|3>
    printf '%s\n' "$1" | cut -f"$2"
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --help|-h) usage; exit 0 ;;
            --list)    local p; for p in "${PROBES[@]}"; do probe_field "$p" 1; done; exit 0 ;;
            --keep)    KEEP=1; shift ;;
            --rev)     [ "$#" -ge 2 ] || die 2 "--rev needs a value"; REV="$2"; shift 2 ;;
            --rev=*)   REV="${1#--rev=}"; shift ;;
            --dir)     [ "$#" -ge 2 ] || die 2 "--dir needs a value"; DIR="$2"; shift 2 ;;
            --dir=*)   DIR="${1#--dir=}"; shift ;;
            --only)    [ "$#" -ge 2 ] || die 2 "--only needs a value"; ONLY+=("$2"); shift 2 ;;
            --only=*)  ONLY+=("${1#--only=}"); shift ;;
            *)         usage >&2; die 2 "unknown argument: $1" ;;
        esac
    done
    local name p found
    for name in ${ONLY[@]+"${ONLY[@]}"}; do
        found=0
        for p in "${PROBES[@]}"; do [ "$(probe_field "$p" 1)" = "$name" ] && found=1; done
        [ "$found" -eq 1 ] || die 2 "unknown probe: $name (see --list)"
    done
}

selected() { # selected <name>
    [ "${#ONLY[@]}" -eq 0 ] && return 0
    local n
    for n in "${ONLY[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

# Build today/, layer/ and host/ under $DIR from the commit $1.
build_layout() {
    local src="$1" sha="$2"
    { git clone -q --no-hardlinks "$src" "$DIR/today" && git -C "$DIR/today" checkout -q --detach "$sha"; } \
        || die 2 "cannot clone $sha into $DIR/today"
    bash "$SIM_SCRIPT" --build-only --rev "$sha" --dir "$DIR/layer" >/dev/null \
        || die 2 "cannot build the layer image of $sha"

    { git clone -q --no-hardlinks "$src" "$DIR/host" && git -C "$DIR/host" checkout -q -b flip-probe "$sha"; } \
        || die 2 "cannot clone $sha into $DIR/host"
    local -a specs keep
    mapfile -t specs < <(git -C "$src" show "$sha:$MANIFEST_REL" | grep -vE '^[[:space:]]*(#|$)')
    [ "${#specs[@]}" -gt 0 ] || die 2 "manifest at $sha has no pathspecs"
    mapfile -t keep < <(git -C "$src" show "$sha:$MIRRORS_REL" 2>/dev/null | tr -d '\r' | grep -vE '^[[:space:]]*(#|$)')
    [ "${#keep[@]}" -gt 0 ] || die 2 "$MIRRORS_REL at $sha lists no mirrored path"
    (
        cd "$DIR/host" || exit 2
        unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
        git rm -r -q --ignore-unmatch -- "${specs[@]}" >/dev/null || exit 2
        git checkout -q "$sha" -- "${keep[@]}" || exit 2
        git -c user.name=flip-probe -c user.email=flip-probe@invalid -c commit.gpgsign=false \
            commit -q -m "probe: remove the layer's paths (row 11)" || exit 2
        git -c protocol.file.allow=always submodule add -q -b develop "$DIR/layer" agent-layer || exit 2
        git -c user.name=flip-probe -c user.email=flip-probe@invalid -c commit.gpgsign=false \
            commit -q -m "probe: mount agent-layer (row 11)" || exit 2
        # Delta gates diff against origin/develop; the probe's own commits are not a delta.
        git update-ref refs/remotes/origin/develop HEAD
        git -C agent-layer update-ref refs/remotes/origin/develop HEAD
    ) || die 2 "cannot build the post-flip host at $DIR/host"
    git -C "$DIR/today" update-ref refs/remotes/origin/develop HEAD
}

# run_probe <tree> <layer-prefix> <command> [env assignments...] — prints the exit
# code on the first line and the output after it; split_rc / split_out take it apart.
run_probe() {
    local tree="$1" prefix="$2" cmd="$3"; shift 3
    local out rc
    cmd="${cmd//\{L\}/$prefix}"
    # shellcheck disable=SC2086  # cmd is a fixed, word-split command line
    out="$(cd "$tree" && env -u PROJECT_ROOT -u AGENT_LAYER_ROOT -u PC_CONFIG_FILE \
              -u SMATCHET_PROJECT_CONFIG -u CLAUDE_PROJECT_DIR -u GIT_DIR -u GIT_WORK_TREE \
              -u GIT_INDEX_FILE "$@" $cmd 2>&1 < /dev/null)"
    rc=$?
    printf '%s\n%s\n' "$rc" "$out"
}

split_rc()  { printf '%s\n' "${1%%$'\n'*}"; }
split_out() { case "$1" in *$'\n'*) printf '%s\n' "${1#*$'\n'}" ;; esac; }

last_line() { # last non-empty line, with the probe trees' paths normalised
    grep -v '^[[:space:]]*$' | tail -n 1 | sed -e "s#$DIR/today#<root>#g" -e "s#$DIR/host#<root>#g"
}

main() {
    parse_args "$@"
    local tool
    for tool in git bash tar; do command -v "$tool" >/dev/null 2>&1 || die 2 "$tool not on PATH"; done
    [ -f "$SIM_SCRIPT" ] || die 2 "simulator missing: $SIM_SCRIPT"

    local src sha
    src="$(git rev-parse --show-toplevel 2>/dev/null)" || die 2 "not inside a git repo"
    sha="$(git -C "$src" rev-parse --verify --quiet "$REV^{commit}")" || die 2 "not a commit: $REV"
    if [ -z "$DIR" ]; then
        DIR="$(mktemp -d "${TMPDIR:-/tmp}/agent-layer-flip-probe.XXXXXX")" || die 2 "mktemp failed"
    else
        [ ! -e "$DIR" ] || die 2 "--dir exists: $DIR (refusing to build into a non-empty path)"
        mkdir -p "$DIR" || die 2 "cannot create $DIR"
        DIR="$(cd "$DIR" && pwd)"
    fi
    build_layout "$src" "$sha"
    say "layout: $DIR  (today/, layer/, host/ with agent-layer/ mounted — commit $sha)"

    local entry name expect cmd ran=0 failed=0 t l c t_rc l_rc c_rc mode verdict
    printf '%-28s %-6s %-6s %-6s %s\n' PROBE TODAY LOCAL CI RESULT
    for entry in "${PROBES[@]}"; do
        name="$(probe_field "$entry" 1)"; expect="$(probe_field "$entry" 2)"; cmd="$(probe_field "$entry" 3)"
        selected "$name" || continue
        t="$(run_probe "$DIR/today" "" "$cmd")"
        l="$(run_probe "$DIR/host" "agent-layer/" "$cmd")"
        c="$(run_probe "$DIR/host" "agent-layer/" "$cmd" PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer)"
        t_rc="$(split_rc "$t")"; l_rc="$(split_rc "$l")"; c_rc="$(split_rc "$c")"
        verdict="ok"
        for mode in local ci; do
            local out rc
            if [ "$mode" = local ]; then out="$(split_out "$l")"; rc="$l_rc"; else out="$(split_out "$c")"; rc="$c_rc"; fi
            if [ "$rc" != "$t_rc" ]; then
                verdict="FAIL ($mode exit $rc, today $t_rc)"; break
            fi
            case "$expect" in
                rc) ;;
                same)
                    if [ "$(printf '%s\n' "$out" | last_line)" != "$(split_out "$t" | last_line)" ]; then
                        verdict="FAIL ($mode last line differs)"; break
                    fi ;;
                match:*)
                    if ! printf '%s\n' "$out" | grep -qE -- "${expect#match:}"; then
                        verdict="FAIL ($mode output lacks /${expect#match:}/)"; break
                    fi ;;
                *) die 2 "probe $name: unknown expectation '$expect'" ;;
            esac
        done
        printf '%-28s %-6s %-6s %-6s %s\n' "$name" "$t_rc" "$l_rc" "$c_rc" "$verdict"
        if [ "$verdict" != ok ]; then
            failed=$((failed + 1))
            printf '    today: %s\n    local: %s\n    ci:    %s\n' \
                "$(split_out "$t" | last_line | cut -c1-200)" \
                "$(split_out "$l" | last_line | cut -c1-200)" \
                "$(split_out "$c" | last_line | cut -c1-200)"
        fi
        ran=$((ran + 1))
    done

    [ "$ran" -gt 0 ] || die 1 "no probe ran — refusing to report green"
    if [ "$failed" -ne 0 ]; then
        say "RED — $failed of $ran probe(s) differ after the flip; layout kept at $DIR"
        exit 1
    fi
    if [ "$KEEP" -eq 1 ]; then
        say "GREEN — $ran probe(s) match after the flip; layout kept at $DIR"
    else
        rm -rf "$DIR"
        say "GREEN — $ran probe(s) match after the flip"
    fi
}

main "$@"
