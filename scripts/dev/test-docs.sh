#!/bin/bash
# test-docs.sh — local mirror of the .github/workflows/doc-validation.yml gate.
#
# WHY THIS EXISTS
#   Pure-docs slices (docs/**, AGENTS.md, uppercase root *.md) skip both
#   `cmake --build` and `scripts/dev/test-all.sh` per
#   docs/agent-rules/process-rules.md § Pure-docs slice skip — there is no
#   executable code to verify. But those same paths are EXACTLY what the
#   "Doc validation" CI workflow gates (anchor resolution, agent-contract
#   parity, shipped-plan-index sync, plan-ref integrity, kebab-case naming,
#   portable purity, agent discovery). Result: the one change-class that
#   triggers doc-validation in CI was the one class with no local gate —
#   docs PRs went red on push (e.g. a stale docs/plans/INDEX.md row from an
#   upstream plan-move surfacing only when a later docs PR runs the workflow).
#
#   This script is the local equivalent. Run it on every pure-docs / docs-
#   touching slice BEFORE push. Cheap (<2s, no compile). Steps mirror
#   .github/workflows/doc-validation.yml job "Doc anchors + agent contract"
#   1:1 — keep the two in sync when either changes.
#
# USAGE
#   bash scripts/dev/test-docs.sh
#
# EXIT
#   0 — every step passed.
#   1 — at least one step failed (names printed in the summary).
#   2 — cannot resolve repo root.

set -euo pipefail

# Resolve repo root so the script runs from anywhere.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || { echo "test-docs: cannot cd to repo root" >&2; exit 2; }

CORE="agents/scripts/core"

# Ordered to match doc-validation.yml. project.config.json schema validation is
# the workflow's one Python-jsonschema step; replicate it inline so a malformed
# config is caught locally too.
STEPS=(
  "test-doc-anchors|bash $CORE/test-doc-anchors.sh"
  "test-agent-contract|bash $CORE/test-agent-contract.sh"
  "test-plan-index|bash $CORE/test-plan-index.sh"
  "test-plan-ref-integrity|bash $CORE/test-plan-ref-integrity.sh"
  "test-plan-naming|bash $CORE/test-plan-naming.sh"
  "test-portable-purity|bash $CORE/test-portable-purity.sh"
  "test-config-globs|bash $CORE/test-config-globs.sh --selftest && bash $CORE/test-config-globs.sh --check"
  "test-gate-selftests|bash $CORE/test-gate-selftests.sh --selftest && bash $CORE/test-gate-selftests.sh --check"
  "test-oob-label-impl|bash $CORE/test-oob-label-impl.sh --selftest && bash $CORE/test-oob-label-impl.sh"
  "test-required-context-adr-consistency|bash $CORE/test-required-context-adr-consistency.sh --selftest && bash $CORE/test-required-context-adr-consistency.sh --check"
  "test-portable-agent-vexp|bash $CORE/test-portable-agent-vexp.sh --selftest && bash $CORE/test-portable-agent-vexp.sh"
  "test-agent-discovery-fixture|bash $CORE/test-agent-discovery-fixture.sh"
  "test-agent-build-facts|bash $CORE/test-agent-build-facts.sh"
  "test-markdown-links|bash $CORE/test-markdown-links.sh"
  "test-orphan-bats|bash $CORE/test-orphan-bats.sh --selftest && bash $CORE/test-orphan-bats.sh"
)

declare -a FAILED=()
pass_count=0

for entry in "${STEPS[@]}"; do
  name="${entry%%|*}"
  cmd="${entry#*|}"
  printf '\n=== %s ===\n' "$name"
  if eval "$cmd"; then
    pass_count=$((pass_count + 1))
  else
    FAILED+=("$name")
  fi
done

printf '\n----------------------------------------\n'
printf 'test-docs — Passed: %d  Failed: %d\n' "$pass_count" "${#FAILED[@]}"

if [ "${#FAILED[@]}" -gt 0 ]; then
  printf 'Failures:\n'
  for f in "${FAILED[@]}"; do
    printf '  - %s\n' "$f"
  done
  printf '\nFix locally (many auto-fix): e.g. "bash %s/test-plan-index.sh --fix".\n' "$CORE"
  exit 1
fi

printf 'PASS — local doc-validation mirror clean.\n'
exit 0
