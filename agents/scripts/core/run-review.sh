#!/usr/bin/env bash
# run-review.sh — review-panel launcher: fan a gate review across the roster,
# one leg per (harness, model).
#
# Port of Whip-Process Tools/run-review.ps1. Differences from the original,
# each deliberate (docs/plans/absorb-whip-process.md Phase 2):
#   - Headless by default: each leg runs its harness CLI non-interactively in
#     the background and completion is the process exiting — the original's
#     interactive panes + status-bus dashboard are replaced by the opt-in
#     --panes WezTerm grid (no status bus: a headless leg's liveness is its
#     PID). Leg SUCCESS is still judged only by the review file landing.
#   - Gates: pre and post only. `close-` is reserved-not-active (the close here
#     has no panel review), `audit` maps to the historical-code-review skill,
#     and `generic` was not ported — so --brief (generic-only in the original)
#     is recognized and rejected.
#   - The roster lives in project.config.json under "review-panel" (the
#     original's Tools/review-roster.psd1); opencode tiers are flattened into
#     the models list — edit the config to change tiers.
#   - Roster degradation: a harness whose CLI is not on PATH is SKIPPED and
#     reported, not fatal — a panel minus one leg is still a panel.
#
# The roster is the single source of truth for which models run and how each
# harness selects one. The launcher owns identity: it takes the round from the
# required --round argument, derives each leg's output filename, and passes
# them in, so no skill carries a roster and nothing self-labels
# (docs/agent-rules/review-panels.md § Reviewer identity).
#
# USAGE
#   run-review.sh --gate pre|post --subject <NN|slug> --round N
#                 [--legs h1,h2] [--timeout-minutes M] [--panes]
#                 [--dry-run] [--smoke-test]
#
#   --gate            pre (4- prefix) or post (5- prefix)
#   --subject         work-item NN or slug under docs/work/items/
#   --round           REQUIRED, 1-999. One number for the whole invocation,
#                     never inferred from folder state (orphans/trips can't
#                     drift it). Same --round refills a tripped leg in place;
#                     bump it for a fresh pass.
#   --legs            harness filter (comma-separated); default = every
#                     harness in the roster. Refill a tripped leg with
#                     --legs <harness> at the same --round.
#   --panes           opt-in: launch legs as an interactive WezTerm grid
#                     (requires wezterm-gui) instead of headless background
#                     processes.
#   --dry-run         print the legs + commands, run nothing.
#   --smoke-test      replace every leg command with an echo + sleep, to
#                     exercise launch/watch/guard plumbing without harnesses.
#
# EXIT
#   0 — every launched leg landed its file; write guard verified clean
#   1 — a leg failed to land, a stray path was written, or the guard could
#       not verify (UNVERIFIED fails closed)
#   2 — usage / environment / roster error

set -uo pipefail

die()  { echo "run-review: $*" >&2; exit 2; }
note() { echo "$*"; }
# `shift 2` with one arg left shifts NOTHING (rc 1, set -e off), so a
# value-taking flag with no value would spin the parse loop forever.
need_val() { [ $# -ge 2 ] || die "$1 requires a value"; }

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || die "not inside a git work tree"
cd "$ROOT" || die "cannot cd to repo root"

# shellcheck source=lib/review-guard.sh
source "$ROOT/agents/scripts/core/lib/review-guard.sh"

# Same interpreter dance as scripts/dev/test-docs.sh: on Windows `python3` is
# the Store alias stub, so resolve by actually executing a candidate.
PY=""
for _c in python3 python py; do
    if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then PY="$_c"; break; fi
done
[ -n "$PY" ] || die "no working python on PATH (roster parsing needs it)"

# --- args -------------------------------------------------------------------
GATE="" SUBJECT="" ROUND="" LEGS="" TIMEOUT_MINUTES=45
DRY_RUN=0 SMOKE_TEST=0 PANES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --gate)              need_val "$@"; GATE="$2"; shift 2 ;;
        --gate=*)            GATE="${1#*=}"; shift ;;
        --subject)           need_val "$@"; SUBJECT="$2"; shift 2 ;;
        --subject=*)         SUBJECT="${1#*=}"; shift ;;
        --round)             need_val "$@"; ROUND="$2"; shift 2 ;;
        --round=*)           ROUND="${1#*=}"; shift ;;
        --legs)              need_val "$@"; LEGS="$2"; shift 2 ;;
        --legs=*)            LEGS="${1#*=}"; shift ;;
        --timeout-minutes)   need_val "$@"; TIMEOUT_MINUTES="$2"; shift 2 ;;
        --timeout-minutes=*) TIMEOUT_MINUTES="${1#*=}"; shift ;;
        --panes)           PANES=1; shift ;;
        --dry-run)         DRY_RUN=1; shift ;;
        --smoke-test)      SMOKE_TEST=1; shift ;;
        # generic-only in the original; the generic gate was not ported.
        --brief)           die "--brief applies only to the 'generic' gate, which is not ported" ;;
        -h|--help)         sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                 die "unknown argument: $1 (see --help)" ;;
    esac
done

case "$GATE" in
    pre)  SKILL="pre-implementation-review"; PREFIX="4-" ;;
    post) SKILL="adversarial-code-review";   PREFIX="5-" ;;
    "")   die "--gate is required (pre|post)" ;;
    *)    die "unknown gate '$GATE' (ported gates: pre, post)" ;;
esac
[ -n "$SUBJECT" ] || die "--subject is required for the '$GATE' gate"
[[ "$ROUND" =~ ^[0-9]+$ ]] && [ "$ROUND" -ge 1 ] && [ "$ROUND" -le 999 ] \
    || die "--round is required (1-999): naming is always explicit, never inferred from folder state"
# Bash arithmetic resolves a non-number to 0, which would silently make the
# deadline instant. 0 stays legal — it is the explicit instant-deadline case.
[[ "$TIMEOUT_MINUTES" =~ ^[0-9]+$ ]] \
    || die "--timeout-minutes must be a non-negative integer (got '$TIMEOUT_MINUTES')"

# --- resolve subject + output folder ---------------------------------------
ITEMS_DIR="docs/work/items"
FOLDER=""
match_count=0
for d in "$ITEMS_DIR"/*/; do
    [ -d "$d" ] || continue
    b="$(basename "$d")"
    if [ "$b" = "$SUBJECT" ] || [[ "$b" =~ ^0*${SUBJECT}(-|$) ]]; then
        FOLDER="$b"; match_count=$((match_count + 1))
    fi
done
[ "$match_count" -eq 1 ] \
    || die "subject '$SUBJECT' did not resolve to exactly one $ITEMS_DIR/NN-slug folder (matched $match_count)"
OUT_REL="$ITEMS_DIR/$FOLDER"
OUT_DIR="$ROOT/$OUT_REL"
SUBJ_ARG="${FOLDER%%-*}"

# --- roster -----------------------------------------------------------------
# TSV per model: harness <TAB> flag <TAB> dwell <TAB> tag <TAB> id
ROSTER_TSV="$("$PY" - "$ROOT/project.config.json" <<'PYEOF'
import json, sys
cfg = json.load(open(sys.argv[1], encoding="utf-8"))
rp = cfg.get("review-panel")
if not rp:
    sys.exit("no 'review-panel' block in project.config.json (the roster; see docs/agent-rules/review-panels.md)")
for h in sorted(rp["harnesses"]):
    e = rp["harnesses"][h]
    for m in e["models"]:
        print("\t".join([h, e["flag"], str(e.get("start_dwell_secs", 3)), m["tag"], m["id"]]))
PYEOF
)" || die "roster parse failed"
# A Windows python emits CRLF line ends; a stray \r would ride the last field
# (the model id) into the leg command line.
ROSTER_TSV="${ROSTER_TSV//$'\r'/}"

mapfile -t ROSTER_HARNESSES < <(printf '%s\n' "$ROSTER_TSV" | cut -f1 | sort -u)

if [ -n "$LEGS" ]; then
    IFS=',' read -ra want <<< "$LEGS"
    declare -A WANT=()
    for w in "${want[@]}"; do
        w="$(echo "$w" | tr -d '[:space:]')"
        [ -n "$w" ] || continue
        ok=0
        for h in "${ROSTER_HARNESSES[@]}"; do [ "$h" = "$w" ] && ok=1; done
        [ "$ok" -eq 1 ] || die "unknown harness '$w'. Roster has: ${ROSTER_HARNESSES[*]}"
        WANT["$w"]=1
    done
else
    declare -A WANT=()
    for h in "${ROSTER_HARNESSES[@]}"; do WANT["$h"]=1; done
fi

# --- build the leg list: one per (harness, model) --------------------------
# A harness whose CLI is missing degrades the panel, not the run: skip + report.
harness_cli() {
    case "$1" in
        cursor) echo "cursor-agent" ;;
        *)      echo "$1" ;;
    esac
}

LEG_NAME=() LEG_HARNESS=() LEG_TAG=() LEG_ID=() LEG_FLAG=() LEG_OUT=() LEG_DWELL=()
SKIPPED=()
while IFS=$'\t' read -r h flag dwell tag id; do
    [ -n "$h" ] || continue
    [ -n "${WANT[$h]+x}" ] || continue
    if ! command -v "$(harness_cli "$h")" >/dev/null 2>&1; then
        SKIPPED+=("$h-$tag ('$(harness_cli "$h")' not on PATH)")
        continue
    fi
    LEG_NAME+=("$h-$tag")
    LEG_HARNESS+=("$h")
    LEG_TAG+=("$tag")
    LEG_ID+=("$id")
    LEG_FLAG+=("$flag")
    LEG_OUT+=("$OUT_REL/${PREFIX}review-$ROUND-$h-$tag.md")
    LEG_DWELL+=("$dwell")
done <<< "$ROSTER_TSV"

N_LEGS=${#LEG_NAME[@]}
if [ "$N_LEGS" -eq 0 ]; then
    for s in "${SKIPPED[@]}"; do echo "run-review: leg SKIPPED - $s" >&2; done
    die "No legs selected."
fi

# --- per-leg command --------------------------------------------------------
# The launcher passes the model and the exact output file; the skill only has
# to review and write there. Every prompt is emitted as a SINGLE-quoted shell
# string — the only metacharacter left is the single quote itself, which the
# quoting helper escapes. Do not switch these to double-quoted strings to
# interpolate something — build the value first and quote it here.
quote_sh() {
    local q="'\\''"
    printf "'%s'" "${1//\'/$q}"
}

get_leg_command() {
    local harness="$1" flag="$2" id="$3" outfile="$4"
    # flag and id come from the roster config; the schema barely constrains
    # them, and they land in an executed script — quote them like the prompt.
    local sel
    sel="$(quote_sh "$flag") $(quote_sh "$id")"
    local prompt
    prompt="$(quote_sh "/$SKILL $SUBJ_ARG out=$outfile")"
    case "$harness" in
        claude)   echo "claude -p --dangerously-skip-permissions $sel $prompt" ;;
        # codex is the one harness that shipped without an unattended flag: it
        # asks per command, so a leg stalls waiting for a human. -a never
        # removes the approval gate; -s workspace-write is the narrowest
        # sandbox that still lets the reviewer write its own review file
        # (-s read-only would block the leg's only output). The write guard at
        # the end of this script, not the sandbox, is what holds the read-only
        # pass to its one file.
        codex)    echo "codex exec -a never -s workspace-write $sel $prompt" ;;
        # opencode's native modules crash if TEMP is on a RAM disk; pin it to
        # the machine's local AppData\Temp (stable, non-RAM, per-account) when
        # on Windows — a no-op elsewhere.
        opencode)
            if [ -n "${LOCALAPPDATA:-}" ]; then
                local tmpdir
                tmpdir="$(quote_sh "$LOCALAPPDATA/Temp")"
                echo "TEMP=$tmpdir TMP=$tmpdir opencode run $sel $prompt"
            else
                echo "opencode run $sel $prompt"
            fi ;;
        # cursor has no slash command; point it at the shared skill in prose.
        cursor)
            local ask
            ask="$(quote_sh "Run the $GATE review of '$SUBJ_ARG' following agents/_shared/skills/$SKILL/SKILL.md and docs/agent-rules/review-panels.md. Write your findings to $outfile.")"
            echo "cursor-agent --force -p $sel $ask" ;;
        *) die "No command template for harness '$harness'." ;;
    esac
}

LEG_CMD=()
for i in $(seq 0 $((N_LEGS - 1))); do
    if [ "$SMOKE_TEST" -eq 1 ]; then
        LEG_CMD+=("echo 'SMOKE-TEST [${LEG_NAME[$i]}] - no real work; sleeping 25s'; sleep 25")
    else
        # `die` inside the substitution exits only the subshell; check the rc
        # so a roster error stops the launch instead of running an empty leg.
        _cmd="$(get_leg_command "${LEG_HARNESS[$i]}" "${LEG_FLAG[$i]}" "${LEG_ID[$i]}" "${LEG_OUT[$i]}")" || exit 2
        LEG_CMD+=("$_cmd")
    fi
done

# --- dry run ----------------------------------------------------------------
if [ "$DRY_RUN" -eq 1 ]; then
    note "DRY RUN  gate=$GATE  subject=$FOLDER  round=$ROUND  legs=$N_LEGS"
    note "watch:   $OUT_DIR"
    for s in "${SKIPPED[@]}"; do note "leg SKIPPED - $s"; done
    for i in $(seq 0 $((N_LEGS - 1))); do
        note ""
        note "--- ${LEG_NAME[$i]}  ->  ${LEG_OUT[$i]}"
        note "${LEG_CMD[$i]}"
    done
    exit 0
fi

# --- run scratch dir + per-leg scripts --------------------------------------
RUN_DIR="$(mktemp -d)"

# Instances of the same harness contend on that harness's shared global config
# at startup: cursor rewrites its cli-config write-temp-then-rename, so a
# second instance starting inside that window dies with EPERM. The original
# serialized concurrent panes on a harness-scoped lock; here a single launcher
# generates every leg, so the same spacing is PRECOMPUTED — the k-th leg of a
# harness starts k*dwell seconds late. The retry below is what makes a lost
# race recover instead of silently dropping the leg from the panel.
declare -A HARNESS_SEEN=()
LEG_OFFSET=()
for i in $(seq 0 $((N_LEGS - 1))); do
    h="${LEG_HARNESS[$i]}"
    k="${HARNESS_SEEN[$h]:-0}"
    LEG_OFFSET+=($((k * LEG_DWELL[i])))
    HARNESS_SEEN["$h"]=$((k + 1))
done

LEG_SCRIPT=()
for i in $(seq 0 $((N_LEGS - 1))); do
    sp="$RUN_DIR/${LEG_NAME[$i]}.sh"
    {
        echo "#!/usr/bin/env bash"
        echo "cd $(quote_sh "$ROOT") || exit 2"
        echo "sleep ${LEG_OFFSET[$i]}"
        # A start that loses the config race dies in seconds with an error; a
        # real headless review runs for minutes. So an early FAILURE means a
        # lost race, not a finished review — retry rather than leave the panel
        # a leg short on a transient. (Unlike the interactive original, an
        # early rc-0 exit here is a legitimately finished leg — only failures
        # retry.)
        echo "rc=0"
        echo "for attempt in 1 2 3; do"
        echo "  t0=\$SECONDS"
        echo "  ${LEG_CMD[$i]}"
        echo "  rc=\$?"
        echo "  elapsed=\$((SECONDS - t0))"
        echo "  if [ \"\$rc\" -eq 0 ] || [ \"\$elapsed\" -ge 30 ]; then break; fi"
        echo "  echo \"[${LEG_NAME[$i]}] exited rc=\$rc after \${elapsed}s - contended start? retry \$attempt/3\""
        echo "  sleep ${LEG_DWELL[$i]}"
        echo "done"
        if [ "$SMOKE_TEST" -eq 1 ]; then
            echo "touch $(quote_sh "$RUN_DIR/${LEG_NAME[$i]}.done")"
        fi
        echo "exit \"\$rc\""
    } > "$sp"
    chmod +x "$sp"
    LEG_SCRIPT+=("$sp")
done

# --- write guard: baseline --------------------------------------------------
# Snapshot what is already dirty before launch — the tree is routinely dirty
# mid-item, so only the delta is attributable to a leg. Helpers + contract:
# lib/review-guard.sh.
GUARD_UNVERIFIED=0
if ! get_dirty_state "$ROOT" > "$RUN_DIR/guard.before"; then
    GUARD_UNVERIFIED=1
fi

# --- launch -----------------------------------------------------------------
LAUNCH_EPOCH="$(date +%s)"
LEG_PID=()
if [ "$PANES" -eq 1 ]; then
    # Opt-in interactive grid. wezterm cli cannot reach the gui mux socket on
    # Windows, so wezterm-gui builds the layout itself on startup via a
    # generated gui-startup Lua config (the original's mechanism, ported).
    WEZTERM_GUI="$(command -v wezterm-gui || true)"
    [ -n "$WEZTERM_GUI" ] || die "--panes needs wezterm-gui on PATH"
    LUA="$RUN_DIR/layout.wezterm.lua"
    {
        echo "local wezterm = require 'wezterm'"
        echo "local mux = wezterm.mux"
        echo "local ok, cfg = pcall(dofile, wezterm.home_dir .. '/.wezterm.lua')"
        echo "if not ok or type(cfg) ~= 'table' then cfg = wezterm.config_builder() end"
        echo "local progs = {"
        for i in $(seq 0 $((N_LEGS - 1))); do
            echo "  { 'bash', '-c', [[bash '${LEG_SCRIPT[$i]}'; exec bash]] },"
        done
        echo "}"
        # Grid: split into `cols` equal columns, then each column into its rows.
        cat <<'LUAEOF'
wezterm.on('gui-startup', function(cmd)
  local n = #progs
  local cols = math.ceil(math.sqrt(n))
  local per, extra = math.floor(n / cols), n % cols
  local count = {}
  for c = 1, cols do count[c] = per + (c <= extra and 1 or 0) end
  local idx = 1
  local tab, first, window = mux.spawn_window { args = progs[idx] }
  idx = idx + 1
  local heads = { first }
  for c = 2, cols do
    heads[c] = heads[c-1]:split {
      direction = 'Right', size = (cols - c + 1) / (cols - c + 2),
      args = progs[idx] }
    idx = idx + 1
  end
  for c = 1, cols do
    local need = count[c] - 1
    local cur = heads[c]
    for r = 1, need do
      cur = cur:split {
        direction = 'Bottom', size = (need - r + 1) / (need - r + 2),
        args = progs[idx] }
      idx = idx + 1
    end
  end
end)
return cfg
LUAEOF
    } > "$LUA"
    "$WEZTERM_GUI" --config-file "$LUA" start &
else
    for i in $(seq 0 $((N_LEGS - 1))); do
        bash "${LEG_SCRIPT[$i]}" > "$RUN_DIR/${LEG_NAME[$i]}.log" 2>&1 &
        LEG_PID+=("$!")
    done
fi

note "Panel launched: $N_LEGS legs (gate=$GATE subject=$FOLDER round=$ROUND)"
for s in "${SKIPPED[@]}"; do note "  leg SKIPPED - $s"; done
note "  $OUT_DIR"
note "  leg logs: $RUN_DIR"

# --- watch ------------------------------------------------------------------
# A leg counts as landed only if its file is written AFTER launch. Seeding
# "seen" with the files already present (a refill of the same round, or a
# re-run) would otherwise mark every leg landed on the first tick and stop the
# watcher before any leg had actually run.
declare -A SEEN=()
for f in "$OUT_DIR/${PREFIX}review-"*.md; do
    [ -f "$f" ] && SEEN["$(basename "$f")"]=1
done

leg_landed() {
    local p="$ROOT/$1" m
    [ -f "$p" ] || return 1
    # GNU stat takes -c %Y; BSD/macOS stat takes -f %m. Probe both so a
    # macOS run does not report every leg unlanded.
    m="$(stat -c %Y "$p" 2>/dev/null)" || m="$(stat -f %m "$p" 2>/dev/null)" || return 1
    [ "$m" -ge "$LAUNCH_EPOCH" ]
}

# Is any leg still working? Headless legs are plain child processes, so the
# original's status bus is replaced by PID liveness — which, unlike the bus,
# CAN say a leg finished. Interactive --panes sessions never exit, so there
# the quiet timeout is the only stop signal (as in the original without bus).
panel_busy() {
    [ "$PANES" -eq 1 ] && return 1
    local pid
    for pid in "${LEG_PID[@]}"; do
        kill -0 "$pid" 2>/dev/null && return 0
    done
    return 1
}

DEADLINE=$((LAUNCH_EPOCH + TIMEOUT_MINUTES * 60))
QUIET_SECS=180
LAST_NEW="$(date +%s)"
declare -A LANDED=()

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    sleep 3
    for f in "$OUT_DIR/${PREFIX}review-"*.md; do
        [ -f "$f" ] || continue
        b="$(basename "$f")"
        if [ -z "${SEEN[$b]+x}" ]; then
            SEEN["$b"]=1
            LAST_NEW="$(date +%s)"
            note "  + $b"
        fi
    done
    if [ "$SMOKE_TEST" -eq 1 ]; then
        done_count=0
        for i in $(seq 0 $((N_LEGS - 1))); do
            [ -f "$RUN_DIR/${LEG_NAME[$i]}.done" ] && done_count=$((done_count + 1))
        done
        [ "$done_count" -eq "$N_LEGS" ] && break
        continue
    fi
    # A leg's file landing is the success signal; a process exiting only says
    # the turn ended.
    for i in $(seq 0 $((N_LEGS - 1))); do
        if [ -z "${LANDED[${LEG_NAME[$i]}]+x}" ] && leg_landed "${LEG_OUT[$i]}"; then
            LANDED["${LEG_NAME[$i]}"]=1
        fi
    done
    [ "${#LANDED[@]}" -eq "$N_LEGS" ] && break
    # Quiet only counts as stalled if nothing is still working. Without the
    # liveness check a fast first leg plus a slow field ends the watch
    # mid-round, and the summary then reads like a result rather than what it
    # is (legs still going). No new-filename precondition: a refill of the
    # same round re-writes an EXISTING name, so SEEN never grows and gating
    # on it would make a dead refill wait out the full timeout.
    if [ $(( $(date +%s) - LAST_NEW )) -ge "$QUIET_SECS" ] \
        && ! panel_busy; then
        break
    fi
done

# --- summary ----------------------------------------------------------------
note ""
MISSING=()
for i in $(seq 0 $((N_LEGS - 1))); do
    [ -z "${LANDED[${LEG_NAME[$i]}]+x}" ] && MISSING+=("${LEG_NAME[$i]}")
done
# Count LANDED legs, not newly-seen filenames: a refill of the same round
# re-writes an EXISTING file, which adds no new name yet is a landing.
LANDED_COUNT="${#LANDED[@]}"
if [ "$SMOKE_TEST" -eq 1 ]; then MISSING=(); LANDED_COUNT="$N_LEGS"; fi
note "Watcher stopped. $LANDED_COUNT/$N_LEGS legs wrote a file."
[ "${#MISSING[@]}" -gt 0 ] && note "No file from: ${MISSING[*]}"
# The watcher stopping is not the panel stopping. Say so loudly when legs are
# still working, so a partial count is never mistaken for the round's outcome.
if [ "${#MISSING[@]}" -gt 0 ] && panel_busy; then
    note ""
    note "INCOMPLETE - at least one leg is STILL RUNNING."
    note "This count is not the round's result. Re-check the output folder before judging it."
fi

# --- write guard: report ----------------------------------------------------
# Any path whose state changed during the round and is not an expected review
# file was written by something other than a leg doing its job. This FAILS the
# round (exit 1): the findings may still be good, but the strays must be
# reviewed and reverted before the diff is judged.
: > "$RUN_DIR/guard.expected"
for i in $(seq 0 $((N_LEGS - 1))); do
    echo "${LEG_OUT[$i]}" >> "$RUN_DIR/guard.expected"
done
if ! get_dirty_state "$ROOT" > "$RUN_DIR/guard.after"; then
    GUARD_UNVERIFIED=1
fi

FAIL=0
[ "${#MISSING[@]}" -gt 0 ] && FAIL=1

# rc 1 from either scan means it could not run (git failed, or no work tree).
# Say UNVERIFIED rather than printing the clean line: a safety check that goes
# quiet when it breaks trains the reader to believe silence.
if [ "$GUARD_UNVERIFIED" -eq 1 ]; then
    note ""
    note "WRITE GUARD: UNVERIFIED - could not read the repo state for: $ROOT"
    note "No claim is made about what the legs wrote; check by hand."
    FAIL=1
else
    mapfile -t STRAYS < <(get_stray_paths "$RUN_DIR/guard.before" "$RUN_DIR/guard.after" "$RUN_DIR/guard.expected")
    if [ "${#STRAYS[@]}" -gt 0 ]; then
        note ""
        note "WRITE GUARD: ${#STRAYS[@]} path(s) changed outside the round's review files."
        for p in "${STRAYS[@]}"; do note "  $p"; done
        # The guard cannot tell a leg from a human editing the repo while the
        # round runs — it reports the delta, not its author. Read the list
        # before concluding a reviewer overstepped.
        note "Review these before judging the round (a concurrent edit of your own lands here too)."
        FAIL=1
    else
        # "Clean" is a claim about a FINISHED round. The watch loop can exit
        # with legs still live, and a leg that writes after this scan is
        # invisible to it — so when any leg is unlanded, the guard reports
        # what it actually knows: clean SO FAR.
        if [ "${#MISSING[@]}" -gt 0 ]; then
            note "Write guard: clean SO FAR - the round is INCOMPLETE (${#MISSING[@]} leg(s) unlanded); a later write would not be seen."
        else
            note "Write guard: clean (nothing changed outside the round's review files)."
        fi
    fi
fi

exit "$FAIL"
