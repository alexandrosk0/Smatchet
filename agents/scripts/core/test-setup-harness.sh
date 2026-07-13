#!/usr/bin/env bash
# test-setup-harness.sh — V1 + V7 verification for docs/plans/shipped/agents-skill-conversion.md.
#
# Covers:
#   1. setup-harness.sh claude-code creates .claude/skills/perf-measure + .claude/skills/perf-instrument symlinks.
#   2. Existing skills (grill-with-docs, scratchpad-recall) still linked (no regression).
#   3. Re-running setup-harness.sh produces zero new link-dir lines (idempotency).
#   4. agents/perf-{measure,instrument}.md still exist (dual-publish: agent form preserved for Codex / Cursor).
#   5. SKILL.md aliases exist + contain a sync-warning header comment.
#   6. V7 doc-consistency: rg matches in agents/ + docs/ are consistent with dual-publish wording
#      (perf-detective / spike-hunter / debug-detective all mention skill-form availability).
#   7. sync-settings-hooks.sh heals missing template hooks additively.
#   8. Codex setup verifies native discovery, generates Codex-native
#      hooks/custom agents, and does not clobber Claude Code setup.
#
# Auto-enrolled by scripts/dev/test-all.sh.

set -euo pipefail

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"
cd "$PROJ_DIR" || exit 1

PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then
        PY="$_p"
        break
    fi
done

PASS=0
FAIL=0
declare -a FAILURES=()

note() { echo "[setup-harness] $*"; }
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
nope() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1"; }

# --- Hermetic-adapter guard (order-independence for test-all.sh) --------------
# setup-harness.sh has no root override — it regenerates the REAL .claude/ adapter
# (agents/hooks/skills/settings). This test invokes it against the live tree, so
# left unrestored it FRESHENS .claude/ and MASKS the staleness that
# test-adapter-drift / test-agent-contract exist to catch: if this test runs
# earlier in test-all.sh, a genuine stale-adapter drift is silently re-linked away
# (reproduced: drift → this test → drift now passes). Snapshot the adapter
# artifacts this test perturbs and restore them byte-for-byte on exit, so the
# shared mirror is exactly as we found it regardless of suite order.
_HARNESS_SNAP="$(mktemp -d)"
_HARNESS_SNAP_PATHS=(.claude/agents .claude/hooks .claude/skills .claude/settings.json)
for _hp in "${_HARNESS_SNAP_PATHS[@]}"; do
    if [ -e "$_hp" ]; then
        mkdir -p "$_HARNESS_SNAP/$(dirname "$_hp")"
        cp -a "$_hp" "$_HARNESS_SNAP/$_hp" 2>/dev/null || true
    fi
done
# shellcheck disable=SC2329  # invoked indirectly via the EXIT trap below
_restore_harness_snapshot() {
    local _hp
    for _hp in "${_HARNESS_SNAP_PATHS[@]}"; do
        rm -rf "$_hp" 2>/dev/null || true
        if [ -e "$_HARNESS_SNAP/$_hp" ]; then
            mkdir -p "$(dirname "$_hp")"
            cp -a "$_HARNESS_SNAP/$_hp" "$_hp" 2>/dev/null || true
        fi
    done
    rm -rf "$_HARNESS_SNAP"
}
trap _restore_harness_snapshot EXIT

# Resolve an agent's canonical .md across the post-reorg layout (agents/core/
# + agents/project/, falling back to the legacy flat path). The agentic reorg
# (PRs #542-549) split agents into core/ + project/; this test follows.
# Prints the path and returns 0 if found; returns 1 otherwise.
resolve_agent_md() {
    local name="$1"
    local candidate
    for candidate in "agents/core/${name}.md" "agents/project/${name}.md" "agents/${name}.md"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

# -------------------------------------------------------------------- Test 1
note "Test 1 — first run links perf-measure + perf-instrument skills"
# setup-harness.sh is idempotent; if links exist, this is a no-op.
SETUP_OUT_1=$(bash agents/scripts/core/setup-harness.sh claude-code 2>&1) || true  # assert on artifacts regardless of exit; keep the suite running

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
SETUP_OUT_2=$(bash agents/scripts/core/setup-harness.sh claude-code 2>&1) || true  # assert on artifacts regardless of exit; keep the suite running
NEW_LINKS=$(echo "$SETUP_OUT_2" | grep -c '^\s*link-' || true)
if [[ "$NEW_LINKS" -eq 0 ]]; then
    ok "idempotent re-run (zero new link- lines)"
else
    nope "re-run produced $NEW_LINKS link- lines (expected 0)"
fi

# -------------------------------------------------------------------- Test 4
note "Test 4 — agent files preserved under dual-publish"
for agent in perf-measure perf-instrument; do
    if agent_path="$(resolve_agent_md "$agent")"; then
        ok "$agent_path exists (cross-harness canonical)"
    else
        nope "agents/{core,project}/$agent.md missing — dual-publish broken"
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
    # The agentic reorg (PRs #542-549) reworded the sync-warning header and
    # moved the referenced agent file from agents/<skill>.md to
    # agents/core/<skill>.md. Match on the durable intent (a "mirror" header
    # that points back at the canonical agent file + a keep-in-sync directive)
    # rather than the exact pre-reorg sentence.
    if grep -qi "mirror" "$SKILL_FILE" && grep -qi "in sync" "$SKILL_FILE"; then
        ok "$skill SKILL.md carries sync-warning header"
    else
        nope "$skill SKILL.md missing sync-warning header"
    fi
done

# -------------------------------------------------------------------- Test 6
note "Test 6 — V7 doc-consistency: dependent agents mention skill-form availability"
for agent in perf-detective spike-hunter debug-detective; do
    if ! agent_path="$(resolve_agent_md "$agent")"; then
        nope "agents/{core,project}/$agent.md (or legacy agents/$agent.md) missing"
    elif grep -q "Helper-form preference" "$agent_path"; then
        ok "$agent_path mentions skill-form preference"
    else
        nope "$agent_path missing Helper-form preference block"
    fi
done

if grep -q "Claude Code skill alias" "docs/agent-rules/delegation.md"; then
    ok "delegation.md mentions skill-alias availability"
else
    nope "delegation.md missing skill-alias note"
fi

# -------------------------------------------------------------------- Test 7
note "Test 7 — sync-settings-hooks.sh heals missing template hooks (additive, non-destructive)"
T7_SYNC="agents/scripts/core/sync-settings-hooks.sh"
T7_TMPL="docs/harness/claude-code/settings.json.tmpl"
if ! command -v jq >/dev/null 2>&1; then
    ok "Test 7 skipped — jq not installed (sync degrades to a WARN by design)"
elif [[ ! -f "$T7_SYNC" || ! -f "$T7_TMPL" ]]; then
    nope "Test 7 — $T7_SYNC or $T7_TMPL missing"
else
    T7_DIR="$(mktemp -d)"
    T7_DEP="$T7_DIR/settings.json"
    # Deployed: permissions sentinel + a user-added custom SessionStart hook +
    # only the bootstrap hook + only lint-cpp-drain under Stop (the proven gap).
    cat > "$T7_DEP" <<'JSON'
{
  "permissions": { "defaultMode": "plan", "allow": ["Bash(ls:*)"] },
  "hooks": {
    "SessionStart": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "bash \"$CLAUDE_PROJECT_DIR/agents/scripts/core/clear-session-context.sh\"", "timeout": 3000 },
        { "type": "command", "command": "echo t7-user-hook", "timeout": 1000 }
      ]}
    ],
    "Stop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "bash \"$CLAUDE_PROJECT_DIR/.claude/hooks/lint-cpp-drain.sh\"", "timeout": 120000 }
      ]}
    ]
  }
}
JSON
    bash "$T7_SYNC" "$T7_TMPL" "$T7_DEP" >/dev/null 2>&1 || true
    T7_BEFORE="$(cat "$T7_DEP")"
    bash "$T7_SYNC" "$T7_TMPL" "$T7_DEP" >/dev/null 2>&1 || true   # 2nd run = idempotent?
    T7_AFTER="$(cat "$T7_DEP")"

    if [[ -z "$PY" ]]; then
        nope "Test 7 - python not found for sync-settings-hooks assertion"
    elif "$PY" - "$T7_DEP" "$T7_TMPL" <<'PY'
import json, sys
dep = json.load(open(sys.argv[1])); tmpl = json.load(open(sys.argv[2]))
assert dep.get("permissions") == {"defaultMode": "plan", "allow": ["Bash(ls:*)"]}, "permissions clobbered"
def cmds(o):
    return {(ev, g.get("matcher", ""), h["command"])
            for ev, groups in o["hooks"].items() for g in groups for h in g["hooks"]}
missing = cmds(tmpl) - cmds(dep)
assert not missing, f"template hooks not healed: {missing}"
ss = [h["command"] for g in dep["hooks"]["SessionStart"] for h in g["hooks"]]
assert "echo t7-user-hook" in ss, "user hook lost"
assert len([g for g in dep["hooks"]["Stop"] if g.get("matcher", "") == ""]) == 1, "duplicate Stop matcher group"
PY
    then
        ok "sync heals missing hooks (permissions preserved, user hook kept, no Stop dup)"
    else
        nope "sync-settings-hooks.sh merge incorrect"
    fi
    if [[ "$T7_BEFORE" == "$T7_AFTER" ]]; then
        ok "sync is idempotent (2nd run no-op)"
    else
        nope "sync not idempotent — 2nd run changed the file"
    fi
    rm -rf "$T7_DIR"
fi

# -------------------------------------------------------------------- Test 8
note "Test 8 - codex setup generates native hooks/agents without clobbering Claude setup"
SETUP_CODEX_OUT=$(bash agents/scripts/core/setup-harness.sh codex 2>&1) || true  # assert on artifacts regardless of exit; keep the suite running
if echo "$SETUP_CODEX_OUT" | grep -q 'Codex parity report:'; then
    ok "codex setup emits parity report"
else
    nope "codex setup missing parity report"
fi
if echo "$SETUP_CODEX_OUT" | grep -q 'agents/{core,project}/\*.md = '; then
    ok "codex setup counts agents/{core,project}/*.md"
else
    nope "codex setup did not count canonical agent files"
fi
if echo "$SETUP_CODEX_OUT" | grep -q '\.codex/agents/\*\.toml'; then
    ok "codex setup reports generated custom agents"
else
    nope "codex setup missing generated custom-agent report"
fi
if echo "$SETUP_CODEX_OUT" | grep -q 'Safe SessionStart/Stop command hooks'; then
    ok "codex setup reports Codex-native hook install"
else
    nope "codex setup missing Codex-native hook install report"
fi
if echo "$SETUP_CODEX_OUT" | grep -q 'git-hooks'; then
    ok "codex setup reports git-hook wiring state"
else
    nope "codex setup missing git-hook wiring report"
fi
if [[ -f ".codex/config.toml" && -f ".codex/hooks.json" ]]; then
    ok ".codex config + hooks generated"
else
    nope ".codex config or hooks missing after codex setup"
fi
CANONICAL_AGENT_COUNT=$(( $(find agents/core -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ' || true) + $(find agents/project -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ' || true) ))
CODEX_AGENT_COUNT=$(find .codex/agents -maxdepth 1 -name '*.toml' 2>/dev/null | wc -l | tr -d ' ' || true)
if [[ "$CODEX_AGENT_COUNT" -eq "$CANONICAL_AGENT_COUNT" ]]; then
    ok ".codex/agents count matches canonical agent count"
else
    nope ".codex/agents count $CODEX_AGENT_COUNT did not match canonical count $CANONICAL_AGENT_COUNT"
fi
if [[ -f ".codex/agents/code-review.toml" ]] \
    && grep -q 'name = "code-review"' ".codex/agents/code-review.toml" \
    && grep -q 'developer_instructions =' ".codex/agents/code-review.toml"; then
    ok "generated code-review Codex agent has required fields"
else
    nope "generated code-review Codex agent missing required fields"
fi
if [[ -n "$PY" ]] && "$PY" -m json.tool ".codex/hooks.json" >/dev/null 2>&1; then
    ok ".codex/hooks.json is valid JSON"
elif [[ -z "$PY" ]]; then
    nope "python not found for .codex/hooks.json validation"
else
    nope ".codex/hooks.json is invalid JSON"
fi
if [[ -f ".claude/settings.json" && -e ".claude/skills/perf-measure/SKILL.md" ]]; then
    ok "codex setup left Claude Code adapter intact"
else
    nope "codex setup clobbered Claude Code adapter files"
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
