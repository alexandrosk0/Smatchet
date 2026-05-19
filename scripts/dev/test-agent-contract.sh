#!/usr/bin/env bash
# test-agent-contract.sh — verify agent prompts conform to AGENTS.md § Agent output contract.
#
# Covers:
#   1. Every Implementer agent has 3 required headings (Files changed / Smoke-test result / Manual residue).
#   2. Every Maintenance agent has 4 required headings (Pre-flight / Mutations applied / Regression gate / Residue requiring user action).
#   3. Every Diagnostic-read-edit agent (debug-detective) has 6 required headings.
#   4. Every agent prompt contains the `## Outcome:` mandate text.
#   5. Banner `model/effort` substring matches frontmatter `harness-hints.claude-code.{model,effort}` byte-for-byte.
#   6. agents/architect.md does NOT contain a `git commit` self-directive.
#   7. AGENTS.md § Agent output contract has 5 class rows (the post-PR split).
#   8. agents/_shared/token-tracking/tests/test_infer_outcome.py passes (Python unit cases for the telemetry classifier).
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PASS=0
FAIL=0
FAILED_CHECKS=()

check_pass() { PASS=$((PASS+1)); printf '  PASS  %s\n' "$1"; }
check_fail() { FAIL=$((FAIL+1)); FAILED_CHECKS+=("$1"); printf '  FAIL  %s\n' "$1"; }

# -------------------------------------------------------------------------
# 1. Implementer agents — 3 required headings.
# -------------------------------------------------------------------------
echo "[1/8] Implementer required headings (## Files changed / ## Smoke-test result / ## Manual residue)"
IMPLEMENTERS=(tracker-backend grid-engine offline-sync command-system lua-binder mcp-toolsmith p4-blame unreal-bridge mechanic)
for a in "${IMPLEMENTERS[@]}"; do
  f="agents/$a.md"
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
echo "[2/8] Maintenance required headings (## Pre-flight / ## Mutations applied / ## Regression gate / ## Residue requiring user action)"
MAINTENANCE=(build-doctor test-author git-janitor)
for a in "${MAINTENANCE[@]}"; do
  f="agents/$a.md"
  miss=0
  for h in "## Pre-flight" "## Mutations applied" "## Regression gate" "## Residue requiring user action"; do
    if ! grep -qF "$h" "$f"; then miss=$((miss+1)); fi
  done
  if [[ $miss -eq 0 ]]; then check_pass "$a"; else check_fail "$a missing $miss/4 Maintenance headings"; fi
done

# -------------------------------------------------------------------------
# 3. Diagnostic read-edit (debug-detective) — 6 required headings.
# -------------------------------------------------------------------------
echo
echo "[3/8] Diagnostic read-edit required headings (debug-detective)"
DD_REQUIRED=("## Hypotheses" "## Evidence" "## Cause" "## Files changed (temp-debug)" "## Cleanup" "## Handoff")
miss=0
for h in "${DD_REQUIRED[@]}"; do
  if ! grep -qF "$h" agents/debug-detective.md; then
    miss=$((miss+1))
    echo "    missing: $h"
  fi
done
if [[ $miss -eq 0 ]]; then check_pass "debug-detective 6/6"; else check_fail "debug-detective missing $miss/6 Diagnostic headings"; fi

# -------------------------------------------------------------------------
# 4. Every agent has ## Outcome: mandate text.
# -------------------------------------------------------------------------
echo
echo "[4/8] ## Outcome: mandate present in every agent prompt (24 files, README excluded)"
miss_outcome=()
for f in agents/*.md; do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  if ! grep -qF "## Outcome:" "$f"; then miss_outcome+=("$base"); fi
done
if [[ ${#miss_outcome[@]} -eq 0 ]]; then
  check_pass "24/24 agents have ## Outcome: mandate"
else
  for m in "${miss_outcome[@]}"; do echo "    missing: $m"; done
  check_fail "${#miss_outcome[@]} agents missing ## Outcome: mandate"
fi

# -------------------------------------------------------------------------
# 5. Banner model/effort substring matches frontmatter harness-hints.claude-code.{model,effort}.
# -------------------------------------------------------------------------
echo
echo "[5/8] Banner ↔ frontmatter model/effort match"
banner_mismatch=()
for f in agents/*.md; do
  base=$(basename "$f")
  [[ "$base" == "README.md" ]] && continue
  fm_model=$(awk '/^harness-hints:/,/^---/' "$f" | grep -E "^    model:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  fm_effort=$(awk '/^harness-hints:/,/^---/' "$f" | grep -E "^    effort:" | head -1 | awk -F': *' '{print $2}' | tr -d '[:space:]')
  [[ -z "$fm_model" || -z "$fm_effort" ]] && continue  # Agent has no claude-code hint; skip.
  expected="$fm_model/$fm_effort"
  banner=$(grep -m1 -F '**Banner**' "$f" || true)
  if ! echo "$banner" | grep -qF "$expected"; then
    banner_mismatch+=("$base: expected $expected; banner: $banner")
  fi
done
if [[ ${#banner_mismatch[@]} -eq 0 ]]; then
  check_pass "all banners match frontmatter"
else
  for m in "${banner_mismatch[@]}"; do echo "    $m"; done
  check_fail "${#banner_mismatch[@]} banner mismatches"
fi

# -------------------------------------------------------------------------
# 6. agents/architect.md does NOT contain a git commit self-directive.
# -------------------------------------------------------------------------
echo
echo "[6/8] architect.md emit-only (no self-commit directive)"
if grep -qE '^[^>`]*Commit immediately with ' agents/architect.md; then
  check_fail "architect.md still instructs the agent to commit"
else
  check_pass "architect.md is emit-only"
fi

# -------------------------------------------------------------------------
# 7. AGENTS.md § Agent output contract has 5 class rows.
# -------------------------------------------------------------------------
echo
echo "[7/8] docs/agent-rules/DELEGATION.md output-contract table has 5 class rows"
# Table lives in docs/agent-rules/DELEGATION.md § Agent output contract since
# AGENTS.md L192-422 was extracted into the new file. AGENTS.md still carries
# a redirect stub naming the subsection.
class_rows=$(awk '/^## Agent output contract/,/^All five classes also end/' docs/agent-rules/DELEGATION.md | grep -cE '^\| \*\*(Investigator|Diagnostic|Implementer|Helper|Maintenance)\b')
if [[ "$class_rows" -eq 5 ]]; then
  check_pass "5 class rows present"
else
  check_fail "expected 5 class rows in DELEGATION.md, found $class_rows"
fi

# -------------------------------------------------------------------------
# 8. _infer_outcome unit tests pass.
# -------------------------------------------------------------------------
echo
echo "[8/8] agents/_shared/token-tracking/tests/test_infer_outcome.py"
if python agents/_shared/token-tracking/tests/test_infer_outcome.py; then
  check_pass "_infer_outcome unit tests"
else
  check_fail "_infer_outcome unit tests"
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
