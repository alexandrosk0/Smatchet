#!/usr/bin/env bash
# test-skill-vs-agent-parity.sh — guard against skill / agent mismatch.
#
# Smatchet's perf-* helpers ship in two forms:
#   - `agents/{core,project}/<name>.md` (Codex / Cursor / Aider / generic harness)
#   - `agents/_shared/skills/<name>/SKILL.md` (Claude Code skill)
#
# NOTE: the agentic reorg (PRs #542-549) split agents into agents/core/ +
# agents/project/. This test resolves the agent twin in either subdir rather
# than the pre-reorg flat agents/<name>.md path.
#
# This test asserts every skill that names a corresponding agent file
# actually has one (shape check — catches a renamed agent leaving an
# orphaned skill, or a new skill without its agent twin). Text body drift
# between the two is NOT a failure today — the forms diverge intentionally
# (skill prose is shorter than agent prose). When functional parity testing
# is added (driver scripts + stdout diff per harness), it lands as a
# separate stretch script.
#
# Exit codes:
#   0 — every shipped skill has its matching agent file
#   1 — skill exists without its agent twin
#   2 — environment problem

set -euo pipefail

cd "$(dirname "$0")/../../.."

PASS=0
FAIL=0

# Skills that are intentionally skill-only (no agent twin). Add new
# skill-only helpers here to keep the assertion accurate.
SKILL_ONLY_HELPERS=(
    grill-with-docs
    scratchpad-recall
    author-plan-doc
    gate-escape-postmortem
    adversarial-code-review
    but-for-real
    drain-memory
    debug-instrument
    git-cleanup-procedures
    test-authoring
    coderabbit-handoff
    historical-code-review
    pre-implementation-review
    address-review-feedback
    close-work-item
)

is_skill_only() {
    local name="$1"
    for s in "${SKILL_ONLY_HELPERS[@]}"; do
        [ "$s" = "$name" ] && return 0
    done
    return 1
}

# Resolve an agent's canonical .md across the post-reorg layout. Prints the
# path and returns 0 if found; returns 1 otherwise. Checks agents/core/ and
# agents/project/ (and the legacy flat path, for backward compatibility).
resolve_agent_md() {
    local name="$1"
    local candidate
    for candidate in "agents/core/${name}.md" "agents/project/${name}.md" "agents/${name}.md"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

for skill_dir in agents/_shared/skills/*/; do
    [ -d "$skill_dir" ] || continue
    skill_md="$skill_dir/SKILL.md"
    [ -f "$skill_md" ] || continue
    name="$(basename "$skill_dir")"

    if is_skill_only "$name"; then
        echo "SKIP: $name (intentionally skill-only — no agent twin expected)"
        continue
    fi

    if agent_md="$(resolve_agent_md "$name")"; then
        echo "PASS: $name (skill ↔ agent both present: $agent_md)"
        PASS=$((PASS+1))
    else
        echo "FAIL: $name — SKILL.md ships at ${skill_md} but no canonical agent file exists (agents/core|agents/project|legacy agents/${name}.md)"
        echo "  Either add agents/core/${name}.md or agents/project/${name}.md, OR add '$name' to SKILL_ONLY_HELPERS in this script"
        FAIL=$((FAIL+1))
    fi
done

if [ "$PASS" -eq 0 ] && [ "$FAIL" -eq 0 ]; then
    echo "test-skill-vs-agent-parity: no skills found (vacuously passes)"
fi

echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ]
