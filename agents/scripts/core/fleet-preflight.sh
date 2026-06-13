#!/usr/bin/env bash
# fleet-preflight.sh — mechanical pre-launch validation of a Workflow fleet
# against docs/agent-rules/workflow-fleets.md § Pre-launch checklist. Every check
# below maps 1:1 to a now-written rule whose violation killed a real fleet during
# the 2026-06-10/11 audit-fleet debacle (failure inventory in that doc).
#
# Usage:
#   fleet-preflight.sh <workflow-script> [fleet-dir] [--strict]
#
#   <workflow-script>  the JS passed to the Workflow tool (the agent() fan-out).
#   [fleet-dir]        optional staged input/output dir (build/<slug>/); when
#                      given, every file under it is size-checked.
#   --strict           exit 1 if any WARN fired (gate mode). Default: advisory
#                      (exit 0 regardless), per "WARN-first, gate after
#                      calibration" — workflow-fleets.md line 87 backlog item.
#
# Checks (each cites the rule-doc section it enforces):
#   1. model-pin       (§ Model pinning)        every agent() call carries model:
#   2. input-size      (§ Input-size pre-flight) every staged file ≤ 1/3 window
#   3. out-of-workspace(§ In-workspace staging)  no ~/.claude / %TEMP% / abs paths
#   4. checkpoint-step (§ Checkpoint contract)   prompts write to a repo file
#   5. concurrency     (§ Concurrency)           reminder when siblings are live
#
# Advisory by default (WARN lines, exit 0) so it can run on every launch without
# blocking; --strict promotes it to a gate once the WARN rate is calibrated.
#
# Test seams (production leaves all unset → identical behaviour):
#   PREFLIGHT_WINDOW_TOKENS    context-window token budget (default 200000); the
#                              1/3 byte cap = WINDOW_TOKENS/3*4 (~4 bytes/token).
#   PREFLIGHT_ACTIVE_SESSIONS  live-session registry dir (default .claude/.active-sessions).
#   PREFLIGHT_CONCURRENCY_CAP  the ≤N reminder threshold (default 5).

set -euo pipefail
cd "$(dirname "$0")/../../.."

usage() { echo "usage: fleet-preflight.sh <workflow-script> [fleet-dir] [--strict]" >&2; }

STRICT=0
posargs=()
for a in "$@"; do
    case "$a" in
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

# --- verdict -----------------------------------------------------------------
echo "fleet-preflight: ${warn_count} warning(s) over ${agent_calls:-0} agent() call(s)."
if [ "$STRICT" -eq 1 ] && [ "$warn_count" -gt 0 ]; then
    exit 1
fi
exit 0
