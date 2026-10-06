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
#   agent-layer-flip-probe.sh --suite [--rev REV] [--dir DIR] [--keep]
#   agent-layer-flip-probe.sh --list
#
#   --rev REV    Commit to probe (default HEAD). Built from git objects, so
#                uncommitted edits are NOT probed.
#   --dir DIR    Where to build. Must not exist. Default: a mktemp dir.
#   --keep       Keep DIR after a green run (a red run always keeps it).
#   --only NAME  Run only this probe (repeatable).
#   --suite      Instead of the probes, run the whole host suite
#                (scripts/dev/test-all.sh --ci) in today/ and in both post-flip
#                modes, and compare the failing scripts. See SUITE below.
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
#   layer-ctl/ a copy of layer/, for the control run
#
# MODES — each probe runs three times, and a probe marked ctl a fourth:
#   today    in today/, with no root variables
#   local    in host/, with no root variables: a hook or a developer's shell, where
#            the roots must come from the superproject
#   ci       in host-ci/, with the row-12 workflow values PROJECT_ROOT=. and
#            AGENT_LAYER_ROOT=agent-layer
#   control  in layer-ctl/, with both roots on it: the standalone layer, as its own
#            CI runs. It is what a script that climbs to its own tree would read.
#
# CONTROL — a match proves nothing unless the probe could have failed. A probe
# marked ctl reads host content, so its control run must come out DIFFERENT from
# its ci run (exit code, last line or ERE match, paths normalised); if it does not,
# the host and the layer give that probe the same answer, so it cannot tell which
# tree a script read, and it reports BLIND, which fails the run. Such a probe needs
# a fixture (fixture_<name> below) that gives the host something to report. A
# probe marked - reads only layer content or provisions: a wrong layer path fails
# its local and ci runs outright, so a control would add nothing.
#
# SUITE — the probes cover the invocations someone thought to list; the suite
# covers every test driver test-all.sh enrols, which is row 11c's battery. A
# script that fails after the flip but not today is a regression, named with its
# owner: one under agent-layer/ is layer content, which the seed must carry fixed,
# so it FAILS the run; a host script still naming a layer path is the flip PR's to
# rewire (rows 11-16), so it is PENDING. Fewer scripts after the flip than today
# FAILS too: a root guard skipped a root (row 9d). Three full runs, so it takes
# a while; run it before the seed and on the flip's base.
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
# changes at the flip. {STUB} is a directory of stand-in tools (a gh that reports
# every PR merged). The command runs under `bash -c`, from the tree's root.
PROBES=(
    # Provisioning first: the adapter probes below read what setup-harness writes,
    # which is what checks it — so it carries no control of its own.
    $'setup-harness\trc\t-\tbash {L}agents/scripts/core/setup-harness.sh claude-code'
    $'adapter-drift\tmatch:PASS\tctl\tbash {L}agents/scripts/core/test-adapter-drift.sh'
    # The codex adapter's custom agents include the host's agents/project/.
    $'setup-harness-codex\tmatch:^# Source: agents/project/\tctl\tbash {L}agents/scripts/core/setup-harness.sh codex >/dev/null && grep -h "^# Source: agents/project/" .codex/agents/*.toml'
    $'harness-provisioned\tsame\tctl\tbash {L}agents/scripts/core/check-harness-provisioned.sh'
    # The deployed SessionStart commands, run as Claude Code runs them: the session
    # baseline the HEAD-drift guard reads, and the resync that carries hook fixes
    # into .claude/hooks/. A layer script named from the project dir exits 127.
    $'session-banner\tmatch:^branch=\t-\tc="$(jq -r \'.hooks.SessionStart[].hooks[].command | select(contains("session-tree-banner"))\' .claude/settings.json)" && echo \'{"session_id":"flip-probe"}\' | CLAUDE_PROJECT_DIR="$PWD" bash -c "$c" >/dev/null 2>&1; cat .claude/.active-sessions/flip-probe'
    $'hook-sync\tmatch:^synced$\t-\techo "# flip-probe: stale copy" >> .claude/hooks/guard-head-drift.sh && c="$(jq -r \'.hooks.SessionStart[].hooks[].command | select(contains("clear-session-context"))\' .claude/settings.json)" && echo \'{}\' | CLAUDE_PROJECT_DIR="$PWD" bash -c "$c" >/dev/null 2>&1; if cmp -s .claude/hooks/guard-head-drift.sh {L}docs/harness/claude-code/hooks/guard-head-drift.sh; then echo synced; else echo stale; fi'
    # The stand-in gh: a pr-count trigger counts live merges otherwise, which moves
    # between two runs of one probe; offline it counts each tree's own history.
    $'followup-due-nudge\tsame\tctl\tPATH="{STUB}:$PATH" bash {L}agents/scripts/core/followup-due-nudge.sh'
    $'plan-archival-owed\tmatch:plan archival owed: flip-probe-fixture\tctl\tbash {L}agents/scripts/core/plan-archival-owed.sh --list'
    $'work-item-owed\tmatch:99-flip-probe-fixture\tctl\tbash {L}agents/scripts/core/work-item-owed.sh --list'
    $'audit-doc-status-owed\tmatch:FLIP_PROBE_FIXTURE_AUDIT\\.md\tctl\tbash {L}agents/scripts/core/audit-doc-status-owed.sh --list'
    $'historical-ledger-reconcile\tsame\tctl\tbash {L}agents/scripts/core/historical-review-ledger-reconcile.sh'
    # Host links into the layer (docs/agent-rules/, agents/scripts/, AGENTS.md ...)
    # dangle until row 15's cross-boundary sweep rewrites them to agent-layer/;
    # that path does not exist before the flip, so the sweep cannot land first.
    $'markdown-links\tpending:15:scanned ([2-9][0-9]{2}|[1-9][0-9]{3,}) markdown\tctl\tbash {L}agents/scripts/core/test-markdown-links.sh --all'
    $'shell-lint\tmatch:scripts/dev/pre-ship\\.sh\tctl\tbash {L}agents/scripts/core/test-shell-lint.sh --list-targets'
    $'workflow-yaml\tsame\tctl\tbash {L}agents/scripts/core/test-workflow-yaml.sh'
    $'doc-anchors\tmatch:docs/flip-probe-fixture\\.md\tctl\tbash {L}agents/scripts/core/test-doc-anchors.sh'
    $'plan-doc-table-probe\tsame\tctl\tbash {L}agents/scripts/core/test-plan-doc-table-probe.sh'
    $'pre-push-merged-pr-guard\tsame\tctl\tbash {L}agents/scripts/core/test-pre-push-merged-pr-guard.sh'
    $'agent-contract\tsame\tctl\tbash {L}agents/scripts/core/test-agent-contract.sh'
    $'portable-agent-vexp\tmatch:agents/project/flip-probe-fixture\\.md\tctl\tbash {L}agents/scripts/core/test-portable-agent-vexp.sh'
    $'skill-vs-agent-parity\tmatch:PASS: flip-probe-fixture \\(skill\tctl\tbash {L}agents/scripts/core/test-skill-vs-agent-parity.sh'
    $'subsystem-docs\tsame\tctl\tbash {L}agents/scripts/project/test-subsystem-docs.sh'
    $'plan-staleness\tmatch:- flip-probe-fixture-stale\tctl\tPATH="{STUB}:$PATH" bash {L}agents/scripts/project/test-plan-staleness.sh --warn'
    $'required-context-parity\tsame\tctl\tbash {L}agents/scripts/core/test-required-context-parity.sh'
    $'workflow-job-mask\tsame\tctl\tbash {L}agents/scripts/core/test-workflow-job-mask.sh'
    $'plan-naming\tmatch:Flip_Probe_Fixture\\.md\tctl\tbash {L}agents/scripts/core/test-plan-naming.sh'
    $'plan-index\tsame\tctl\tbash {L}agents/scripts/core/test-plan-index.sh'
    $'plan-claim-anchors\tsame\tctl\tbash {L}agents/scripts/core/test-plan-claim-anchors.sh --all'
    # The rule-id contract card the selftest checks is the layer's AGENTS.md.
    $'lint-rules-selftest\tsame\t-\tbash {L}agents/scripts/project/test-lint-rules.sh --selftest'
    # The default mode guards the layer's framework index; --list counts the host's
    # categories, which the bare layer does not have.
    $'backlog-counts\tsame\t-\tbash {L}agents/scripts/core/test-backlog-counts.sh'
    $'backlog-counts-list\tsame\tctl\tbash {L}agents/scripts/core/test-backlog-counts.sh --list'
    $'lint-rules-scope\tmatch:Source/\tctl\tbash {L}agents/scripts/project/test-lint-rules.sh --scan-offline'
    $'fleet-preflight\tmatch:without model: pin\tctl\tbash {L}agents/scripts/core/fleet-preflight.sh flip-probe-fixture-workflow.js'
    # The audit drivers read only layer files (their python beside them): a wrong
    # path fails the local and ci runs outright.
    $'dead-export-audit\tsame\t-\tbash {L}agents/scripts/core/test-dead-export-audit.sh'
    $'small-helper-audit\tsame\t-\tbash {L}agents/scripts/core/test-small-helper-audit.sh'
    # The HOST's required contexts (the layer's config names only its three lanes).
    $'branch-protection-config\tmatch:"Windows \\+ MSVC"\tctl\tenv REPO=probe/host bash {L}agents/scripts/core/setup-branch-protection.sh --dry-run'
    # Host-only gate (the layer does not carry it), so no control. The scope check
    # is the point: post-flip the comparison must be against the agent-layer/
    # mount, and a gate comparing the host with itself prints the host instead.
    # The admin-merge guard's required contexts: the HOST's, not the layer's three.
    $'admin-merge-contexts\tmatch:Windows \\+ MSVC\tctl\t. {L}agents/scripts/core/safe-admin-merge.sh >/dev/null 2>&1; read_required_contexts'
    $'mirrored-paths\tmatch:^layer: .*/agent-layer$\t-\tbash scripts/dev/test-mirrored-paths.sh'
    # A regenerated host baseline names its regeneration command the way the host
    # spells it (row 16a): `agent-layer/agents/...` after the flip, or the command
    # in the header names a file that is not there. One Python generator (through
    # layer_paths.py) and the bash catalog; both files are restored afterwards.
    $'baseline-header\tmatch:^header-ok$\tctl\tr=0; { bash {L}agents/scripts/core/test-dead-export-audit.sh --baseline && bash {L}agents/scripts/project/test-lint-rules.sh --catalog --refresh; } >/dev/null 2>&1 && grep -qF "run \\`bash {L}agents/scripts/core/test-dead-export-audit.sh" docs/high-integrity/dead-export-baseline.md && grep -qF "run \\`bash {L}agents/scripts/project/test-lint-rules.sh --catalog" docs/high-integrity/baseline.md || r=1; git checkout -q -- docs/high-integrity/dead-export-baseline.md docs/high-integrity/baseline.md 2>/dev/null; [ "$r" -eq 0 ] && echo header-ok; exit "$r"'
)

# FIXTURES — fixture_<probe> <tree> <layer-prefix>, for a probe whose real host
# gives the same answer as the bare layer (nothing owed, nothing broken): plant
# host content that gives it something to report, so a script that read the layer
# instead would answer differently. Prints each path it created, relative to the
# tree; the probe stages them (some checks read the index) and removes them after
# the run. Planted for the today, local and ci runs, never for the control. Paths
# and the strings some gates scan for are assembled here, never written out whole,
# so this file cannot trip the doc-anchors, vexp or plan-reference gates itself.
FIX="flip-probe-fixture"
PLANS="docs/plans"

# fx_new <tree> <relpath> — refuse to plant over anything that exists.
fx_new() {
    if [ -e "$1/$2" ] || [ -L "$1/$2" ]; then
        printf 'fixture path exists: %s\n' "$2" >&2
        return 1
    fi
    mkdir -p "$(dirname "$1/$2")"
}
fixture_plan_archival_owed() {
    local f="$PLANS/active/$FIX.md"
    fx_new "$1" "$f" && printf '# Plan — %s\n\n> **Status**: shipped\n' "$FIX" > "$1/$f" && printf '%s\n' "$f"
}
fixture_work_item_owed() {
    local d="docs/work/items/99-$FIX"
    fx_new "$1" "$d" && mkdir "$1/$d" \
        && printf '# Specification — %s\n' "$FIX" > "$1/$d/1-specification.md" && printf '%s\n' "$d"
}
fixture_audit_doc_status_owed() {
    local a="FLIP_PROBE_FIXTURE_AUDIT.md" p="$PLANS/shipped/$FIX.md"
    fx_new "$1" "$a" && fx_new "$1" "$p" \
        && printf '# Audit — %s\nFinding 1: open.\n' "$FIX" > "$1/$a" \
        && printf '# Plan — %s\nRemediates %s findings.\n' "$FIX" "$a" > "$1/$p" && printf '%s\n' "$a" "$p"
}
fixture_doc_anchors() { # a reference to a section no rule doc has
    local f="docs/$FIX.md"
    fx_new "$1" "$f" && printf 'See AGENTS.md %s Flip probe fixture section.\n' '§' > "$1/$f" && printf '%s\n' "$f"
}
fixture_portable_agent_vexp() { # a project agent naming a vexp tool
    local f="agents/project/$FIX.md"
    fx_new "$1" "$f" && printf 'Call %s%s first.\n' 'mcp__vexp' '__run_pipeline' > "$1/$f" && printf '%s\n' "$f"
}
fixture_skill_vs_agent_parity() { # a layer skill whose agent twin is a host project agent
    local d="${2}agents/_shared/skills/$FIX" f="agents/project/$FIX.md"
    fx_new "$1" "$d" && fx_new "$1" "$f" && mkdir "$1/$d" \
        && printf '# %s\n' "$FIX" > "$1/$d/SKILL.md" && printf '# %s\n' "$FIX" > "$1/$f" && printf '%s\n' "$d" "$f"
}
fixture_plan_staleness() { # every cited PR merged ({STUB}'s gh says so), every post-ship section a stub
    local f="$PLANS/active/$FIX-stale.md" stub='*(populated post-ship'
    fx_new "$1" "$f" && printf '# Plan — %s\n\n## Implementation log\n%s — cites #1)*\n\n## Deviations from plan\n%s)*\n\n## Verification (actual)\n%s)*\n' \
        "$FIX" "$stub" "$stub" "$stub" > "$1/$f" && printf '%s\n' "$f"
}
fixture_plan_naming() {
    local f="$PLANS/active/Flip_Probe_Fixture.md"
    fx_new "$1" "$f" && printf '# Plan — %s\n' "$FIX" > "$1/$f" && printf '%s\n' "$f"
}
fixture_fleet_preflight() { # a fan-out with an unpinned agent() call, named from the host root
    local f="$FIX-workflow.js"
    fx_new "$1" "$f" && printf "await agent('%s');\n" "$FIX" > "$1/$f" && printf '%s\n' "$f"
}

REV="HEAD"
DIR=""
KEEP=0
ONLY=()
SUITE=0

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
            --suite)   SUITE=1; shift ;;
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

# suite_failures <test-all output> — its failing scripts, the mount prefix stripped
# so a post-flip path compares with today's.
suite_failures() {
    sed -n '/^Failed scripts:$/,$p' "$1" | sed -n 's/^  - \([^ ]*\) (.*/\1/p' \
        | sed 's|^agent-layer/||' | sort -u
}

# suite_exit2 <test-all output> — scripts --ci turned into a skip for exiting 2,
# the mount prefix stripped. Exit 2 is how a wrapper reports a tree it cannot
# reach (`cd … || exit 2`), and --ci never lists those as failures.
suite_exit2() {
    awk '/^#{50}$/ { if ((getline line) > 0 && line ~ /^# /) name = substr(line, 3); next }
         /^SKIPPED \(ci\): exit 2/ { print name }' "$1" | sed 's|^agent-layer/||' | sort -u
}

# suite_bad <test-all output> — every script that failed or exited 2.
suite_bad() {
    { suite_failures "$1"; suite_exit2 "$1"; } | sort -u
}

suite_count() { # suite_count <test-all output> — the Scripts: total, empty when absent
    sed -n 's/^AGGREGATE .*Scripts: \([0-9][0-9]*\).*/\1/p' "$1" | tail -n 1
}

# run_suite — test-all.sh in today/, host/ (local) and host-ci/ (ci), concurrently;
# then compare each post-flip run's failing scripts and count with today's.
run_suite() {
    local scrub=(-u PROJECT_ROOT -u AGENT_LAYER_ROOT -u PC_CONFIG_FILE -u SMATCHET_PROJECT_CONFIG
                 -u CLAUDE_PROJECT_DIR -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE)
    # Provision .claude/ first, as CI's agentic-selftests lane does: the hook
    # suites run the deployed hooks, and with none deployed they fail in every
    # tree alike, which the comparison would read as no regression.
    local tree pre
    for tree in today host host-ci; do
        pre=""; [ "$tree" = today ] || pre="agent-layer/"
        ( cd "$DIR/$tree" && env "${scrub[@]}" bash "${pre}agents/scripts/core/setup-harness.sh" claude-code ) \
            > "$DIR/suite-setup-$tree.txt" 2>&1 < /dev/null \
            || die 1 "setup-harness failed in $tree/ — see $DIR/suite-setup-$tree.txt"
    done
    ( cd "$DIR/today" && env "${scrub[@]}" bash scripts/dev/test-all.sh --ci ) > "$DIR/suite-today.txt" 2>&1 < /dev/null &
    local p_today=$!
    ( cd "$DIR/host" && env "${scrub[@]}" bash scripts/dev/test-all.sh --ci ) > "$DIR/suite-local.txt" 2>&1 < /dev/null &
    local p_local=$!
    ( cd "$DIR/host-ci" && env "${scrub[@]}" PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer \
        bash scripts/dev/test-all.sh --ci ) > "$DIR/suite-ci.txt" 2>&1 < /dev/null &
    local p_ci=$!
    wait "$p_today" "$p_local" "$p_ci"   # test-all exits 1 on any failure; the outputs are compared below

    local t_count mode count failed=0 pending=0 path
    t_count="$(suite_count "$DIR/suite-today.txt")"
    [ -n "$t_count" ] || die 1 "today's suite printed no AGGREGATE line — see $DIR/suite-today.txt"
    say "today: $t_count script(s), $(suite_bad "$DIR/suite-today.txt" | wc -l | tr -d ' ') failing or exiting 2"
    for mode in local ci; do
        count="$(suite_count "$DIR/suite-$mode.txt")"
        if [ -z "$count" ]; then
            say "FAIL  $mode: no AGGREGATE line — see $DIR/suite-$mode.txt"; failed=$((failed + 1)); continue
        fi
        if [ "$count" -lt "$t_count" ]; then
            say "FAIL  $mode: $count script(s) after the flip, $t_count today — a root was skipped"
            failed=$((failed + 1))
        fi
        while IFS= read -r path; do
            [ -n "$path" ] || continue
            if [ -e "$DIR/host/agent-layer/$path" ]; then
                say "FAIL  $mode: $path (layer content: it fails after the flip, not today)"
                failed=$((failed + 1))
            else
                say "PENDING  $mode: $path (host content: owed to the flip PR)"
                pending=$((pending + 1))
            fi
        done < <(comm -13 <(suite_bad "$DIR/suite-today.txt") <(suite_bad "$DIR/suite-$mode.txt"))
    done
    if [ "$failed" -ne 0 ]; then
        say "RED — $failed layer regression(s) or skipped root(s) after the flip; layout and outputs kept at $DIR"
        exit 1
    fi
    if [ "$KEEP" -eq 1 ]; then
        say "GREEN — no layer script regresses after the flip; $pending host script(s) owed to the flip PR; layout kept at $DIR"
    else
        rm -rf "$DIR"
        say "GREEN — no layer script regresses after the flip; $pending host script(s) owed to the flip PR"
    fi
    exit 0
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
    # On a branch named as host/'s is: a detached HEAD changes what some hooks do
    # (the session banner writes no baseline without a branch), so today/ must
    # differ from host/ only in the flip.
    { git clone -q --no-hardlinks "$src" "$DIR/today" && git -C "$DIR/today" checkout -q -B flip-probe "$sha"; } \
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
    { cp -a "$DIR/host" "$DIR/host-ci" && cp -a "$DIR/layer" "$DIR/layer-ctl"; } \
        || die 2 "cannot copy the post-flip host and the layer"
    mkdir -p "$DIR/stub-bin" || die 2 "cannot create $DIR/stub-bin"
    cat > "$DIR/stub-bin/gh" <<'GH' || die 2 "cannot write the stub gh"
#!/usr/bin/env bash
# flip-probe stand-in: authenticated, and every PR is merged.
case "${1:-} ${2:-}" in
    "auth status") exit 0 ;;
    "pr view")     echo MERGED; exit 0 ;;
esac
exit 1
GH
    chmod +x "$DIR/stub-bin/gh" || die 2 "cannot make the stub gh executable"
}

# plant_fixture <probe> <tree> <prefix> — plant the probe's fixture, if it has one,
# and stage it; sets FIX_PATHS to what was planted.
FIX_PATHS=""
plant_fixture() {
    local fn="fixture_${1//-/_}" p
    FIX_PATHS=""
    declare -F "$fn" >/dev/null || return 0
    FIX_PATHS="$("$fn" "$2" "$3")" || die 2 "probe $1: cannot plant its fixture in $2"
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        case "$p" in /*|*..*) die 2 "probe $1: fixture named an unsafe path: $p" ;; esac
        git -C "$2" add -f -- "$p" >/dev/null 2>&1 || true   # a path inside the submodule stays untracked
    done <<<"$FIX_PATHS"
}

# clear_fixture <tree> <paths> — unstage and remove what plant_fixture planted.
clear_fixture() {
    local p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        git -C "$1" rm -r -q -f --cached --ignore-unmatch -- "$p" >/dev/null 2>&1 || true
        rm -rf -- "${1:?}/$p"
    done <<<"$2"
}

# run_fixtured <probe> <tree> <prefix> <command> [env...] — run_probe with the
# probe's fixture planted for the run; sets RUN_OUT.
RUN_OUT=""
run_fixtured() {
    local name="$1" tree="$2" prefix="$3" cmd="$4" planted
    shift 4
    plant_fixture "$name" "$tree" "$prefix"
    planted="$FIX_PATHS"
    RUN_OUT="$(run_probe "$tree" "$prefix" "$cmd" "$@")"
    clear_fixture "$tree" "$planted"
}

# run_probe <tree> <layer-prefix> <command> [env assignments...] — prints the exit
# code on the first line and the output after it; split_rc / split_out take it apart.
run_probe() {
    local tree="$1" prefix="$2" cmd="$3"; shift 3
    local out rc
    cmd="${cmd//\{L\}/$prefix}"
    cmd="${cmd//\{STUB\}/$DIR/stub-bin}"
    out="$(cd "$tree" && env -u PROJECT_ROOT -u AGENT_LAYER_ROOT -u PC_CONFIG_FILE \
              -u SMATCHET_PROJECT_CONFIG -u CLAUDE_PROJECT_DIR -u GIT_DIR -u GIT_WORK_TREE \
              -u GIT_INDEX_FILE "$@" bash -c "$cmd" 2>&1 < /dev/null)"
    rc=$?
    printf '%s\n%s\n' "$rc" "$out"
}

split_rc()  { printf '%s\n' "${1%%$'\n'*}"; }
split_out() { case "$1" in *$'\n'*) printf '%s\n' "${1#*$'\n'}" ;; esac; }

last_line() { # last line with a word in it, with the probe trees' paths normalised
    grep -E '[[:alnum:]]' | tail -n 1 | sed -E "s#$DIR/(today|host-ci|host|layer-ctl)#<root>#g"
}

# outcome <expectation> <rc> <output> — what a control run must differ in: the
# exit code, the last line and, for an ERE expectation, whether it matched.
outcome() {
    local expect="$1" rc="$2" out="$3" line re hit=""
    line="$(printf '%s\n' "$out" | last_line)"
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
    # An inherited GIT_DIR (a hook, a linked worktree) would aim every `git -C` below
    # at the SOURCE repository — git honours GIT_DIR over -C — so a checkout or an
    # update-ref meant for a scratch clone would move the source's HEAD and refs.
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
    parse_args "$@"
    local tool
    for tool in git bash tar; do command -v "$tool" >/dev/null 2>&1 || die 2 "$tool not on PATH"; done
    command -v jq >/dev/null 2>&1 || die 2 "jq not on PATH (the hook probes read .claude/settings.json with it)"
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
    say "layout: $DIR  (today/, layer/ + layer-ctl/, host/ + host-ci/ with agent-layer/ mounted — commit $sha)"
    [ "$SUITE" -eq 0 ] || run_suite

    local entry name expect ctl cmd ran=0 failed=0 pending=0 t l c x t_rc l_rc c_rc x_rc mode verdict re owed
    printf '%-28s %-6s %-6s %-6s %-6s %s\n' PROBE TODAY LOCAL CI CTL RESULT
    for entry in "${PROBES[@]}"; do
        name="$(probe_field "$entry" 1)"; expect="$(probe_field "$entry" 2)"
        ctl="$(probe_field "$entry" 3)"; cmd="$(probe_field "$entry" 4)"
        case "$ctl" in ctl|-) ;; *) die 2 "probe $name: unknown control '$ctl'" ;; esac
        selected "$name" || continue
        run_fixtured "$name" "$DIR/today" "" "$cmd"; t="$RUN_OUT"
        run_fixtured "$name" "$DIR/host" "agent-layer/" "$cmd"; l="$RUN_OUT"
        run_fixtured "$name" "$DIR/host-ci" "agent-layer/" "$cmd" PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer; c="$RUN_OUT"
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
            x="$(run_probe "$DIR/layer-ctl" "" "$cmd" PROJECT_ROOT=. AGENT_LAYER_ROOT=.)"
            x_rc="$(split_rc "$x")"
            if [ "$verdict" = ok ] \
               && [ "$(outcome "$expect" "$x_rc" "$(split_out "$x")")" = "$(outcome "$expect" "$c_rc" "$(split_out "$c")")" ]; then
                verdict="BLIND (the standalone layer gives the same answer as the host)"
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
