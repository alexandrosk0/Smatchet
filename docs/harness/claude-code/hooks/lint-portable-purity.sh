#!/usr/bin/env bash
# PostToolUse hook: edit-time portable-purity guard.
#
# Runs agents/scripts/core/test-portable-purity.sh when an edited file lands
# under a PORTABLE dir (agents/core/, agents/_shared/, docs/agent-rules/,
# docs/harness/). Closes the edit-time fast-feedback gap: a NEW project-literal
# leak into a portable doc otherwise only fails at CR/CI (a full round-trip),
# never at the 5-second edit-time point. Same trigger/block shape as the C++
# lint PostToolUse hook (lint-cpp.sh).
#
# FAIL-OPEN by contract (docs/harness/SETUP.md § Hook-authoring checklist —
# promoting a gate-script into a per-call hook, point 2): a missing
# script, absent deps (git/python), an unparseable payload, or an edit OUTSIDE
# the portable dirs all exit 0 (allow). The hook only BLOCKS (exit 2) when the
# gate itself reports a NEW leak — never on its own plumbing failure.

set -u

# --- Read tool input JSON from stdin ---------------------------------------
INPUT="$(cat || true)"
[[ -z "$INPUT" ]] && exit 0

if command -v jq >/dev/null 2>&1; then
    FILE_PATH="$(printf '%s' "$INPUT" | jq -r '.tool_input.file_path // empty')"
else
    FILE_PATH="$(printf '%s' "$INPUT" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
fi
[[ -z "$FILE_PATH" ]] && exit 0

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"

# --- Normalise the edited path to a project-relative forward-slash form -----
NORM_PROJ="${PROJ_DIR//\\//}"
NORM_FILE="${FILE_PATH//\\//}"
if command -v cygpath >/dev/null 2>&1; then
    NORM_PROJ="$(cygpath -m "$NORM_PROJ" 2>/dev/null || printf '%s' "$NORM_PROJ")"
    NORM_FILE="$(cygpath -m "$NORM_FILE" 2>/dev/null || printf '%s' "$NORM_FILE")"
fi
REL="${NORM_FILE#"$NORM_PROJ"/}"
# Path outside the project root → nothing to check (fail-open).
[[ "$REL" == "$NORM_FILE" ]] && exit 0

# --- Only portable dirs trigger the gate -----------------------------------
case "$REL" in
    agents/core/*|agents/_shared/*|docs/agent-rules/*|docs/harness/*) : ;;
    *) exit 0 ;;
esac

# --- Locate the gate (moved build-vs-agentic; tolerate either home) ---------
GATE=""
for cand in \
    "$PROJ_DIR/agents/scripts/core/test-portable-purity.sh" \
    "$PROJ_DIR/scripts/dev/test-portable-purity.sh"; do
    [[ -f "$cand" ]] && { GATE="$cand"; break; }
done
[[ -z "$GATE" ]] && exit 0   # gate absent → fail-open

# --- Run the gate (--check). Exit 1 = NEW leak; 2 = missing dep (fail-open) -
OUT="$(CLAUDE_PROJECT_DIR="$PROJ_DIR" bash "$GATE" --check 2>&1)"
RC=$?

if [[ "$RC" -eq 1 ]]; then
    {
        echo "[portable-purity-hook] NEW project-literal leak in a portable dir ($REL) — fix before responding."
        printf '%s\n' "$OUT"
        echo "[portable-purity-hook] Use a config-key reference instead, or (if intentional) refresh the baseline:"
        echo "[portable-purity-hook]   bash agents/scripts/core/test-portable-purity.sh --refresh"
    } >&2
    exit 2
fi

# RC 0 (clean) or RC 2 (python/git missing) → allow. The full test-all.sh run
# remains the authoritative gate; this hook is edit-time fast feedback only.
exit 0
