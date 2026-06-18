#!/usr/bin/env bash
# UserPromptSubmit hook — capture the user's prompt as a redacted one-liner.
#
# Persists a single safe line per prompt into a gitignored, branch-keyed capture
# file (.session-intent/<branch>.log). The ship-loop orchestrator reads that file
# at `gh pr create` time to synthesise the PR `## Intent` section (traceability:
# human ask -> diff). See docs/agent-rules/ship-loops.md § Intent capture.
#
# SECURITY / CONTRACT:
#   * UserPromptSubmit stdout is injected into the model's context. This hook must
#     therefore print NOTHING to stdout — it is a pure side-effect writer.
#   * The raw prompt may carry secrets / home-dir usernames. It is ALWAYS routed
#     through agents/scripts/core/redact-intent.py first. FAIL-SAFE: if the
#     redactor is unavailable or errors, the hook writes NOTHING (never the raw
#     prompt) and still exits 0 — capture is best-effort, never a leak and never
#     a block on prompt submission.
set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
REDACTOR="$PROJ_DIR/agents/scripts/core/redact-intent.py"

INPUT="$(cat || true)"
[[ -z "$INPUT" ]] && exit 0

# Resolve a python interpreter (required for both JSON parse fallback + redaction).
# Probe-EXECUTE each candidate, don't just `command -v` it: on Windows `python3`
# is usually the Microsoft Store App-execution-alias stub — it resolves on PATH
# but errors on run ("Python was not found"). A name-only check would pick the
# stub, the redactor would never run, and capture would silently no-op.
PY=""
for _cand in python3 python py; do
    if command -v "$_cand" >/dev/null 2>&1 && "$_cand" -c "import sys" >/dev/null 2>&1; then
        PY="$_cand"; break
    fi
done

# Extract .prompt from the event JSON: jq primary, python-json fallback.
PROMPT=""
if command -v jq >/dev/null 2>&1; then
    PROMPT="$(printf '%s' "$INPUT" | jq -r '.prompt // empty' 2>/dev/null || true)"
elif [[ -n "$PY" ]]; then
    PROMPT="$(printf '%s' "$INPUT" | "$PY" -c 'import sys,json
try: sys.stdout.write(json.load(sys.stdin).get("prompt","") or "")
except Exception: pass' 2>/dev/null || true)"
fi
[[ -z "$PROMPT" ]] && exit 0

# Fail-safe: no interpreter or no redactor -> write nothing (never the raw prompt).
[[ -z "$PY" || ! -f "$REDACTOR" ]] && exit 0

REDACTED="$(printf '%s' "$PROMPT" | "$PY" "$REDACTOR" 2>/dev/null || true)"
[[ -z "$REDACTED" ]] && exit 0

# Branch-keyed capture file (batched feature -> one branch -> one PR -> one log).
# symbolic-ref --short (not rev-parse --abbrev-ref): on an UNBORN branch rev-parse
# prints "HEAD" to stdout AND exits non-zero, so `|| echo _unknown` appended both
# -> "HEAD-_unknown.log"; symbolic-ref returns the real branch (exit 0) on an
# unborn branch and fails cleanly (no stdout) on detached HEAD -> _unknown.
BRANCH="$(git -C "$PROJ_DIR" symbolic-ref --short HEAD 2>/dev/null || echo _unknown)"
SAFE_BRANCH="$(printf '%s' "$BRANCH" | tr -c 'A-Za-z0-9._-' '-')"
[[ -z "$SAFE_BRANCH" ]] && SAFE_BRANCH="_unknown"

CAPTURE_DIR="$PROJ_DIR/.session-intent"
mkdir -p "$CAPTURE_DIR" 2>/dev/null || exit 0
CAPTURE_FILE="$CAPTURE_DIR/$SAFE_BRANCH.log"

# Provenance header on first create: marks the file as hook-produced (vs a
# hand-authored fallback file), so the orchestrator / a debugger can confirm
# capture actually ran in a live session. The orchestrator reads ONLY `- `-prefixed
# lines, so this `#` header is inert to Intent synthesis (docs/agent-rules/ship-loops.md
# § Intent capture). Best-effort; a missing date never blocks the append.
if [[ ! -f "$CAPTURE_FILE" ]]; then
    printf '# capture-intent v1 %s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)" \
        >> "$CAPTURE_FILE" 2>/dev/null || true
fi

printf '%s\n' "- $REDACTED" >> "$CAPTURE_FILE" 2>/dev/null || true

# Cap-on-append: keep the provenance header + last CAP bullet lines so a long-lived
# branch's log can't grow unbounded (the file is per-container ephemeral in remote
# sessions, but local branches persist). Best-effort; never blocks prompt submission.
# CAP is env-overridable so the bats suite can exercise the trim cheaply.
# Neutral name (no project literal) — this template is portable across repos.
CAP="${CAPTURE_INTENT_CAP:-200}"
line_count="$(wc -l < "$CAPTURE_FILE" 2>/dev/null || echo 0)"
if [[ "$line_count" -gt $((CAP + 1)) ]]; then
    _tmp="$CAPTURE_FILE.tmp"
    {
        grep -m1 '^# capture-intent ' "$CAPTURE_FILE" 2>/dev/null || true
        grep '^- ' "$CAPTURE_FILE" 2>/dev/null | tail -n "$CAP" || true
    } > "$_tmp" 2>/dev/null
    mv "$_tmp" "$CAPTURE_FILE" 2>/dev/null || rm -f "$_tmp" 2>/dev/null
fi

# No stdout — UserPromptSubmit stdout would be injected into model context.
exit 0
