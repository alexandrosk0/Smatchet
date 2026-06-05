#!/usr/bin/env bash
# test-agent-build-facts.sh — verify build-related FACTS in agent prompts match
# ground truth, so an agent never instructs a user to run a preset that does not
# exist or cites a retired toolchain. Both classes shipped real defects the eval
# caught (build-doctor cited ninja-debug-msvc-{tsan,msan}; test-author cited
# ninja-asan; code-review / coderabbit-triage cited "MinGW UCRT").
#
# Checks:
#   1. Every `ninja-*` preset token in agents/**/*.md resolves to a real
#      `"name"` in CMakePresets.json.
#   2. No retired-build-toolchain phrase ("MinGW UCRT") in agents/**/*.md.
#      (MSYS2-as-lint-tooling / MinGW-GCC-sanitizer-runtime mentions stay
#      allowed — only the retired C++ *build-target* phrasing is banned.)
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PRESETS_JSON="CMakePresets.json"

PASS=0
FAIL=0
FAILED_CHECKS=()
check_pass() { PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"; }
check_fail() { FAIL=$((FAIL + 1)); FAILED_CHECKS+=("$1"); printf '  FAIL  %s\n' "$1"; }

# All agent prompt + skill markdown (excludes the root README).
agent_md() {
  find agents -name '*.md' ! -name 'README.md' 2>/dev/null | sort
}

# -------------------------------------------------------------------------
# 1. ninja-* preset tokens resolve to a real CMakePresets.json preset name.
# -------------------------------------------------------------------------
echo "[1/2] ninja-* preset tokens in agents/**/*.md resolve to CMakePresets.json"

# Valid preset names (the ninja-* ones; hidden _smatchet-* presets never match
# the ninja- prefix so they cannot collide).
valid_presets=" $(grep -oE '"name"[[:space:]]*:[[:space:]]*"ninja-[a-z0-9-]+"' "$PRESETS_JSON" 2>/dev/null \
  | sed -E 's/.*"(ninja-[a-z0-9-]+)".*/\1/' | sort -u | tr '\n' ' ') "

if [[ "$valid_presets" == "  " || -z "${valid_presets// /}" ]]; then
  check_fail "could not read any ninja-* preset from $PRESETS_JSON"
else
  dangling=0
  while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    while IFS= read -r tok; do
      [[ -z "$tok" ]] && continue
      # Whole-word membership test against the valid set.
      if [[ "$valid_presets" != *" $tok "* ]]; then
        echo "    $f: unknown preset '$tok'"
        dangling=$((dangling + 1))
      fi
    done < <(grep -oE 'ninja-[a-z0-9-]+' "$f" 2>/dev/null | sort -u || true)
  done < <(agent_md)
  if [[ $dangling -eq 0 ]]; then
    check_pass "all ninja-* tokens resolve to a real preset"
  else
    check_fail "$dangling dangling ninja-* preset reference(s) in agent prompts"
  fi
fi

# -------------------------------------------------------------------------
# 2. No retired "MinGW UCRT" build-target citation in agent prompts.
# -------------------------------------------------------------------------
echo
echo "[2/2] No retired-build-toolchain phrase 'MinGW UCRT' in agents/**/*.md"
retired_hits="$(grep -rIlF 'MinGW UCRT' agents --include='*.md' 2>/dev/null || true)"
if [[ -z "$retired_hits" ]]; then
  check_pass "no 'MinGW UCRT' build-target citation (target is MSVC + Clang)"
else
  while IFS= read -r h; do [[ -n "$h" ]] && echo "    $h"; done <<< "$retired_hits"
  check_fail "retired 'MinGW UCRT' citation present — target is MSVC + Clang"
fi

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
echo
echo "----------------------------------------"
echo "test-agent-build-facts — Passed: $PASS  Failed: $FAIL"
if [[ $FAIL -gt 0 ]]; then
  echo
  echo "Failures:"
  for c in "${FAILED_CHECKS[@]}"; do echo "  - $c"; done
  exit 1
fi
exit 0
