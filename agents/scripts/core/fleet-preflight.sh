#!/usr/bin/env bash
# fleet-preflight.sh — mechanical pre-launch validation of a Workflow fleet
# against docs/agent-rules/workflow-fleets.md § Pre-launch checklist. Every check
# below maps 1:1 to a now-written rule whose violation killed a real fleet during
# the 2026-06-10/11 audit-fleet debacle (failure inventory in that doc).
#
# Usage:
#   fleet-preflight.sh <workflow-script> [fleet-dir] [--strict]
#   fleet-preflight.sh --selftest
#
#   <workflow-script>  the JS passed to the Workflow tool (the agent() fan-out).
#   [fleet-dir]        optional staged input/output dir (build/<slug>/); when
#                      given, every file under it is size-checked.
#   --strict           exit non-zero if any WARN fired (gate mode). Default:
#                      advisory (exit 0 regardless). Per workflow-fleets.md
#                      § Pre-launch checklist this is now a MANDATORY pre-launch
#                      MUST for any fan-out above the documented agent-count
#                      threshold — a hard violation (e.g. an unpinned fan-out
#                      agent() — failure 2) makes --strict exit 1 and blocks the
#                      launch.
#   --selftest         run inline fixtures; assert a violating script exits
#                      non-zero under --strict and a clean script exits 0.
#
# Checks (each cites the rule-doc section it enforces):
#   1. model-pin       (§ Model pinning)        every agent() call carries model:
#   2. input-size      (§ Input-size pre-flight) every staged file ≤ 1/3 window
#   3. out-of-workspace(§ In-workspace staging)  no ~/.claude / %TEMP% / abs paths
#   4. checkpoint-step (§ Checkpoint contract)   prompts write to a repo file
#   5. concurrency     (§ Concurrency)           reminder when siblings are live
#   6. fanout-width    (§ Two fan-out mechanisms) parallel/pipeline width ≤ slots
#   7. read-discipline (§ Per-agent scoping)      broad-scope prompt needs a windowed-read directive
#   8. path-hygiene    (§ Semantic-search exceptions) no bare-basename source file in a prompt
#
# Advisory by default (WARN lines, exit 0) so it can run on every launch without
# blocking; --strict promotes it to a gate once the WARN rate is calibrated.
#
# Test seams (production leaves all unset → identical behaviour):
#   PREFLIGHT_WINDOW_TOKENS    context-window token budget (default 200000); the
#                              1/3 byte cap = WINDOW_TOKENS/3*4 (~4 bytes/token).
#   PREFLIGHT_ACTIVE_SESSIONS  live-session registry dir (default .claude/.active-sessions).
#   PREFLIGHT_CONCURRENCY_CAP  the ≤N reminder threshold (default 5).
#   PREFLIGHT_SLOTS            in-process Workflow slot count for check 6
#                              (default min(16, nproc-2)).

set -euo pipefail
cd "$(dirname "$0")/../../.."

usage() { echo "usage: fleet-preflight.sh <workflow-script> [fleet-dir] [--strict] | --selftest" >&2; }

# --- selftest ----------------------------------------------------------------
# Proves the gate's contract from inline fixtures: the canonical hard violation
# (a multi-agent fan-out with no model: pin — failure 2) must exit non-zero
# under --strict, and a clean fan-out must exit 0. Synths the fixtures in a
# temp dir and re-invokes this script via its repo-relative path (post-`cd` cwd
# is repo-root), matching the sibling convention (plan-archival-owed.sh).
# selftest: asserts-failure — the violating fixture MUST exit non-zero under
# --strict; if it does not, the gate is inert and the selftest FAILs.
run_selftest() {
    local fail=0 tmp out rc
    tmp="$(mktemp -d)" || { echo "fleet-preflight selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN
    self="agents/scripts/core/fleet-preflight.sh"

    # Pin the check-6 slot count so the selftest isolates the model-pin behavior
    # it targets. Without this, check 6 falls back to min(16, nproc-2); on a host
    # with <=3 cores the clean fixture's 2-wide pipeline tops the slot count, adds
    # a fan-out-width WARN, and --strict exits non-zero — failing assert 3 on a
    # small CI runner even though model pinning is correct (Cursor Bugbot, #1354).
    export PREFLIGHT_SLOTS=16

    # Violating fixture: a 3-wide parallel() fan-out, every agent() unpinned.
    cat > "$tmp/violating.js" <<'EOF'
await parallel([
  agent({ prompt: 'read Source/Foo.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ prompt: 'read Source/Bar.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ prompt: 'read Source/Baz.cpp:10 offset 0 limit 50; write build/x/r.md' }),
]);
EOF
    # Clean fixture: same fan-out, every agent() carries a model: pin.
    cat > "$tmp/clean.js" <<'EOF'
await pipeline([
  agent({ model: 'sonnet', prompt: 'read Source/Foo.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ model: 'sonnet', prompt: 'read Source/Bar.cpp:10 offset 0 limit 50; write build/x/r.md' }),
]);
EOF

    # Assert 1: violating + --strict → non-zero (the gate's whole point).
    out="$(bash "$self" "$tmp/violating.js" --strict 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: violating fan-out passed --strict (rc=0) — gate is inert"; fail=1
    fi
    # Assert 2: violating WITHOUT --strict → exit 0 (advisory default holds).
    out="$(bash "$self" "$tmp/violating.js" 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: violating fan-out without --strict exited ${rc}, expected 0 (advisory)"; fail=1
    fi
    # Assert 3: clean + --strict → exit 0 (no false positive blocks a good fleet).
    out="$(bash "$self" "$tmp/clean.js" --strict 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: clean fan-out failed --strict (rc=${rc}) — false positive"
        echo "$out"; fail=1
    fi

    if [ "$fail" = "0" ]; then echo "fleet-preflight --selftest: PASS (3/3)"; return 0; fi
    echo "fleet-preflight --selftest: FAIL"; return 1
}

STRICT=0
posargs=()
for a in "$@"; do
    case "$a" in
        --selftest) run_selftest; exit $? ;;
        --strict)  STRICT=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         posargs+=("$a") ;;
    esac
done

script="${posargs[0]:-}"
fleet_dir="${posargs[1]:-}"

if [ -z "$script" ]; then usage; exit 2; fi
if [ ! -f "$script" ]; then
    echo "fleet-preflight: workflow script not found: $script" >&2
    exit 2
fi

WINDOW_TOKENS="${PREFLIGHT_WINDOW_TOKENS:-200000}"
ACTIVE_SESSIONS="${PREFLIGHT_ACTIVE_SESSIONS:-.claude/.active-sessions}"
CONCURRENCY_CAP="${PREFLIGHT_CONCURRENCY_CAP:-5}"
# tokens→bytes at ~4 B/token, capped at ⅓ window. /3 first is deliberate: a
# slightly-lower (more conservative) byte ceiling, and never overflows.
# shellcheck disable=SC2017
CAP_BYTES=$(( WINDOW_TOKENS / 3 * 4 ))

warn_count=0
warn() { warn_count=$((warn_count + 1)); echo "WARN  $*"; }
note() { echo "note  $*"; }

echo "fleet-preflight: ${script}${fleet_dir:+  fleet-dir=${fleet_dir}}  (cap ${CAP_BYTES}B / ${WINDOW_TOKENS} tok)"

# --- check 1: model-pin (§ Model pinning) ------------------------------------
# Walk every agent( call by paren-depth and flag any whose argument list carries
# no `model:` key. The whole file is slurped into one buffer (line by line, awk-
# portable — no gawk RS slurp dependency) then scanned char-by-char. A match must
# not be preceded by an identifier char, so `subagent(` / `myAgent(` don't trip
# it. Quotes/comments are ignored naively — acceptable for an advisory linter.
# Emits `WARN<TAB>snippet` per unpinned call plus a final `CALLS<TAB>n`.
model_out="$(awk '
    { buf = buf $0 "\n" }
    END {
        n = length(buf); i = 1; calls = 0
        while (i <= n - 5) {
            if (substr(buf, i, 6) == "agent(" \
                && (i == 1 || substr(buf, i - 1, 1) !~ /[A-Za-z0-9_]/)) {
                calls++
                depth = 0; started = 0; callbuf = ""; j = i + 5
                while (j <= n) {
                    ch = substr(buf, j, 1)
                    if (ch == "(") { depth++; started = 1 }
                    else if (ch == ")") { depth-- }
                    callbuf = callbuf ch
                    if (started && depth == 0) break
                    j++
                }
                if (callbuf !~ /model:/) {
                    snip = substr(callbuf, 1, 70); gsub(/\n/, " ", snip)
                    print "WARN\t" snip
                }
            }
            i++
        }
        print "CALLS\t" calls
    }
' "$script")"

agent_calls=0
while IFS=$'\t' read -r tag rest; do
    case "$tag" in
        WARN)  warn "agent() call without model: pin — ${rest}" ;;
        CALLS) agent_calls="$rest" ;;
    esac
done <<< "$model_out"

# --- check 2: input-size (§ Input-size pre-flight) ---------------------------
# Every staged input must be ≤ 1/3 of the agent context window; an oversized Read
# is an unrecoverable mid-run overflow (failure 3). Only runs when a fleet-dir is
# given and exists.
if [ -n "$fleet_dir" ] && [ -d "$fleet_dir" ]; then
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        sz=$(wc -c < "$f" 2>/dev/null || echo 0)
        if [ "$sz" -gt "$CAP_BYTES" ]; then
            warn "input over 1/3-window cap (${sz}B > ${CAP_BYTES}B): ${f}"
        fi
    done < <(find "$fleet_dir" -type f 2>/dev/null || true)
fi

# --- check 3: out-of-workspace paths (§ In-workspace staging) ----------------
# A background agent pointed at ~/.claude session dirs, %TEMP%, /tmp, or any
# absolute out-of-worktree path hits a permission deny-all and dies mid-run
# (failure 4). Flag each offending line in the script.
# Literal regex alternation, not shell text — the ~ and $-prefixes are pattern
# bytes matched against the workflow script, never expanded by this shell.
# shellcheck disable=SC2088,SC2016
oow_re='~/\.claude|\$HOME/\.claude|%TEMP%|\$TEMP|\$env:TEMP|/tmp/|[A-Za-z]:[\\/]Users[\\/][^\\/]+[\\/]\.claude'
while IFS=: read -r lineno text; do
    [ -z "$lineno" ] && continue
    trimmed="${text#"${text%%[![:space:]]*}"}"
    warn "out-of-workspace path (line ${lineno}): ${trimmed:0:70}"
done < <(grep -nE "$oow_re" "$script" 2>/dev/null || true)

# --- check 4: checkpoint-step (§ Checkpoint contract) ------------------------
# The runtime deletes its run dir on kill (failure 5, observed twice). If the
# script spawns agents but never mentions a build/ repo-file path, no agent is
# being told to checkpoint its deliverable to disk → a fleet death loses it all.
if [ "${agent_calls:-0}" -ge 1 ] && ! grep -qE 'build/' "$script"; then
    warn "no agent prompt references a repo-file checkpoint step (build/<slug>/...) — § Checkpoint contract"
fi

# --- check 5: concurrency reminder (§ Concurrency) ---------------------------
# Cannot read the runtime's effective concurrency from the script statically, so
# this is a non-failing reminder (note, not warn): when ≥1 sibling session is
# live, the fan-out must be capped at ≤ CONCURRENCY_CAP. Best-effort; silent when
# the registry dir is absent.
if [ -d "$ACTIVE_SESSIONS" ]; then
    live=$(find "$ACTIVE_SESSIONS" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
    if [ "${live:-0}" -ge 1 ]; then
        note "${live} sibling session(s) live — cap this fan-out at ≤ ${CONCURRENCY_CAP} concurrent agents (§ Concurrency)"
    fi
fi

# --- check 6: fan-out width vs slot count (§ Two fan-out mechanisms) ----------
# The in-process Workflow tool runs min(16, cores-2) agent() calls at once; a
# parallel()/pipeline() whose fan-out WIDTH (items in one call) tops that queues
# the tail, which reads as a stall in /workflows though nothing failed. Fires
# only for scripts that call parallel()/pipeline(); counts the largest top-level
# array-literal element count (the likely fan-out source — naive but adequate for
# an advisory linter, like the other checks here) and WARNs when it exceeds the
# local slot count. PREFLIGHT_SLOTS overrides the count for deterministic tests.
if grep -qE '(parallel|pipeline)[[:space:]]*\(' "$script"; then
    if [ -n "${PREFLIGHT_SLOTS:-}" ]; then
        slots="$PREFLIGHT_SLOTS"
    else
        cores=$(nproc 2>/dev/null || echo 4)
        slots=$(( cores - 2 )); [ "$slots" -lt 1 ] && slots=1
        [ "$slots" -gt 16 ] && slots=16
    fi
    # Largest top-level array-literal element count. An array-literal '[' is one
    # preceded (ignoring whitespace) by = ( , : [ { > or start-of-file — an index
    # access (foo[i]) is preceded by an identifier/')'/']' and is skipped. Element
    # count = commas at the array's own bracket depth + 1 (a trailing comma does
    # not add an element). Quotes/comments ignored naively, per this file's policy.
    width="$(awk '
        { buf = buf $0 "\n" }
        END {
            n = length(buf); maxw = 0; i = 1
            while (i <= n) {
                if (substr(buf, i, 1) != "[") { i++; continue }
                p = i - 1
                while (p >= 1 && substr(buf, p, 1) ~ /[ \t\r\n]/) p--
                pc = (p >= 1) ? substr(buf, p, 1) : "^"
                if (pc != "^" && pc !~ /[=(,:[{>]/) { i++; continue }
                depth = 1; commas = 0; sawelem = 0; lastComma = 0; j = i + 1
                while (j <= n) {
                    c = substr(buf, j, 1)
                    if (c ~ /[([{]/)       { depth++; sawelem = 1; lastComma = 0 }
                    else if (c ~ /[]})]/)  { depth--; if (depth == 0) break; sawelem = 1; lastComma = 0 }
                    else if (c == ",")     { if (depth == 1) { commas++; lastComma = 1 } sawelem = 1 }
                    else if (c !~ /[ \t\r\n]/) { sawelem = 1; lastComma = 0 }
                    j++
                }
                w = 0
                if (sawelem) { w = commas + 1; if (lastComma) w = commas }
                if (w > maxw) maxw = w
                i = j + 1
            }
            print maxw
        }
    ' "$script")"
    if [ "${width:-0}" -gt "$slots" ]; then
        warn "fan-out width ${width} > local slot count ${slots} (min(16,cores-2)) — tail queues & reads as a stall; merge clusters or wave the fan-out (§ Two fan-out mechanisms)"
    else
        note "fan-out width ${width:-0} ≤ ${slots} slot(s) (min(16,cores-2))"
    fi
fi

# --- checks 7-8: per-agent prompt read-discipline + path hygiene -------------
# Walk every agent() call (same paren-depth scan as check 1) and emit its full
# argument buffer once; shell-side, two prompt-SHAPE checks the static gate can
# see run per call. These are the runtime-killers the input-size byte cap (check
# 2) is blind to — a prompt that *tells* the agent to read broadly:
#   7. read-discipline (§ Per-agent scoping, failure 6): a prompt naming a broad
#      dir / glob scope with NO windowed-read directive lets the agent Read files
#      whole → transcript bloat → mid-run auto-compaction (the survey-fleet death).
#   8. path-hygiene (§ Semantic-search exceptions): a *.cpp/*.h/... token with no
#      '/' is a bare basename → "File does not exist" + a wasted recovery turn
#      that compounds compaction pressure (7 such errors in run wf_62807bcd-dc8).
calls_out="$(awk '
    { buf = buf $0 "\n" }
    END {
        n = length(buf); i = 1
        while (i <= n - 5) {
            if (substr(buf, i, 6) == "agent(" \
                && (i == 1 || substr(buf, i - 1, 1) !~ /[A-Za-z0-9_]/)) {
                depth = 0; started = 0; callbuf = ""; j = i + 5
                while (j <= n) {
                    ch = substr(buf, j, 1)
                    if (ch == "(") { depth++; started = 1 }
                    else if (ch == ")") { depth-- }
                    callbuf = callbuf ch
                    if (started && depth == 0) break
                    j++
                }
                gsub(/\n/, " ", callbuf)
                print "CALL\t" callbuf
            }
            i++
        }
    }
' "$script")"

# A prompt has "broad scope" when it points an agent at a directory / glob / the
# whole tree; it is DISCIPLINED when it also carries a windowed-read directive
# (offset/limit, "window", "never read whole", a file:line target, a tool-call
# budget, Grep-only, or the scope-refusing cavecrew-investigator agentType).
broad_re='Source/|tests/|docs/|\*\*|all files|every file|entire (repo|codebase|tree|directory)|across the (repo|codebase|tree)|examine all|whole (repo|tree|directory)'
windowed_re='offset|limit|window|never read|cavecrew-investigator|targeted read|stay under|tool call|grep|file:line|:[0-9]+|repo-relative'
# A bare source basename: a *.cpp/.hpp/.cc/.cxx/.hh/.h token NOT preceded by '/'
# (a path like Source/Foo.cpp has the '/' and is fine).
basename_re='(^|[^/A-Za-z0-9_.])[A-Za-z0-9_]+\.(cpp|hpp|cxx|cc|hh|h)([^A-Za-z0-9]|$)'
while IFS=$'\t' read -r tag callbuf; do
    [ "$tag" = CALL ] || continue
    [ -z "$callbuf" ] && continue
    snip="${callbuf:0:70}"
    if printf '%s' "$callbuf" | grep -qiE "$broad_re" \
       && ! printf '%s' "$callbuf" | grep -qiE "$windowed_re"; then
        warn "agent() names a broad scope with no windowed-read directive — the agent may Read files whole and auto-compact (§ Per-agent scoping): ${snip}"
    fi
    if printf '%s' "$callbuf" | grep -qE "$basename_re"; then
        warn "agent() prompt references a bare source basename (no path) — likely File-not-found + a wasted turn (§ Semantic-search exceptions): ${snip}"
    fi
done <<< "$calls_out"

# --- verdict -----------------------------------------------------------------
# Under --strict any WARN is a hard violation that exits non-zero and BLOCKS the
# launch (workflow-fleets.md § Pre-launch checklist makes --strict a MANDATORY
# pre-launch MUST above the documented fan-out threshold). The canonical hard
# violation — an unpinned multi-agent fan-out (check 1, failure 2) — is exercised
# by --selftest. Advisory default still exits 0.
echo "fleet-preflight: ${warn_count} warning(s) over ${agent_calls:-0} agent() call(s)."
if [ "$STRICT" -eq 1 ] && [ "$warn_count" -gt 0 ]; then
    echo "fleet-preflight: --strict — ${warn_count} hard violation(s); blocking launch." >&2
    exit 1
fi
exit 0
