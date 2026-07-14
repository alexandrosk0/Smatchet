#!/usr/bin/env bash
# test-agent-contract.sh — verify agent prompts conform to AGENTS.md § Agent output contract.
#
# Covers:
#   1. Every Implementer agent has 3 required headings (Files changed / Smoke-test result / Manual residue).
#   2. Every Maintenance agent has 4 required headings (Pre-flight / Mutations applied / Regression gate / Residue requiring user action).
#   3. Every Diagnostic-read-edit agent (debug-detective) has 6 required headings.
#   4. Every agent prompt contains the `## Outcome:` mandate text.
#   5. Banner identity line matches frontmatter: `🤖 AGENT: <name> · <model>/<effort> · <read-only|read-edit>` (name + model/effort + read-scope; version via check 10).
#   6. "$(agent_path architect)" does NOT contain a `git commit` self-directive.
#   7. docs/agent-rules/delegation.md § Agent output contract has 6 class rows (post-H9 Design class added).
#   7b. architect.md emits the Design-class sections (Goal / Affected components / Interface contracts / Risks / Implementation handoff).
#   8. agents/_shared/token-tracking/tests/test_infer_outcome.py passes (Python unit cases for the telemetry classifier).
#   9. agent-token-log.py canonical and .claude/hooks/ copy are byte-identical (link_file drift check per process.md 2026-05-19 P3).
#  10. Frontmatter `version: N` matches banner `· vN` (H7 fix from eval doc).
#  11. Skill ↔ agent SKILL.md sibling parity — same version + same triggers (eval punch-list item 7 + M9).
#  12. V3.3 — Source/Core/src/P4Annotate.cpp has exactly one SubprocessCapture::Run call site.
#  13. V10.1 — agents/core/debug-detective.md contains literal "reproducer-first contract" (slice 10).
#  14. Every agent prompt references the `## Self-improvement` output-contract section
#      (mention counts — not a literal trailing heading; agent-size-reduction.md § Deviations).
#  15. Top-level `model:` frontmatter key exists and equals harness-hints.claude-code.model
#      (the Claude Code native model-routing wire; delegation.md § Model tiering).
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.

set -euo pipefail

command -v python >/dev/null 2>&1 || { echo "python required" >&2; exit 2; }

cd "$(git rev-parse --show-toplevel)"

PASS=0
FAIL=0
FAILED_CHECKS=()

check_pass() { PASS=$((PASS+1)); printf '  PASS  %s\n' "$1"; }
check_fail() { FAIL=$((FAIL+1)); FAILED_CHECKS+=("$1"); printf '  FAIL  %s\n' "$1"; }

# Agents live under agents/{core,project}/ (the portable/project split) — resolve
# by name so this contract is location-agnostic. Excludes agents/_shared/.
agent_path() {
  find agents -path 'agents/_shared/*' -prune -o -name "$1.md" -print 2>/dev/null | head -1
}
# All agent prompt files (both tiers, excluding _shared + the root README).
agent_files() {
  find agents/core agents/project -maxdepth 1 -name '*.md' 2>/dev/null | sort
}

# -------------------------------------------------------------------------
# 1. Implementer agents — 3 required headings.
# -------------------------------------------------------------------------
echo "[1/15] Implementer required headings (## Files changed / ## Smoke-test result / ## Manual residue)"
IMPLEMENTERS=(tracker-backend grid-engine offline-sync command-system lua-binder mcp-toolsmith p4-annotate unreal-bridge mechanic ui-host)
for a in "${IMPLEMENTERS[@]}"; do
  f="$(agent_path "$a")"
  miss=0
  for h in "## Files changed" "## Smoke-test result" "## Manual residue"; do
    if ! grep -qF "$h" "$f"; then miss=$((miss+1)); fi
  done
  if [[ $miss -eq 0 ]]; then check_pass "$a"; else check_fail "$a missing $miss/3 Implementer headings"; fi
done

# -------------------------------------------------------------------------
# 2. Maintenance agents — 4 required headings.
# -------------------------------------------------------------------------
echo
echo "[2/15] Maintenance required headings (## Pre-flight / ## Mutations applied / ## Regression gate / ## Residue requiring user action)"
MAINTENANCE=(build-doctor test-author git-janitor p4-janitor)
for a in "${MAINTENANCE[@]}"; do
  f="$(agent_path "$a")"
  miss=0
  for h in "## Pre-flight" "## Mutations applied" "## Regression gate" "## Residue requiring user action"; do
    if ! grep -qF "$h" "$f"; then miss=$((miss+1)); fi
  done
  if [[ $miss -eq 0 ]]; then check_pass "$a"; else check_fail "$a missing $miss/4 Maintenance headings"; fi
done

# -------------------------------------------------------------------------
# 3. Diagnostic read-edit (debug-detective) — 6 required report-shape headings.
# The report-shape templates were extracted to the debug-instrument skill
# (reduce-agent-prompt-bloat Slice 2), so the headings may live in the agent
# OR in its delegated skill. The invariant is "the diagnostic report shape is
# declared in a discoverable place the agent points to" — satisfied by either.
# -------------------------------------------------------------------------
echo
echo "[3/15] Diagnostic read-edit required headings (debug-detective + debug-instrument skill)"
DD_REQUIRED=("## Hypotheses" "## Evidence" "## Cause" "## Files changed (temp-debug)" "## Cleanup" "## Handoff")
DD_AGENT="$(agent_path debug-detective)"
DD_SKILL="agents/_shared/skills/debug-instrument/SKILL.md"
miss=0
for h in "${DD_REQUIRED[@]}"; do
  if ! grep -qF "$h" "$DD_AGENT" && ! { [[ -f "$DD_SKILL" ]] && grep -qF "$h" "$DD_SKILL"; }; then
    miss=$((miss+1))
    echo "    missing (agent + debug-instrument skill): $h"
  fi
done
if [[ $miss -eq 0 ]]; then check_pass "debug-detective 6/6"; else check_fail "debug-detective missing $miss/6 Diagnostic headings"; fi

# -------------------------------------------------------------------------
# 4. Every agent has ## Outcome: mandate text.
# -------------------------------------------------------------------------
echo
echo "[4/15] ## Outcome: mandate present in every agent prompt (README excluded)"
miss_outcome=()
total_agents=0
for f in $(agent_files); do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  total_agents=$((total_agents + 1))
  if ! grep -qF "## Outcome:" "$f"; then miss_outcome+=("$base"); fi
done
if [[ ${#miss_outcome[@]} -eq 0 ]]; then
  check_pass "$total_agents/$total_agents agents have ## Outcome: mandate"
else
  for m in "${miss_outcome[@]}"; do echo "    missing: $m"; done
  check_fail "${#miss_outcome[@]} agents missing ## Outcome: mandate"
fi

# -------------------------------------------------------------------------
# 5. Banner identity line matches frontmatter: every agent must open with
#    `🤖 AGENT: <name> · <model>/<effort> · <read-only|read-edit> · v<N>` so the
#    name + model + effort + write-scope it ran under is printed at the top of
#    its output. Asserts name, model/effort, AND the read-only token (the
#    `read-write` perf-gatekeeper drift slipped past the old model/effort-only
#    substring check). Version (`· vN`) is asserted by check 10.
# -------------------------------------------------------------------------
echo
echo "[5/15] Banner ↔ frontmatter identity (name · model/effort · read-scope)"
banner_mismatch=()
for f in $(agent_files); do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  fm_model=$(awk '/^harness-hints:/,/^---/' "$f" | grep -E "^    model:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  fm_effort=$(awk '/^harness-hints:/,/^---/' "$f" | grep -E "^    effort:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  [[ -z "$fm_model" || -z "$fm_effort" ]] && continue  # Agent has no claude-code hint; skip.
  fm_name=$(grep -m1 -E "^name:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  fm_ro=$(grep -m1 -E "^read-only:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  if [[ "$fm_ro" == "true" ]]; then ro_tok="read-only"; else ro_tok="read-edit"; fi
  # Anchor to the OPENING banner (`🤖 AGENT: …`) — the identity line printed at
  # the top of the agent's output — so a one-sided drift in only the open banner
  # is still caught (the close `✅ END —` half shares the line).
  expected="🤖 AGENT: ${fm_name} · ${fm_model}/${fm_effort} · ${ro_tok}"
  banner=$(grep -m1 -F '**Banner**' "$f" || true)
  if ! echo "$banner" | grep -qF "$expected"; then
    banner_mismatch+=("$base: expected '$expected'; banner: $banner")
  fi
done
if [[ ${#banner_mismatch[@]} -eq 0 ]]; then
  check_pass "all banners match frontmatter (name · model/effort · read-scope)"
else
  for m in "${banner_mismatch[@]}"; do echo "    $m"; done
  check_fail "${#banner_mismatch[@]} banner mismatches"
fi

# -------------------------------------------------------------------------
# 6. "$(agent_path architect)" does NOT contain a git commit self-directive.
# -------------------------------------------------------------------------
echo
echo "[6/15] architect.md emit-only (no self-commit directive)"
if grep -qE '^[^>`]*Commit immediately with ' "$(agent_path architect)"; then
  check_fail "architect.md still instructs the agent to commit"
else
  check_pass "architect.md is emit-only"
fi

# -------------------------------------------------------------------------
# 7. AGENTS.md § Agent output contract has 6 class rows (Design class added
#    per H9 from docs/reference/agentic-infrastructure-2026-05-23.md).
# -------------------------------------------------------------------------
echo
echo "[7/15] docs/agent-rules/delegation.md output-contract table has 6 class rows"
# Table lives in docs/agent-rules/delegation.md § Agent output contract since
# AGENTS.md L192-422 was extracted into the new file. AGENTS.md still carries
# a redirect stub naming the subsection.
class_rows=$(awk '/^## Agent output contract/,/^All six classes also end/' docs/agent-rules/delegation.md | grep -cE '^\| \*\*(Investigator|Design|Diagnostic|Implementer|Helper|Maintenance)\b')
if [[ "$class_rows" -eq 6 ]]; then
  check_pass "6 class rows present"
else
  check_fail "expected 6 class rows in delegation.md, found $class_rows"
fi

# H9 extension: also verify the architect agent has the Design class's 5
# required headings. architect's prompt has historically emitted Goal /
# Affected components / Interface contracts / Risks / Implementation handoff
# as numbered list items (`1. **Goal** — ...`) rather than `## Goal`
# section headings; the check matches both shapes so the existing prompt
# passes without a rewrite. A future architect.md rewrite that uses literal
# `## Goal` headings will also pass.
echo
echo "[7b/15] architect.md emits Design-class sections (Goal / Affected components / Interface contracts / Risks / Implementation handoff)"
design_miss=0
for h in "Goal" "Affected components" "Interface contracts" "Risks" "Implementation handoff"; do
  if ! grep -qE "^(## $h|[0-9]+\. \*\*$h\*\*)" "$(agent_path architect)"; then
    design_miss=$((design_miss+1))
    echo "    missing: $h"
  fi
done
if [[ $design_miss -eq 0 ]]; then
  check_pass "architect.md 5/5 Design sections"
else
  check_fail "architect.md missing $design_miss/5 Design sections"
fi

# -------------------------------------------------------------------------
# 8. _infer_outcome unit tests pass.
# -------------------------------------------------------------------------
echo
echo "[8/15] agents/_shared/token-tracking/tests/test_infer_outcome.py"
if python agents/_shared/token-tracking/tests/test_infer_outcome.py; then
  check_pass "_infer_outcome unit tests"
else
  check_fail "_infer_outcome unit tests"
fi

# -------------------------------------------------------------------------
# 9. agent-token-log.py canonical vs .claude/hooks/ copy drift check.
# -------------------------------------------------------------------------
# Per process.md 2026-05-19 orchestrator P3 — `link_file()` in
# agents/scripts/core/setup-harness.sh short-circuits when destination exists, so an
# independent copy at `.claude/hooks/agent-token-log.py` (different inode,
# possibly stale) can silently diverge from the canonical at
# `agents/_shared/token-tracking/agent-token-log.py`. Edits to the canonical
# don't propagate; the live Claude Code SubagentStop hook runs the stale copy.
# Catch the drift at PR time before the misclassification ships.
echo
echo "[9/15] agent-token-log.py — canonical vs .claude/hooks/ copy drift"
canonical=agents/_shared/token-tracking/agent-token-log.py
hook_copy=.claude/hooks/agent-token-log.py
if [[ ! -f "$hook_copy" ]]; then
  # Hook copy absent — Claude Code harness not set up locally. Skip cleanly.
  check_pass "drift check skipped (hook copy absent — setup-harness.sh not run)"
elif cmp -s "$canonical" "$hook_copy"; then
  check_pass "canonical and hook copy byte-identical"
else
  check_fail "DRIFT: $canonical vs $hook_copy differ — run \`cp -f $canonical $hook_copy\` or \`bash agents/scripts/core/setup-harness.sh claude-code\` (after \`rm $hook_copy\`)"
fi

# -------------------------------------------------------------------------
# 10. Frontmatter `version: N` matches banner `· vN`.
# -------------------------------------------------------------------------
# H7 from docs/reference/agentic-infrastructure-2026-05-23.md: eight agents
# had `version: 1` in frontmatter but `· v2` in their banner; telemetry
# pivots on frontmatter so v2 work was undercounted. PR #421 fixed those
# eight. This check prevents future drift by asserting the two are always
# kept in lockstep (the agent-versioning rule in
# docs/agent-rules/delegation.md § Agent versioning).
echo
echo "[10/15] Frontmatter version ↔ banner version match"
version_mismatch=()
for f in $(agent_files); do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  fm_version=$(awk '/^---$/{p=!p;next}p' "$f" | grep -E "^version:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  [[ -z "$fm_version" ]] && continue  # No frontmatter version — skip.
  banner=$(grep -m1 -F '**Banner**' "$f" || true)
  # Banner shape: "... · v<N>`. Close (before ..." — find the `· v<N>` token.
  if ! printf '%s' "$banner" | grep -qE "· v${fm_version}([\` ]|$)"; then
    version_mismatch+=("$base: frontmatter v$fm_version not found in banner line")
  fi
done
if [[ ${#version_mismatch[@]} -eq 0 ]]; then
  check_pass "all frontmatter versions match banner"
else
  for m in "${version_mismatch[@]}"; do echo "    $m"; done
  check_fail "${#version_mismatch[@]} frontmatter↔banner version mismatches"
fi

# -------------------------------------------------------------------------
# 11. Skill ↔ agent sibling parity (eval punch-list item 7).
# -------------------------------------------------------------------------
# Three agents (perf-instrument / perf-measure / perf-gatekeeper) live as
# both `agents/<name>.md` AND `agents/_shared/skills/<name>/SKILL.md`. With
# no parity gate, edits to one form silently diverged from the other. This
# check enforces: when a sibling pair exists, version + triggers must match.
# Skills with no sibling agent (`grill-with-docs`, `scratchpad-recall`)
# still require their own `version:` field for telemetry parity (eval M9).
echo
echo "[11/15] Skill ↔ agent SKILL.md parity (version + triggers)"
skill_drift=()
for skill_md in agents/_shared/skills/*/SKILL.md; do
  skill_name=$(basename "$(dirname "$skill_md")")
  # `|| true`: under `set -euo pipefail` a SKILL.md with no `version:` line makes
  # grep exit 1, which (via pipefail) aborts the whole script BEFORE the `-z`
  # guard below can report it as drift. Tolerate the empty match so the guard runs.
  skill_ver=$(awk '/^---$/{p=!p;next}p' "$skill_md" | grep -E "^version:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]' || true)
  if [[ -z "$skill_ver" ]]; then
    skill_drift+=("$skill_name SKILL.md: missing frontmatter version: field (telemetry parity, eval M9)")
    continue
  fi
  agent_md="agents/$skill_name.md"
  if [[ ! -f "$agent_md" ]]; then
    continue  # Skill-only (no sibling agent) — version-presence check already passed.
  fi
  agent_ver=$(awk '/^---$/{p=!p;next}p' "$agent_md" | grep -E "^version:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]' || true)  # || true: same set -e/pipefail guard as skill_ver above
  if [[ "$skill_ver" != "$agent_ver" ]]; then
    skill_drift+=("$skill_name: SKILL.md v=$skill_ver vs agents/$skill_name.md v=$agent_ver")
  fi
  # Trigger parity — extract the list of triggers from each. Use sorted
  # comparison so order-of-triggers in the YAML doesn't trip the check.
  skill_triggers=$(awk '/^---$/{p=!p;next}p' "$skill_md" | awk '/^triggers:/{f=1;next} f && /^[a-zA-Z]/{f=0} f{print $0}' | sort)
  agent_triggers=$(awk '/^---$/{p=!p;next}p' "$agent_md" | awk '/^triggers:/{f=1;next} f && /^[a-zA-Z]/{f=0} f{print $0}' | sort)
  if [[ "$skill_triggers" != "$agent_triggers" ]]; then
    skill_drift+=("$skill_name: SKILL.md triggers differ from agents/$skill_name.md triggers")
  fi
done
if [[ ${#skill_drift[@]} -eq 0 ]]; then
  check_pass "all skill / agent sibling pairs in parity"
else
  for d in "${skill_drift[@]}"; do echo "    $d"; done
  check_fail "${#skill_drift[@]} skill↔agent parity violations"
fi

# -------------------------------------------------------------------------
# 12. V3.3 (slice 3 of autonomous-debugging-no-creds) — P4Annotate.cpp keeps
#     exactly one `SubprocessCapture::Run` call site. Any future second spawn
#     must come with a sibling `cfg.P4RunOverride` consult or this gate trips.
# -------------------------------------------------------------------------
echo
echo "[12/15] V3.3 — Source/Core/src/P4Annotate.cpp has exactly one SubprocessCapture::Run call site"
p4annotate_src=Source/Core/src/P4Annotate.cpp
if [[ ! -f "$p4annotate_src" ]]; then
  check_fail "V3.3: $p4annotate_src missing"
else
  run_count=$(grep -cE 'SubprocessCapture::Run\(' "$p4annotate_src" || true)
  override_count=$(grep -cE 'cfg\.P4RunOverride' "$p4annotate_src" || true)
  if [[ "$run_count" -eq 1 && "$override_count" -ge 1 ]]; then
    check_pass "V3.3: exactly 1 SubprocessCapture::Run + $override_count P4RunOverride consult(s)"
  else
    check_fail "V3.3: expected 1 SubprocessCapture::Run + >=1 P4RunOverride consult; found run=$run_count override=$override_count"
  fi
fi

# -------------------------------------------------------------------------
# 13. V10.1 (slice 10 of autonomous-debugging-no-creds) — debug-detective.md
#     contains the literal phrase "reproducer-first contract" so future doc
#     rewrites don't silently soften the contract. Same shape as check 7b.
# -------------------------------------------------------------------------
echo
echo "[13/15] V10.1 — agents/core/debug-detective.md contains literal 'reproducer-first contract'"
if grep -qF "reproducer-first contract" "$(agent_path debug-detective)"; then
  check_pass "V10.1: 'reproducer-first contract' phrase present"
else
  check_fail "V10.1: 'reproducer-first contract' phrase missing from debug-detective.md"
fi

# -------------------------------------------------------------------------
# 14. Every agent prompt references the `## Self-improvement` output-contract
#     section (the trailing-section convention). Same grep shape as check 4
#     (## Outcome:) — machine-enforces that no agent silently drops the
#     self-improvement contract. A mention (inline in the output-contract line
#     OR a literal heading) counts; we deliberately do NOT require a literal
#     trailing heading — that would re-bloat the agents the size-reduction
#     campaign shrank, for zero detectability gain (agent-size-reduction.md
#     § Deviations). README excluded, like check 4.
# -------------------------------------------------------------------------
echo
echo "[14/15] ## Self-improvement contract referenced in every agent prompt (README excluded)"
miss_selfimp=()
total_si=0
for f in $(agent_files); do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  total_si=$((total_si + 1))
  if ! grep -qF "## Self-improvement" "$f"; then miss_selfimp+=("$base"); fi
done
if [[ ${#miss_selfimp[@]} -eq 0 ]]; then
  check_pass "$total_si/$total_si agents reference ## Self-improvement"
else
  for m in "${miss_selfimp[@]}"; do echo "    missing: $m"; done
  check_fail "${#miss_selfimp[@]} agents missing ## Self-improvement reference"
fi

# -------------------------------------------------------------------------
# 15. Top-level `model:` key ↔ harness-hints.claude-code.model parity.
#     The top-level key is the Claude Code wire (native subagent model
#     routing); the harness-hints value feeds pi/codex + the banner (check 5).
#     Both must exist and agree — delegation.md § Model tiering is the change
#     procedure (edit both keys + banner together).
# -------------------------------------------------------------------------
echo
echo "[15/15] Top-level model: ↔ harness-hints.claude-code.model parity"
model_mismatch=()
for f in $(agent_files); do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  hint_model=$(awk '/^harness-hints:/,/^---/' "$f" | grep -E "^    model:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  [[ -z "$hint_model" ]] && continue  # Agent has no claude-code hint; skip.
  top_model=$(grep -m1 -E "^model:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  if [[ -z "$top_model" ]]; then
    model_mismatch+=("$base: has harness-hints model '$hint_model' but no top-level model: key")
  elif [[ "$top_model" != "$hint_model" ]]; then
    model_mismatch+=("$base: top-level model '$top_model' != harness-hints model '$hint_model'")
  fi
done
if [[ ${#model_mismatch[@]} -eq 0 ]]; then
  check_pass "all top-level model: keys match harness-hints.claude-code.model"
else
  for m in "${model_mismatch[@]}"; do echo "    $m"; done
  check_fail "${#model_mismatch[@]} model-key mismatches"
fi

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
echo
echo "----------------------------------------"
echo "test-agent-contract — Passed: $PASS  Failed: $FAIL"
if [[ $FAIL -gt 0 ]]; then
  echo
  echo "Failures:"
  for c in "${FAILED_CHECKS[@]}"; do echo "  - $c"; done
  exit 1
fi
exit 0
