#!/usr/bin/env bash
# PreToolUse hook: mechanically enforce the fleet-preflight § Pre-launch checklist
# on a `Workflow` fan-out (docs/agent-rules/workflow-fleets.md). The orchestrator
# MUST run `fleet-preflight.sh --strict` before any fan-out above 2 agents
# (tooling.md `fleet-preflight-not-enforced-at-launch`, option (a) shipped #1354).
# This hook is option (b): the stronger, can't-forget enforcement — it runs the
# preflight on the Workflow tool's inline `script` (or `scriptPath`) and BLOCKS a
# launch that carries a hard violation (an unpinned multi-agent fan-out — the
# TPM-starvation death this whole gate was written from).
#
# Opt-in: this hook only fires when a `PreToolUse` matcher for `Workflow` is wired
# into settings.json (docs/harness/claude-code/settings.json.tmpl). `.claude/` is
# a gitignored re-sync; the template here is what survives commit. It alters
# behaviour for every session, hence the explicit settings opt-in.
#
# Threshold + downgrade envs (production leaves all unset → block-on-hard-violation
# for fan-outs above 2 agents):
#   SMATCHET_FLEET_PREFLIGHT_HOOK=0          fully disable (no-op, exit 0).
#   SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK=0    downgrade block → advisory (warn to
#                                            stderr, exit 0). Default 1 (block).
#   SMATCHET_FLEET_PREFLIGHT_HOOK_MIN_AGENTS only block when the script's agent()
#                                            count EXCEEDS this (default 2 — the
#                                            documented ">2 agents" MUST). A 1–2
#                                            agent helper is never blocked.
#
# FAIL-OPEN by design: a guard that blocks every Workflow when jq is missing / the
# preflight script is absent / the payload has no inline script would be worse
# than the gap it closes. Each of those exits 0 (allow) with a stderr note. The
# only non-zero exit is a CONFIRMED hard violation in an above-threshold fan-out.
#
# PreToolUse protocol: tool-input JSON on stdin; exit 0 = allow, exit 2 = block
# (stderr is surfaced to the session as the block reason).

set -u

# --- opt-out -----------------------------------------------------------------
[ "${SMATCHET_FLEET_PREFLIGHT_HOOK:-1}" = "0" ] && exit 0

MIN_AGENTS="${SMATCHET_FLEET_PREFLIGHT_HOOK_MIN_AGENTS:-2}"
case "$MIN_AGENTS" in ''|*[!0-9]*) MIN_AGENTS=2 ;; esac
BLOCK="${SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK:-1}"

# --- locate the repo root + the preflight script -----------------------------
# Prefer the harness-provided project dir; fall back to `git rev-parse` from the
# script's own dir (robust whether this runs from the canonical
# docs/harness/claude-code/hooks/ path or the synced .claude/hooks/ copy, which
# sit at different depths).
ROOT="${CLAUDE_PROJECT_DIR:-}"
if [ -z "$ROOT" ]; then
    ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel 2>/dev/null || true)"
fi
[ -n "$ROOT" ] || { echo "[fleet-preflight-hook] cannot resolve repo root — skipping (fail-open)" >&2; exit 0; }
PREFLIGHT="$ROOT/agents/scripts/core/fleet-preflight.sh"
[ -f "$PREFLIGHT" ] || { echo "[fleet-preflight-hook] fleet-preflight.sh not found — skipping (fail-open)" >&2; exit 0; }

# --- read tool-input JSON ----------------------------------------------------
INPUT="$(cat || true)"
[ -z "$INPUT" ] && exit 0

# jq is the only reliable way to pull a large multi-line JS string out of the
# payload; a sed/regex extraction of an arbitrary script body is not safe. Absent
# jq → fail-open (the documented MUST still stands as author discipline).
command -v jq >/dev/null 2>&1 || { echo "[fleet-preflight-hook] jq unavailable — skipping (fail-open)" >&2; exit 0; }

TOOL="$(printf '%s' "$INPUT" | jq -r '.tool_name // empty' 2>/dev/null || true)"
# The matcher should already scope this to Workflow; double-check so a mis-wired
# matcher can't make the hook act on another tool.
[ -n "$TOOL" ] && [ "$TOOL" != "Workflow" ] && exit 0

SCRIPT_INLINE="$(printf '%s' "$INPUT" | jq -r '.tool_input.script // empty' 2>/dev/null || true)"
SCRIPT_PATH="$(printf '%s' "$INPUT" | jq -r '.tool_input.scriptPath // empty' 2>/dev/null || true)"

# --- resolve the script to validate ------------------------------------------
tmp=""
target=""
if [ -n "$SCRIPT_PATH" ] && [ -f "$SCRIPT_PATH" ]; then
    target="$SCRIPT_PATH"
elif [ -n "$SCRIPT_INLINE" ]; then
    tmp="$(mktemp 2>/dev/null || true)"
    [ -z "$tmp" ] && exit 0          # mktemp failed → fail-open
    printf '%s' "$SCRIPT_INLINE" > "$tmp"
    target="$tmp"
else
    # name-only (a saved workflow, pre-vetted) or empty payload → nothing to
    # statically validate here.
    exit 0
fi

# --- run the preflight gate --------------------------------------------------
out="$(bash "$PREFLIGHT" "$target" --strict 2>&1)"; rc=$?
# fleet-preflight prints: "fleet-preflight: <n> warning(s) over <m> agent() call(s)."
agents="$(printf '%s' "$out" | sed -n 's/.*over \([0-9][0-9]*\) agent().*/\1/p' | tail -n1)"
agents="${agents:-0}"
[ -n "$tmp" ] && rm -f "$tmp" 2>/dev/null || true

# --- decide ------------------------------------------------------------------
# Only a CONFIRMED hard violation (rc!=0) in an above-threshold fan-out acts. A
# clean fan-out, or a 1–2 agent helper with a stray warn, passes untouched.
if [ "$rc" -ne 0 ] && [ "$agents" -gt "$MIN_AGENTS" ]; then
    {
        echo "[fleet-preflight-hook] Workflow fan-out of ${agents} agent() call(s) has a hard pre-launch violation:"
        printf '%s\n' "$out" | grep -E '^(WARN|fleet-preflight:)' || true
        echo "[fleet-preflight-hook] Per docs/agent-rules/workflow-fleets.md § Pre-launch checklist: pin model: on every agent(), stage inputs under build/<slug>/, give broad-scope prompts a windowed-read directive."
        echo "[fleet-preflight-hook] Re-check: bash agents/scripts/core/fleet-preflight.sh <script> --strict"
    } >&2
    if [ "$BLOCK" != "0" ]; then
        echo "[fleet-preflight-hook] blocking launch (set SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK=0 to downgrade to advisory)." >&2
        exit 2
    fi
fi
exit 0
