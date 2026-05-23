#!/usr/bin/env bash
# test-setup-harness.sh — V1 + V7 verification for docs/design/agents-skill-conversion.md.
#
# Covers:
#   1. setup-harness.sh claude-code creates .claude/skills/perf-measure + .claude/skills/perf-instrument symlinks.
#   2. Existing skills (grill-with-docs, scratchpad-recall) still linked (no regression).
#   3. Re-running setup-harness.sh produces zero new link-dir lines (idempotency).
#   4. agents/perf-{measure,instrument}.md still exist (dual-publish: agent form preserved for Codex / Cursor).
#   5. SKILL.md aliases exist + contain a sync-warning header comment.
#   6. V7 doc-consistency: rg matches in agents/ + docs/ are consistent with dual-publish wording
#      (perf-detective / spike-hunter / debug-detective all mention skill-form availability).
#
# Auto-enrolled by scripts/dev/test-all.sh.

set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$PROJ_DIR"

PASS=0
FAIL=0
declare -a FAILURES=()

note() { echo "[setup-harness] $*"; }
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
nope() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1"; }

# -------------------------------------------------------------------- Test 1
note "Test 1 — first run links perf-measure + perf-instrument skills"
# setup-harness.sh is idempotent; if links exist, this is a no-op.
SETUP_OUT_1=$(bash scripts/setup-harness.sh claude-code 2>&1)

if [[ -e ".claude/skills/perf-measure/SKILL.md" ]]; then
    ok "perf-measure SKILL.md reachable via .claude/skills/"
else
    nope "perf-measure SKILL.md missing under .claude/skills/"
fi

if [[ -e ".claude/skills/perf-instrument/SKILL.md" ]]; then
    ok "perf-instrument SKILL.md reachable via .claude/skills/"
else
    nope "perf-instrument SKILL.md missing under .claude/skills/"
fi

# -------------------------------------------------------------------- Test 2
note "Test 2 — existing skills still linked (no regression)"
for existing in grill-with-docs scratchpad-recall agent-tokens; do
    if [[ -e ".claude/skills/$existing/SKILL.md" ]]; then
        ok "$existing SKILL.md reachable"
    else
        nope "$existing SKILL.md missing — regression"
    fi
done

# -------------------------------------------------------------------- Test 3
note "Test 3 — re-run produces zero new link-dir lines (idempotency)"
SETUP_OUT_2=$(bash scripts/setup-harness.sh claude-code 2>&1)
NEW_LINKS=$(echo "$SETUP_OUT_2" | grep -c '^\s*link-' || true)
if [[ "$NEW_LINKS" -eq 0 ]]; then
    ok "idempotent re-run (zero new link- lines)"
else
    nope "re-run produced $NEW_LINKS link- lines (expected 0)"
fi

# -------------------------------------------------------------------- Test 4
note "Test 4 — agent files preserved under dual-publish"
for agent in perf-measure perf-instrument; do
    if [[ -f "agents/$agent.md" ]]; then
        ok "agents/$agent.md exists (cross-harness canonical)"
    else
        nope "agents/$agent.md missing — dual-publish broken"
    fi
done

# -------------------------------------------------------------------- Test 5
note "Test 5 — SKILL.md aliases have sync-warning header"
for skill in perf-measure perf-instrument; do
    SKILL_FILE="agents/_shared/skills/$skill/SKILL.md"
    if [[ ! -f "$SKILL_FILE" ]]; then
        nope "$SKILL_FILE missing"
        continue
    fi
    if grep -q "Claude-Code skill mirror of agents/$skill.md" "$SKILL_FILE"; then
        ok "$skill SKILL.md carries sync-warning header"
    else
        nope "$skill SKILL.md missing sync-warning header"
    fi
done

# -------------------------------------------------------------------- Test 6
note "Test 6 — V7 doc-consistency: dependent agents mention skill-form availability"
for agent in perf-detective spike-hunter debug-detective; do
    if grep -q "Helper-form preference" "agents/$agent.md"; then
        ok "$agent.md mentions skill-form preference"
    else
        nope "$agent.md missing Helper-form preference block"
    fi
done

if grep -q "Claude Code skill alias" "docs/agent-rules/delegation.md"; then
    ok "delegation.md mentions skill-alias availability"
else
    nope "delegation.md missing skill-alias note"
fi

# -------------------------------------------------------------------- Report
echo
# test-all.sh aggregator parses the literal "Passed: N  Failed: M" line.
echo "Passed: $PASS  Failed: $FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo
    echo "setup-harness failures:"
    for msg in "${FAILURES[@]}"; do echo "  - $msg"; done
    exit 1
fi
exit 0
