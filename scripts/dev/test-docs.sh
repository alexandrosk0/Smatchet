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
#   2 — cannot resolve repo root, or the layer/host step split (HOST_ONLY_STEPS)
#       no longer adds up to STEPS.

set -euo pipefail

# Resolve repo root so the script runs from anywhere.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || { echo "test-docs: cannot cd to repo root" >&2; exit 2; }

CORE="agents/scripts/core"

# md_lint is pure in-tree Python; the mirror needs the same interpreter CI has.
# Resolve AND run each candidate: on Windows `python3` is the Microsoft Store App
# Execution Alias stub — it satisfies `command -v` but prints "Python was not
# found; run without arguments to install..." and exits non-zero, so a
# resolve-only guard passes and md_lint then fails with that banner instead of a
# usable message.
PY=""
for _c in python3 python py; do
  if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then PY="$_c"; break; fi
done
[ -n "$PY" ] || {
  echo "test-docs: no working python3 on PATH (md_lint step needs it)" >&2; exit 2; }

# The standalone agent-layer probe (plan agent-surface-extraction-repo row 9b) —
# see HOST_ONLY_STEPS below. Source/ under the host tree is the same probe
# test-all.sh's LAYER_HOST_SUT_RE and test-agent-contract.sh's host_content_present() use.
HOST_TREE="${PROJECT_ROOT:-$ROOT}"
LAYER_STANDALONE=0
if [ ! -d "$HOST_TREE/Source" ]; then
  LAYER_STANDALONE=1
fi

# test-markdown-links diff-scopes against origin/develop in the host, which
# grandfathers pre-existing breakage. A standalone layer scans --all against its
# own committed docs/high-integrity/markdown-link-baseline.md instead: on a seed
# (the simulator's throwaway origin, a new repo's first push) origin/develop IS
# the tree, so a diff-scoped scan checks nothing — and --all needs no history.
MDLINKS_SCOPE=""
if [ "$LAYER_STANDALONE" -eq 1 ]; then
  MDLINKS_SCOPE=" --all"
fi

# Ordered to match doc-validation.yml. project.config.json schema validation is
# the workflow's one Python-jsonschema step; replicate it inline so a malformed
# config is caught locally too.
STEPS=(
  "test-doc-anchors|bash $CORE/test-doc-anchors.sh"
  "test-agent-contract|bash $CORE/test-agent-contract.sh"
  "test-plan-index|bash $CORE/test-plan-index.sh"
  "test-plan-ref-integrity|bash $CORE/test-plan-ref-integrity.sh"
  "test-plan-claim-anchors|bash $CORE/test-plan-claim-anchors.sh --selftest && bash $CORE/test-plan-claim-anchors.sh --all"
  "check-pr-intent-sync|bash $CORE/check-pr-intent.sh --check-workflow-sync"
  "test-plan-naming|bash $CORE/test-plan-naming.sh"
  "md_lint|$PY $CORE/md_lint.py --selftest && $PY $CORE/md_lint.py --all"
  "test-portable-purity|bash $CORE/test-portable-purity.sh"
  "test-config-globs|bash $CORE/test-config-globs.sh --selftest && bash $CORE/test-config-globs.sh --check"
  "test-gate-selftests|bash $CORE/test-gate-selftests.sh --selftest && bash $CORE/test-gate-selftests.sh --check"
  "test-oob-label-impl|bash $CORE/test-oob-label-impl.sh --selftest && bash $CORE/test-oob-label-impl.sh"
  "test-required-context-adr-consistency|bash $CORE/test-required-context-adr-consistency.sh --selftest && bash $CORE/test-required-context-adr-consistency.sh --check"
  "test-portable-agent-vexp|bash $CORE/test-portable-agent-vexp.sh --selftest && bash $CORE/test-portable-agent-vexp.sh"
  "test-agent-discovery-fixture|bash $CORE/test-agent-discovery-fixture.sh"
  "test-agent-build-facts|bash $CORE/test-agent-build-facts.sh"
  "test-markdown-links|bash $CORE/test-markdown-links.sh$MDLINKS_SCOPE"
  "test-orphan-bats|bash $CORE/test-orphan-bats.sh --selftest && bash $CORE/test-orphan-bats.sh"
  "work_item_lint|$PY $CORE/work_item_lint.py --selftest && $PY $CORE/work_item_lint.py --all && bash scripts/dev/test-work-item-lint.sh"
)

# Standalone agent-layer subset (plan agent-surface-extraction-repo row 9b). This
# script is mirrored byte-for-byte into the agent-layer repo, where there is no
# consuming product: the steps below read product content (plans, presets, work
# items, ADRs, the product's CI workflows) and would hard-fail on its absence, so
# there they print an explicit SKIP. Every other step runs in both trees (the link
# check at --all scope there; see MDLINKS_SCOPE). LAYER_STANDALONE is set above.
declare -A HOST_ONLY_STEPS=(
  [test-plan-index]="reads docs/plans/"
  [test-plan-ref-integrity]="reads docs/plans/"
  [test-plan-claim-anchors]="reads docs/plans/"
  [test-plan-naming]="reads docs/plans/"
  [check-pr-intent-sync]="diffs a verdict regex against the product's .github/workflows/"
  [test-config-globs]="every project.config.json glob must match tracked product files"
  [test-required-context-adr-consistency]="reads docs/adr/ and docs/plans/shipped/"
  [test-agent-build-facts]="resolves the product's CMakePresets.json"
  [work_item_lint]="reads docs/work/"
)
# Steps that run in the standalone layer. A new STEPS entry breaks this count on
# purpose: classify it — layer-runnable (bump this) or host-only (add it above).
LAYER_STEP_COUNT=10

declare -A STEP_NAMES=()
for entry in "${STEPS[@]}"; do STEP_NAMES["${entry%%|*}"]=1; done
for name in "${!HOST_ONLY_STEPS[@]}"; do
  [ -n "${STEP_NAMES[$name]:-}" ] || {
    echo "test-docs: HOST_ONLY_STEPS names '$name', which is not a STEPS entry — fix the split" >&2; exit 2; }
done
if [ "${#STEPS[@]}" -ne $((LAYER_STEP_COUNT + ${#HOST_ONLY_STEPS[@]})) ]; then
  echo "test-docs: ${#STEPS[@]} steps != $LAYER_STEP_COUNT layer + ${#HOST_ONLY_STEPS[@]} host-only." >&2
  echo "  Classify the new step: layer-runnable (bump LAYER_STEP_COUNT) or host-only (HOST_ONLY_STEPS)." >&2
  exit 2
fi

if [ "$LAYER_STANDALONE" -eq 1 ]; then
  printf 'test-docs: no Source/ under %s — standalone agent layer: running the %d layer steps, skipping %d host-only.\n' \
    "$HOST_TREE" "$LAYER_STEP_COUNT" "${#HOST_ONLY_STEPS[@]}"
fi

declare -a FAILED=()
pass_count=0
skip_count=0

for entry in "${STEPS[@]}"; do
  name="${entry%%|*}"
  cmd="${entry#*|}"
  printf '\n=== %s ===\n' "$name"
  if [ "$LAYER_STANDALONE" -eq 1 ] && [ -n "${HOST_ONLY_STEPS[$name]:-}" ]; then
    printf 'SKIP (standalone agent layer): %s\n' "${HOST_ONLY_STEPS[$name]}"
    skip_count=$((skip_count + 1))
    continue
  fi
  if eval "$cmd"; then
    pass_count=$((pass_count + 1))
  else
    FAILED+=("$name")
  fi
done

printf '\n----------------------------------------\n'
printf 'test-docs — Passed: %d  Failed: %d  Skipped: %d\n' "$pass_count" "${#FAILED[@]}" "$skip_count"

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
