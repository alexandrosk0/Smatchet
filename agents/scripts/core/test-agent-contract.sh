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
#
# Modes:
#   (no args) | --check   run all 15 checks against the tree.
#   --selftest            dogfood the locale-safe matcher (see grep_fixed below).

set -euo pipefail

# -------------------------------------------------------------------------
# grep_fixed <pattern> — byte-wise fixed-string match against stdin.
# -------------------------------------------------------------------------
# GNU grep 3.0 — the build Git for Windows ships — fails a `-F` (and `-E`)
# match when the PATTERN contains a 4-byte UTF-8 character (`🤖` U+1F916)
# under a multibyte locale (LANG=en_US.UTF-8). Check 5 interpolates the robot
# emoji into its expected banner, so every agent read as a banner mismatch for
# Windows devs while CI (Ubuntu, newer grep) passed — a false negative, not
# drift. `LC_ALL=C` forces the byte-wise compare a fixed-string match wants
# anyway, so both environments return the same verdict.
# Only emoji-bearing PATTERNS are affected: an ASCII pattern still matches a
# line that contains emoji, so the other `grep -qF` sites here are fine.
grep_fixed() { LC_ALL=C grep -qF -- "$1"; }

# claude_code_hint <file> <key> — read `harness-hints.claude-code.<key>`.
#
# Scoped to the `claude-code:` SUB-BLOCK, not to `harness-hints:` as a whole.
# The old form (`awk '/^harness-hints:/,/^---/' | grep '^    model:'`) took the
# FIRST 4-space-indented key under harness-hints regardless of which harness owns
# it. `.coderabbit.yaml` documents `harness-hints.<harness>:` blocks (e.g.
# `harness-hints.claude:`) as intentional, so the moment any agent lists a second
# harness BEFORE `claude-code:`, checks 5 and 15 silently compare the wrong
# harness's value — a false PASS or a false FAIL with no way to tell which.
# The awk range also ran to EOF when the frontmatter terminator was missing.
#
# Defined ABOVE the --selftest dispatch on purpose: bash executes function
# definitions in order, so a helper defined further down does not exist yet when
# `--selftest` runs and exits. (Caught by the fixtures below on first run.)
claude_code_hint() {  # <file> <key>
  awk -v k="    $2:" '
    # FRONTMATTER-SCOPED. Ending the block only on a following 2-space key (the
    # first cut) leaked: neither a 0-space top-level key (`version:`) nor the
    # closing `---` cleared inblk, so when the sought key was ABSENT the scan ran
    # on into the document body and picked up any 4-space-indented `model:` in an
    # indented example — inventing a value for a partial block and defeating the
    # very partial-block path checks 5 and 15 exist to exercise. Reproduced:
    # a partial block + `version: 3` + a body line `    model: sonnet` returned
    # "sonnet". (Cursor Bugbot on PR #2150; fixture (g) below pins it.)
    NR == 1 && /^---[[:space:]]*$/ { fm = 1; next }
    !fm { exit }                                  # no frontmatter -> no hints
    fm && /^---[[:space:]]*$/ { exit }            # closing --- ends the search
    /^  claude-code:/ { inblk = 1; next }
    # Any NON-BLANK line that is not one of the block s 4-space children ends the
    # block. Covers 2-space siblings, 0-space top-level keys, and `---` alike.
    inblk && !/^    / && /[^[:space:]]/ { inblk = 0 }
    inblk && index($0, k) == 1 { sub(/^[^:]*: */, ""); gsub(/[[:space:]]/, ""); print; exit }
  ' "$1"
}

# has_claude_code_block <file> — true when the agent declares a claude-code hint
# block at all. This is the ONLY legitimate reason to skip the model checks; a
# block that exists but is partial is a finding, not a skip.
has_claude_code_block() {  # <file>
  # Frontmatter-scoped for the same reason as claude_code_hint: a whole-file grep
  # would count a `  claude-code:` line quoted in the agent s prose as a real
  # hint block, turning a legitimate no-block skip into a spurious finding.
  awk '
    NR == 1 && /^---[[:space:]]*$/ { fm = 1; next }
    !fm { exit 1 }
    fm && /^---[[:space:]]*$/ { exit 1 }
    /^  claude-code:/ { exit 0 }
  ' "$1"
}

# model_verdict <file> — print the check-15 mismatch reason, or nothing when the
# agent is compliant (or legitimately exempt). Extracted from the check-15 loop
# so the DECISION is testable, not just the extractors it calls: the fail-open
# this closes lived in the branching, so fixtures over `claude_code_hint` alone
# would leave the actual defect unproven.
model_verdict() {  # <file>
  local f="$1" base hint top
  base=$(basename "$f")
  hint=$(claude_code_hint "$f" model)
  top=$(grep -m1 -E "^model:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  if ! has_claude_code_block "$f"; then
    # No hint block is the ONLY legitimate skip — and only when a top-level key
    # still pins the model. With neither, the agent silently inherits the
    # session model, which is the regression this check exists to prevent.
    [[ -n "$top" ]] && return 0
    printf '%s: no model: key and no claude-code hint block — agent will inherit the session model\n' "$base"
    return 0
  fi
  if [[ -z "$hint" && -z "$top" ]]; then
    printf '%s: claude-code block present but neither model: key set — agent will inherit the session model\n' "$base"
  elif [[ -z "$hint" ]]; then
    printf '%s: has top-level model '\''%s'\'' but no harness-hints.claude-code.model\n' "$base" "$top"
  elif [[ -z "$top" ]]; then
    printf '%s: has harness-hints model '\''%s'\'' but no top-level model: key\n' "$base" "$hint"
  elif [[ "$top" != "$hint" ]]; then
    printf '%s: top-level model '\''%s'\'' != harness-hints model '\''%s'\''\n' "$base" "$top" "$hint"
  fi
  return 0
}

# selftest: asserts-failure — the negative case below feeds a drifted banner and
# requires grep_fixed to REJECT it, so a helper stubbed to `return 0` cannot pass.
run_selftest() {
  local rc=0
  local fixture='**Banner** — open with: `🤖 AGENT: architect · opus/high · read-only · v2`'
  # Positive: a 4-byte-UTF-8 pattern must match under a multibyte locale. The
  # explicit UTF-8 export is the regression trigger — drop grep_fixed's LC_ALL=C
  # and this case fails on GNU grep 3.0 (Git-Bash) exactly as check 5 did.
  if ! ( export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
         printf '%s\n' "$fixture" | grep_fixed '🤖 AGENT: architect · opus/high · read-only' ); then
    echo "selftest: FAIL — 4-byte-UTF-8 pattern did not match (grep locale regression)" >&2
    rc=1
  fi
  # Negative: real drift (model changed) must still be reported as a mismatch.
  if ( export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
       printf '%s\n' "$fixture" | grep_fixed '🤖 AGENT: architect · sonnet/high · read-only' ); then
    echo "selftest: FAIL — a drifted banner was accepted as a match" >&2
    rc=1
  fi
  # --- model-key extraction + fail-open fixtures (finding #1881) -------------
  # The real tree has 0 violations (all 26 agents carry both keys), so nothing
  # in-repo exercises these paths. Without fixtures the fix is unproven and a
  # regression would be invisible — the same "verified nothing, reported PASS"
  # shape the fix exists to close.
  local tmpd; tmpd="$(mktemp -d)"

  # (a) Multi-harness ordering: a NON-claude-code harness listed FIRST must not
  #     be the value compared. This is what the old harness-hints-wide awk range
  #     got wrong.
  cat > "$tmpd/multi.md" <<'FIX'
---
name: multi
model: opus
harness-hints:
  claude:
    model: sonnet
    effort: low
  claude-code:
    model: opus
    effort: high
---
FIX
  if [[ "$(claude_code_hint "$tmpd/multi.md" model)" != "opus" ]]; then
    echo "selftest: FAIL — claude_code_hint read the wrong harness's model (got '$(claude_code_hint "$tmpd/multi.md" model)', want 'opus')" >&2
    rc=1
  fi
  if [[ "$(claude_code_hint "$tmpd/multi.md" effort)" != "high" ]]; then
    echo "selftest: FAIL — claude_code_hint read the wrong harness's effort" >&2
    rc=1
  fi

  # The cases below drive model_verdict() — the DECISION — not just its
  # extractors, because the fail-open lived in the branching.
  #
  # selftest: asserts-failure — every `want-report` case below must produce a
  # non-empty verdict; a model_verdict stubbed to `return 0` fails all four.
  vcase() {  # <fixture-file> <want: report|silent> <label>
    local got; got="$(model_verdict "$1")"
    if [[ "$2" == "report" && -z "$got" ]]; then
      echo "selftest: FAIL — $3: expected a mismatch report, got silence (fail-open)" >&2; rc=1
    elif [[ "$2" == "silent" && -n "$got" ]]; then
      echo "selftest: FAIL — $3: expected no finding, got '$got'" >&2; rc=1
    fi
  }

  # (b) finding #1881's motivating case: NO model key anywhere. The old
  #     `[[ -z "$hint_model" ]] && continue` waved this through, so the agent
  #     silently inherited the session model.
  printf -- '---\nname: bare\n---\n' > "$tmpd/bare.md"
  vcase "$tmpd/bare.md" report "no model key at all"

  # (c) claude-code block present but partial — a finding, not a skip.
  printf -- '---\nname: partial\nharness-hints:\n  claude-code:\n    effort: high\n---\n' > "$tmpd/partial.md"
  vcase "$tmpd/partial.md" report "partial claude-code block"

  # (d) genuine drift between the two keys must still be caught (the check's
  #     original job — the fix must not trade one blind spot for another).
  printf -- '---\nname: drift\nmodel: opus\nharness-hints:\n  claude-code:\n    model: sonnet\n---\n' > "$tmpd/drift.md"
  vcase "$tmpd/drift.md" report "top-level vs hint drift"

  # (e) multi-harness fixture from (a) is COMPLIANT — the wrong-harness read
  #     would make it look like drift, so this pins the false-positive side.
  vcase "$tmpd/multi.md" silent "multi-harness, keys agree"

  # (f) legitimate exemption: no hint block, but a top-level key still pins the
  #     model. Must stay silent, or the check becomes noise on valid agents.
  printf -- '---\nname: toponly\nmodel: opus\n---\n' > "$tmpd/toponly.md"
  vcase "$tmpd/toponly.md" silent "top-level only, no hint block"

  # (g) selftest: asserts-failure — REGRESSION for the frontmatter leak (Cursor
  #     Bugbot, PR #2150). A PARTIAL claude-code block, then a 0-space top-level
  #     key and the closing `---`, then a 4-space `model:` in the document body.
  #     The first cut of claude_code_hint ended the block only on a 2-space key,
  #     so neither `version:` nor `---` cleared it and the scan reached the body
  #     and returned "sonnet" — inventing a model for a block that declares none.
  #     Fixture (c) missed this because it had no body after the frontmatter.
  cat > "$tmpd/leak.md" <<'LEAKFIX'
---
name: leak
harness-hints:
  claude-code:
    effort: high
version: 3
---

Example config:

    model: sonnet
LEAKFIX
  if [[ -n "$(claude_code_hint "$tmpd/leak.md" model)" ]]; then
    echo "selftest: FAIL — claude_code_hint leaked past the frontmatter and read '$(claude_code_hint "$tmpd/leak.md" model)' from the body" >&2
    rc=1
  fi
  vcase "$tmpd/leak.md" report "partial block with a body model: line"

  # (h) prose mentioning a hint block must NOT count as declaring one, or a
  #     legitimate no-block skip becomes a spurious finding.
  printf -- '---\nname: prose\nmodel: opus\n---\n\nAgents declare:\n\n  claude-code:\n    model: sonnet\n' > "$tmpd/prose.md"
  if has_claude_code_block "$tmpd/prose.md"; then
    echo "selftest: FAIL — has_claude_code_block counted a block quoted in prose" >&2
    rc=1
  fi
  vcase "$tmpd/prose.md" silent "prose mentioning claude-code, real top-level model"
  rm -rf "$tmpd"

  if [[ $rc -eq 0 ]]; then echo "test-agent-contract --selftest: PASS"; fi
  return "$rc"
}

case "${1:---check}" in
  --check)    : ;;
  --selftest) run_selftest; exit $? ;;
  -h|--help)  echo "usage: test-agent-contract.sh [--check|--selftest]"; exit 0 ;;
  *)          echo "usage: test-agent-contract.sh [--check|--selftest]" >&2; exit 2 ;;
esac

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python required (no working interpreter on PATH)" >&2; exit 2; }

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
  fm_model=$(claude_code_hint "$f" model)
  fm_effort=$(claude_code_hint "$f" effort)
  # Skip only when there is genuinely no claude-code hint block. A block that
  # exists but is partial falls through to the banner comparison, where the
  # empty field shows up as a visible mismatch instead of a silent skip
  # (check 15 reports the missing key itself).
  has_claude_code_block "$f" || continue
  fm_name=$(grep -m1 -E "^name:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  fm_ro=$(grep -m1 -E "^read-only:" "$f" | awk -F': *' '{print $2}' | tr -d '[:space:]')
  if [[ "$fm_ro" == "true" ]]; then ro_tok="read-only"; else ro_tok="read-edit"; fi
  # Anchor to the OPENING banner (`🤖 AGENT: …`) — the identity line printed at
  # the top of the agent's output — so a one-sided drift in only the open banner
  # is still caught (the close `✅ END —` half shares the line).
  expected="🤖 AGENT: ${fm_name} · ${fm_model}/${fm_effort} · ${ro_tok}"
  banner=$(grep -m1 -F '**Banner**' "$f" || true)
  # grep_fixed (not a bare `grep -qF`): the 4-byte 🤖 in $expected breaks fixed-
  # string matching on GNU grep 3.0 under a UTF-8 locale — see its definition.
  if ! printf '%s\n' "$banner" | grep_fixed "$expected"; then
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
if "$PY" agents/_shared/token-tracking/tests/test_infer_outcome.py; then
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
  verdict=$(model_verdict "$f")
  [[ -n "$verdict" ]] && model_mismatch+=("$verdict")
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
