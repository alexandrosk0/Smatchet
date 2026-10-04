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
# NOT named test-*.sh ON PURPOSE: it clones the repository twice, copies the
# post-flip host twice more and builds the layer image, and test-all.sh would
# enrol it into every agentic-selftests run.
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
#   today/     a clone of REV: the layout of today, the reference run
#   layer/     the layer image of REV (agent-layer-sim.sh --build-only)
#   host/      a clone of REV with every manifest path removed except the mirrored
#              files (docs/mirrored-paths.txt), and layer/ mounted as the
#              agent-layer/ submodule: Phase C row 11's layout
#   host-ci/   a copy of host/, so the ci run never reads what the local run wrote
#              (setup-harness provisions .claude/ in each, and the adapter probes
#              read it back)
#   host-ctl/  another copy, for the control run
#
# MODES — each probe runs three times, and a probe marked ctl a fourth:
#   today    in today/, with no root variables
#   local    in host/, with no root variables: a hook or a developer's shell, where
#            the roots must come from the superproject
#   ci       in host-ci/, with the row-12 workflow values PROJECT_ROOT=. and
#            AGENT_LAYER_ROOT=agent-layer
#   control  in host-ctl/, with PROJECT_ROOT=agent-layer: the defect this probe
#            hunts, a script reading host content from the layer's tree
#
# CONTROL — a match proves nothing unless the probe could have failed. A probe
# marked ctl reads host content, so its control run must come out DIFFERENT from
# its ci run (exit code, last line or ERE match, with the layer mount's path folded
# into the root so a path echo alone is not a difference); if it does not, the
# probe cannot tell the host from the layer and reports BLIND, which fails the run.
# A probe marked - reads only layer content or provisions: a wrong layer path
# fails its local and ci runs outright, so a control would add nothing.
#
# EXPECTATIONS — per probe, against the today run:
#   rc            local and ci exit as today does
#   same          ...and print the same last line (paths normalised)
#   match:ERE     ...and their output matches ERE (a scope check: proof the probe
#                 read the host, for checks whose counts legitimately change once the
#                 layer's files are no longer in the host tree)
#   pending:ROW:ERE
#                 the output must match ERE, as for match:, but an exit code that
#                 differs from today is OWED to Phase C row ROW, not a failure of
#                 this tree: work the flip itself does and that cannot land before
#                 it. Printed as PENDING with the row, counted apart from the
#                 matches, and the expectation must become match:ERE in the PR that
#                 lands the row.
#
# EXIT
#   0  every probe met its expectation in both post-flip modes (PENDING ones
#      included — each names the Phase C row that owns it), and every ctl probe's
#      control run came out different
#   1  a probe did not, or a ctl probe is BLIND
#   2  usage error, tooling missing, or the layout could not be built
set -uo pipefail

_SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$_SCRIPT_PATH")" && pwd)"
SIM_SCRIPT="$SCRIPT_DIR/agent-layer-sim.sh"
MANIFEST_REL="agents/scripts/core/seed-agent-layer-repo.d/docs/seed-paths.txt"
MIRRORS_REL="docs/mirrored-paths.txt"

# name <TAB> expectation <TAB> control (ctl or -) <TAB> command. {L} is the layer
# prefix: empty today, `agent-layer/` post-flip — exactly how a host caller's path
# changes at the flip.
PROBES=(
    # Provisioning first: the adapter probes below read what setup-harness writes,
    # which is what checks it — so it carries no control of its own.
    $'setup-harness\trc\t-\tbash {L}agents/scripts/core/setup-harness.sh claude-code'
    $'adapter-drift\tmatch:PASS\tctl\tbash {L}agents/scripts/core/test-adapter-drift.sh'
    $'harness-provisioned\tsame\tctl\tbash {L}agents/scripts/core/check-harness-provisioned.sh'
    $'followup-due-nudge\tsame\tctl\tbash {L}agents/scripts/core/followup-due-nudge.sh'
    $'plan-archival-owed\tsame\tctl\tbash {L}agents/scripts/core/plan-archival-owed.sh --list'
    $'work-item-owed\tsame\tctl\tbash {L}agents/scripts/core/work-item-owed.sh --list'
    $'audit-doc-status-owed\tsame\tctl\tbash {L}agents/scripts/core/audit-doc-status-owed.sh --list'
    $'historical-ledger-reconcile\tsame\tctl\tbash {L}agents/scripts/core/historical-review-ledger-reconcile.sh'
    # Host links into the layer (docs/agent-rules/, agents/scripts/, AGENTS.md ...)
    # dangle until row 15's cross-boundary sweep rewrites them to agent-layer/;
    # that path does not exist before the flip, so the sweep cannot land first.
    $'markdown-links\tpending:15:scanned ([2-9][0-9]{2}|[1-9][0-9]{3,}) markdown\tctl\tbash {L}agents/scripts/core/test-markdown-links.sh --all'
    $'shell-lint\tmatch:scripts/dev/pre-ship\\.sh\tctl\tbash {L}agents/scripts/core/test-shell-lint.sh --list-targets'
    $'workflow-yaml\tsame\tctl\tbash {L}agents/scripts/core/test-workflow-yaml.sh'
    $'doc-anchors\tsame\tctl\tbash {L}agents/scripts/core/test-doc-anchors.sh'
    $'plan-doc-table-probe\tsame\tctl\tbash {L}agents/scripts/core/test-plan-doc-table-probe.sh'
    $'pre-push-merged-pr-guard\tsame\tctl\tbash {L}agents/scripts/core/test-pre-push-merged-pr-guard.sh'
    $'agent-contract\tsame\tctl\tbash {L}agents/scripts/core/test-agent-contract.sh'
    $'portable-agent-vexp\tsame\tctl\tbash {L}agents/scripts/core/test-portable-agent-vexp.sh'
    $'skill-vs-agent-parity\tsame\tctl\tbash {L}agents/scripts/core/test-skill-vs-agent-parity.sh'
    $'subsystem-docs\tsame\tctl\tbash {L}agents/scripts/project/test-subsystem-docs.sh'
    $'plan-staleness\tsame\tctl\tbash {L}agents/scripts/project/test-plan-staleness.sh'
    $'required-context-parity\tsame\tctl\tbash {L}agents/scripts/core/test-required-context-parity.sh'
    $'workflow-job-mask\tsame\tctl\tbash {L}agents/scripts/core/test-workflow-job-mask.sh'
    $'plan-naming\tsame\tctl\tbash {L}agents/scripts/core/test-plan-naming.sh'
    $'plan-index\tsame\tctl\tbash {L}agents/scripts/core/test-plan-index.sh'
    $'plan-claim-anchors\tsame\tctl\tbash {L}agents/scripts/core/test-plan-claim-anchors.sh --all'
    $'lint-rules-scope\tmatch:Source/\tctl\tbash {L}agents/scripts/project/test-lint-rules.sh --scan-offline'
    $'fleet-preflight\tsame\tctl\tbash {L}agents/scripts/core/fleet-preflight.sh --selftest'
    # The audit drivers read only layer files (their python beside them): a wrong
    # path fails the local and ci runs outright.
    $'dead-export-audit\tsame\t-\tbash {L}agents/scripts/core/test-dead-export-audit.sh'
    $'small-helper-audit\tsame\t-\tbash {L}agents/scripts/core/test-small-helper-audit.sh'
    # The HOST's required contexts (the layer's config names only its three lanes).
    $'branch-protection-config\tmatch:"Windows \\+ MSVC"\tctl\tenv REPO=probe/host bash {L}agents/scripts/core/setup-branch-protection.sh --dry-run'
    $'mirrored-paths\trc\tctl\tbash scripts/dev/test-mirrored-paths.sh'
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

probe_field() { # probe_field <entry> <1|2|3|4>
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
    # One tree per post-flip mode: a run that writes (setup-harness provisions
    # .claude/) must not hand the next mode a tree it did not build itself. The
    # submodule's .git is a relative gitdir file, so a plain copy stays coherent.
    { cp -a "$DIR/host" "$DIR/host-ci" && cp -a "$DIR/host" "$DIR/host-ctl"; } \
        || die 2 "cannot copy the post-flip host"
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

last_line() { # last line with a word in it, with the probe trees' paths normalised
    grep -E '[[:alnum:]]' | tail -n 1 | sed -E "s#$DIR/(today|host-ci|host-ctl|host)#<root>#g"
}

# outcome <expectation> <rc> <output> [fold] — what a control run must differ in:
# the exit code, the last line and, for an ERE expectation, whether it matched.
# fold maps the layer mount onto the root, so a script that merely echoes the root
# it was handed does not count as having read a different tree.
outcome() {
    local expect="$1" rc="$2" out="$3" fold="${4:-}" line re hit=""
    line="$(printf '%s\n' "$out" | last_line)"
    [ -z "$fold" ] || line="${line//<root>\/agent-layer/<root>}"
    case "$expect" in
        match:*)   re="${expect#match:}" ;;
        pending:*) re="${expect#pending:}"; re="${re#*:}" ;;
        *)         re="" ;;
    esac
    if [ -n "$re" ]; then
        if printf '%s\n' "$out" | grep -qE -- "$re"; then hit=match; else hit=nomatch; fi
    fi
    printf '%s|%s|%s\n' "$rc" "$hit" "$line"
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
    say "layout: $DIR  (today/, layer/, host/ + host-ci/ + host-ctl/ with agent-layer/ mounted — commit $sha)"

    local entry name expect ctl cmd ran=0 failed=0 pending=0 t l c x t_rc l_rc c_rc x_rc mode verdict re owed
    printf '%-28s %-6s %-6s %-6s %-6s %s\n' PROBE TODAY LOCAL CI CTL RESULT
    for entry in "${PROBES[@]}"; do
        name="$(probe_field "$entry" 1)"; expect="$(probe_field "$entry" 2)"
        ctl="$(probe_field "$entry" 3)"; cmd="$(probe_field "$entry" 4)"
        case "$ctl" in ctl|-) ;; *) die 2 "probe $name: unknown control '$ctl'" ;; esac
        selected "$name" || continue
        t="$(run_probe "$DIR/today" "" "$cmd")"
        l="$(run_probe "$DIR/host" "agent-layer/" "$cmd")"
        c="$(run_probe "$DIR/host-ci" "agent-layer/" "$cmd" PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer)"
        t_rc="$(split_rc "$t")"; l_rc="$(split_rc "$l")"; c_rc="$(split_rc "$c")"; x_rc="-"
        verdict="ok"; owed=""
        for mode in local ci; do
            local out rc
            if [ "$mode" = local ]; then out="$(split_out "$l")"; rc="$l_rc"; else out="$(split_out "$c")"; rc="$c_rc"; fi
            case "$expect" in
                pending:*)
                    re="${expect#pending:}"; re="${re#*:}"
                    if ! printf '%s\n' "$out" | grep -qE -- "$re"; then
                        verdict="FAIL ($mode output lacks /$re/)"; break
                    fi
                    if [ "$rc" != "$t_rc" ]; then
                        owed="${expect#pending:}"; owed="${owed%%:*}"
                    fi
                    continue ;;
            esac
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
        if [ "$ctl" = ctl ]; then
            x="$(run_probe "$DIR/host-ctl" "agent-layer/" "$cmd" PROJECT_ROOT=agent-layer AGENT_LAYER_ROOT=agent-layer)"
            x_rc="$(split_rc "$x")"
            if [ "$verdict" = ok ] \
               && [ "$(outcome "$expect" "$x_rc" "$(split_out "$x")" fold)" = "$(outcome "$expect" "$c_rc" "$(split_out "$c")")" ]; then
                verdict="BLIND (the control run, reading the layer as the host, came out the same)"
            fi
        fi
        if [ "$verdict" = ok ] && [ -n "$owed" ]; then
            verdict="PENDING (owed to Phase C row $owed)"
            pending=$((pending + 1))
        fi
        printf '%-28s %-6s %-6s %-6s %-6s %s\n' "$name" "$t_rc" "$l_rc" "$c_rc" "$x_rc" "$verdict"
        case "${verdict%% *}" in FAIL|BLIND) failed=$((failed + 1)) ;; esac
        if [ "${verdict%% *}" != ok ]; then
            printf '    today: %s\n    local: %s\n    ci:    %s\n' \
                "$(split_out "$t" | last_line | cut -c1-200)" \
                "$(split_out "$l" | last_line | cut -c1-200)" \
                "$(split_out "$c" | last_line | cut -c1-200)"
            [ "$x_rc" = - ] || printf '    ctl:   %s\n' "$(split_out "$x" | last_line | cut -c1-200)"
        fi
        ran=$((ran + 1))
    done

    [ "$ran" -gt 0 ] || die 1 "no probe ran — refusing to report green"
    if [ "$failed" -ne 0 ]; then
        say "RED — $failed of $ran probe(s) differ after the flip or cannot tell the trees apart; layout kept at $DIR"
        exit 1
    fi
    local summary="$((ran - pending)) of $ran probe(s) match after the flip"
    [ "$pending" -eq 0 ] || summary="$summary; $pending PENDING, each owed to the Phase C row it names"
    if [ "$KEEP" -eq 1 ]; then
        say "GREEN — $summary; layout kept at $DIR"
    else
        rm -rf "$DIR"
        say "GREEN — $summary"
    fi
}

main "$@"
