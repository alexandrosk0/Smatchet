#!/usr/bin/env bash
# test-subsystem-docs.sh — coverage + staleness gate for per-subsystem agent docs.
# ----------------------------------------------------------------------------
# Per-subsystem leaf docs live at Source/Core/src/<ctx>/{AGENTS,CONTEXT,README}.md
# and are registered in the root CONTEXT-MAP.md. This gate keeps the registry,
# the on-disk leaves, and the central code-review.md checklist in sync.
#
# FAIL (exit 1) on any structural breakage:
#   1. A leaf listed in CONTEXT-MAP.md is missing on disk.
#   2. An on-disk Source/Core/src/<ctx>/AGENTS.md is absent from CONTEXT-MAP.md.
#   3. A leaf invariant bullet is duplicated verbatim in agents/core/code-review.md
#      (central<->leaf drift — the rule must live in ONE place: the leaf).
#
# WARN (never changes exit code), only in --diff mode:
#   4. A context's .cpp/.h changed vs <ref> but its README.md did not (stale orientation).
#
# Modes:
#   (no args)          structural checks 1-3 + staleness WARN vs origin/develop when that
#                      ref resolves (degrades to structural-only if it doesn't).
#   --diff[=]<ref>     structural checks 1-3 + staleness WARN vs <ref> (e.g. origin/develop).
#   --selftest         assert the registry parses and matches on-disk leaves; checks 1-2 only.
#
# Project-bound (hardcodes Source/Core/src paths) — lives in agents/scripts/project/,
# not core/, per the portable/project split. Goes through test-shell-lint.sh (5 rules).
# ----------------------------------------------------------------------------
set -uo pipefail

cd "$(dirname "$0")/../../.."

# MAP/SRC_GLOB are read-only override seams (no command-from-env injection) so
# --selftest can point check 1 (registered-leaf-missing-on-disk) at a synthetic
# temp registry and assert the REAL detection path FAILS on a known-bad fixture.
# Defaults are unchanged in every real invocation (no env set) — a test seam, not
# a logic change. SUBSYS_DOCS_NEGCHECK guards against selftest re-entrancy.
MAP="${SUBSYS_DOCS_MAP:-CONTEXT-MAP.md}"
SRC_GLOB="${SUBSYS_DOCS_SRC_GLOB:-Source/Core/src}"
CENTRAL="agents/core/code-review.md"

# Default (no args, as test-all.sh invokes it): structural checks + staleness vs
# origin/develop when that ref resolves. --diff names an explicit ref; --selftest
# is structure-only (checks 1-2). Staleness is always WARN — never flips exit.
MODE="diff"
DIFF_REF="origin/develop"
case "${1:-}" in
  --diff=*)   DIFF_REF="${1#--diff=}" ;;
  --diff)     DIFF_REF="${2:-origin/develop}" ;;
  --selftest) MODE="selftest" ;;
  "")         ;;
  *) echo "usage: test-subsystem-docs.sh [--diff[=]<ref> | --selftest]" >&2; exit 2 ;;
esac
# Staleness needs a resolvable base ref; degrade to structural-only if absent
# (shallow CI clone, detached HEAD) rather than erroring.
if [[ "$MODE" == "diff" ]] && ! git rev-parse --verify --quiet "$DIFF_REF" >/dev/null 2>&1; then
  MODE="structural"
fi

# --- selftest negative fixture (Gap C / Slice 3) -----------------------------
# selftest: asserts-failure — feed a synthetic registry that names a leaf NOT on
# disk and assert the REAL check-1 path returns non-zero. Re-enters this same
# script (so it exercises production logic, not a copy) with the MAP/SRC_GLOB
# seams pointed at a temp tree; SUBSYS_DOCS_NEGCHECK breaks the recursion.
if [[ "$MODE" == "selftest" && "${SUBSYS_DOCS_NEGCHECK:-0}" != "1" ]]; then
  _neg_tmp="$(mktemp -d)" || { echo "test-subsystem-docs selftest: mktemp failed" >&2; exit 1; }
  trap 'rm -rf "$_neg_tmp"' EXIT
  # A registry section that registers a leaf which does not exist on disk.
  cat > "$_neg_tmp/MAP.md" <<EOF
## Registry
- \`$_neg_tmp/src/ghost/AGENTS.md\` — a deliberately absent leaf (selftest fixture).
## Next section
EOF
  mkdir -p "$_neg_tmp/src"   # SRC_GLOB root exists but holds no */AGENTS.md
  if SUBSYS_DOCS_NEGCHECK=1 SUBSYS_DOCS_MAP="$_neg_tmp/MAP.md" \
       SUBSYS_DOCS_SRC_GLOB="$_neg_tmp/src" bash "$0" --selftest >/dev/null 2>&1; then
    echo "test-subsystem-docs selftest: FAIL — a registry naming a missing-on-disk leaf was NOT detected" >&2
    exit 1
  fi
  rm -rf "$_neg_tmp"; trap - EXIT
fi

fail=0
warn=0

if [[ ! -f "$MAP" ]]; then
  echo "test-subsystem-docs: FAIL — registry $MAP not found" >&2
  exit 1
fi

# --- Parse the registry: every backtick-wrapped Source/Core/src/<ctx>/<X>.md path
# inside the `## Registry` section ONLY (awk scopes to it, so format-contract examples
# and the growth-path `<ctx>` placeholder elsewhere in the doc don't get parsed as real
# leaves). The `<` filter is a belt-and-braces guard against any placeholder path.
# `sort -u` dedups; these are the leaves the registry claims exist.
registered="$(awk '/^## Registry/{f=1; next} /^## /{f=0} f' "$MAP" \
  | grep -oE '`'"$SRC_GLOB"'/[^`]+\.md`' | tr -d '`' | grep -v '<' | sort -u)"

# --- Check 1: every registered leaf exists on disk.
while IFS= read -r leaf; do
  [[ -z "$leaf" ]] && continue
  if [[ ! -f "$leaf" ]]; then
    echo "test-subsystem-docs: FAIL — registered leaf missing on disk: $leaf" >&2
    fail=1
  fi
done <<< "$registered"

# --- Check 2: every on-disk leaf AGENTS.md is registered.
# git ls-files = tracked files only (matches the repo's tracked-vs-ignored discipline).
ondisk_agents="$(git ls-files "$SRC_GLOB/*/AGENTS.md" 2>/dev/null | sort -u)"
while IFS= read -r leaf; do
  [[ -z "$leaf" ]] && continue
  if ! grep -qF "$leaf" <<< "$registered"; then
    echo "test-subsystem-docs: FAIL — on-disk leaf not in $MAP registry: $leaf" >&2
    echo "    add a row for it, or remove the file." >&2
    fail=1
  fi
done <<< "$ondisk_agents"

# --- Check 3: no leaf invariant bullet duplicated verbatim in the central checklist.
# Normalise a line to plain lowercase text (strip markdown emphasis/backticks/list
# marker, collapse whitespace) so a copy-paste is caught regardless of formatting.
# Skip --selftest (structure-only).
_normalize() {
  sed -E 's/[`*_]//g; s/^[[:space:]]*-[[:space:]]+//; s/[[:space:]]+/ /g; s/^ //; s/ $//' \
    | tr '[:upper:]' '[:lower:]'
}
if [[ "$MODE" != "selftest" && -f "$CENTRAL" ]]; then
  central_norm="$(_normalize < "$CENTRAL")"
  while IFS= read -r leaf; do
    [[ -z "$leaf" || ! -f "$leaf" ]] && continue
    # Only bullet lines inside the leaf that read as rules (start with "- "), long
    # enough that a match is meaningful (>= 60 normalised chars rules out short shared phrases).
    while IFS= read -r bullet; do
      norm="$(printf '%s' "$bullet" | _normalize)"
      [[ "${#norm}" -lt 60 ]] && continue
      if grep -qF "$norm" <<< "$central_norm"; then
        echo "test-subsystem-docs: FAIL — leaf rule duplicated in $CENTRAL:" >&2
        echo "    leaf: $leaf" >&2
        echo "    rule: ${norm:0:80}..." >&2
        echo "    keep the rule in the leaf only; the central file points, it does not restate." >&2
        fail=1
      fi
    done < <(grep -E '^- ' "$leaf")
  done <<< "$ondisk_agents"
fi

# --- Check 4 (diff mode only): README staleness WARN.
if [[ "$MODE" == "diff" ]]; then
  changed="$(git diff --name-only "$DIFF_REF"...HEAD 2>/dev/null || git diff --name-only "$DIFF_REF" 2>/dev/null || true)"
  # Discover every context dir that owns a README.md (data-driven — picks up new ones).
  while IFS= read -r readme; do
    [[ -z "$readme" ]] && continue
    ctx_dir="$(dirname "$readme")"
    code_touched="$(grep -E "^${ctx_dir}/.*\.(cpp|h)$" <<< "$changed" || true)"
    readme_touched="$(grep -Fx "$readme" <<< "$changed" || true)"
    if [[ -n "$code_touched" && -z "$readme_touched" ]]; then
      echo "test-subsystem-docs: WARN — $ctx_dir code changed but $readme untouched (orientation may be stale)." >&2
      echo "    review $readme; if still accurate, touch it or add PR label 'subsystem-docs-out-of-band'." >&2
      warn=1
    fi
  done < <(git ls-files "$SRC_GLOB/*/README.md" 2>/dev/null | sort -u)
fi

if [[ "$fail" -ne 0 ]]; then
  echo "test-subsystem-docs: FAILED" >&2
  # Summary line consumed by scripts/dev/test-all.sh (regex-extracted).
  echo "Passed: 0  Failed: 1"
  exit 1
fi
if [[ "$warn" -ne 0 ]]; then
  echo "test-subsystem-docs: passed with staleness warnings (non-blocking)." >&2
else
  echo "test-subsystem-docs: OK — registry, leaves, and central checklist in sync." >&2
fi
# Staleness is WARN-only — a warning still reports Failed: 0 so it never blocks the gate.
echo "Passed: 1  Failed: 0"
exit 0
