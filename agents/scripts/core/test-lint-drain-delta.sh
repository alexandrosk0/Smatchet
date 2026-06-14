#!/usr/bin/env bash
# test-lint-drain-delta.sh — unit-test the lint-drain delta-filter
# (lint_changed_lines + lint_filter_delta in
# docs/harness/claude-code/hooks/lint-cpp-common.sh). The Stop-hook deferred-lint
# drain must emit ONLY findings inside the session's changed-vs-baseline lines,
# grandfathering pre-existing code exactly like the merge-gate delta lint — so it
# stops re-flagging untouched functions or a post-branch-switch working tree
# (categories/tooling.md 2026-06-02 / 2026-06-05 lint-cpp-drain delta-awareness).
#
# Self-contained: sources the canonical lib against a synthetic git repo + feeds
# synthetic cppcheck-template findings (no cppcheck / clang-tidy needed).
# Auto-enrolled by scripts/dev/test-all.sh.

set -u

HERE="$(cd "$(dirname "$0")/../../.." && pwd)"
LIB="$HERE/docs/harness/claude-code/hooks/lint-cpp-common.sh"

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
nope() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

(
  cd "$tmp" || exit 1
  git init -q
  git config core.autocrlf false
  git config user.email t@example.com
  git config user.name tester
  printf 'a\nb\nc\nd\ne\n' > f.cpp
  git add f.cpp
  git commit -qm base
  git branch develop
  # Session change: modify line 3 (c -> CHANGED) and append a new line 6.
  printf 'a\nb\nCHANGED\nd\ne\nNEW\n' > f.cpp
)

export CLAUDE_PROJECT_DIR="$tmp"
export LINT_DELTA_BASE=develop
# shellcheck source=docs/harness/claude-code/hooks/lint-cpp-common.sh
source "$LIB"

abs="$LINT_NORM_PROJ/f.cpp"

# Synthetic findings: line 3 (changed), line 4 (pre-existing/untouched), line 6 (added).
findings="$(printf 'cppcheck: %s:3: [style] x: changed line\ncppcheck: %s:4: [style] y: pre-existing line\ncppcheck: %s:6: [warning] z: added line\n' "$abs" "$abs" "$abs")"

# 1. changed lines resolve to exactly {3,6}
got="$(lint_changed_lines "$abs" | sort -n | tr '\n' ' ')"
if [ "$got" = "3 6 " ]; then ok "lint_changed_lines -> {3,6}"; else nope "lint_changed_lines: want '3 6 ' got '$got'"; fi

# 2. filter keeps the changed-line findings, drops the pre-existing one
out="$(printf '%s\n' "$findings" | lint_filter_delta "$abs")"
if printf '%s' "$out" | grep -q ':3:' && printf '%s' "$out" | grep -q ':6:' \
   && ! printf '%s' "$out" | grep -q ':4:'; then
    ok "lint_filter_delta keeps {3,6}, drops pre-existing 4"
else
    nope "lint_filter_delta wrong set (got: $(printf '%s' "$out" | tr '\n' '|'))"
fi

# 3. fail OPEN when the baseline is unresolvable (non-git checkout / bad ref)
out_open="$(LINT_DELTA_BASE='no-such-ref-xyzzy' bash -c '
    export CLAUDE_PROJECT_DIR="'"$tmp"'"; source "'"$LIB"'"
    printf '"'"'%s\n'"'"' "'"$findings"'" | lint_filter_delta "'"$abs"'"' 2>/dev/null)"
if printf '%s' "$out_open" | grep -q ':4:'; then
    ok "fail-open passes ALL findings when baseline missing"
else
    nope "fail-open dropped findings (got: $(printf '%s' "$out_open" | tr '\n' '|'))"
fi

# 4. unchanged file (baseline resolves, no diff) grandfathers everything -> empty
(cd "$tmp" && git checkout -q develop -- f.cpp)   # revert to baseline
out_clean="$(printf '%s\n' "$findings" | lint_filter_delta "$abs")"
if [ -z "$out_clean" ]; then
    ok "unchanged-vs-baseline file emits no findings (all grandfathered)"
else
    nope "unchanged file still emitted (got: $(printf '%s' "$out_clean" | tr '\n' '|'))"
fi

echo "[lint-drain-delta] PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
