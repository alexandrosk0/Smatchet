#!/usr/bin/env bash
# test-lint-bash.sh — shellcheck gate for scripts/dev/ + agents/scripts/{core,project}.
#
# Per `docs/reference/agentic-infrastructure-2026-05-23.md` punch-list item 5
# (bash-lint sweep). Encodes the failing-rule policy from the eval doc:
# fail on the three checks that have historically masked real bugs
# (SC2086 unquoted vars / SC2046 unquoted command substitution / SC2155
# declare-and-assign masks exit code), warn on everything else at the
# `--severity=warning` level.
#
# Bucket A (CLI) per AGENTS.md § Verification automation. Zero manual steps.
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Exit codes follow the test-author convention:
#   0 — clean (no findings under the fail-set)
#   1 — at least one SC2086 / SC2046 / SC2155 finding (FAIL)
#   2 — shellcheck binary missing (skip; not a fail)

set -euo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

if ! command -v shellcheck >/dev/null 2>&1; then
    cat >&2 <<'EOF'
lint-bash: shellcheck not on PATH.
  Install: npm i -g shellcheck   (preferred — resolves on the bash-tool PATH)
           pacman -S shellcheck  (MSYS2)
           brew install shellcheck (macOS)
  See BUILD.md § Dev-script CLI tools.
EOF
    echo "Passed: 0  Failed: 0  (skipped — shellcheck missing)"
    exit 2
fi

# Fail-set — these three checks have specific incident history in this repo:
#   SC2086 — Unquoted variable. Word-splitting silently injects spurious
#            arguments; the same anti-pattern surfaces in 5+ scripts per the
#            eval doc § P3 (Bash defensive-programming inconsistency).
#   SC2046 — Unquoted command substitution. Same word-splitting risk as
#            SC2086 but harder to spot because the source is dynamic.
#   SC2155 — Declare-and-assign on one line (`local var=$(cmd)` or
#            `export var=$(cmd)`). `local`/`export` swallows the exit code
#            of the RHS, so a failing `cmd` silently sets an empty var
#            instead of erroring the caller. Caught test-config-migration.sh
#            on the first sweep.
#
# Additional checks reported at WARN level only (not failing) so the operator
# sees the residue without the gate going red over style-level findings.
FAIL_RULES="SC2086,SC2046,SC2155"

# Targets — build/infra scripts in scripts/dev/ plus the agentic scripts that
# #609 relocated to agents/scripts/{core,project}/ (previously unscanned). Flat
# globs (non-recursive), matching the historical scripts/dev/*.sh behaviour.
# nullglob: an unmatched glob expands to nothing rather than the literal pattern
# (which would otherwise reach shellcheck as a bogus filename).
shopt -s nullglob
TARGETS=(scripts/dev/*.sh agents/scripts/core/*.sh agents/scripts/project/*.sh)
shopt -u nullglob

# Phase 1 — strict pass on the fail-set. Non-zero exit means real findings.
strict_out=$(shellcheck --include="$FAIL_RULES" --severity=warning "${TARGETS[@]}" 2>&1 || true)
strict_findings=$(printf '%s\n' "$strict_out" | grep -cE '^In .* line [0-9]+' || true)

# Phase 2 — informational pass at WARN severity (every check, fail-set
# included). The diff between strict-findings count and full-warn count
# tells the operator how much non-blocking residue exists.
warn_out=$(shellcheck --severity=warning "${TARGETS[@]}" 2>&1 || true)
warn_findings=$(printf '%s\n' "$warn_out" | grep -cE '^In .* line [0-9]+' || true)

if [ "$strict_findings" -gt 0 ]; then
    echo "$strict_out"
    echo
    echo "Passed: 0  Failed: ${strict_findings}"
    echo "FAIL — ${strict_findings} finding(s) under the fail-set ($FAIL_RULES)"
    echo "       additional ${warn_findings} total at --severity=warning (informational)"
    exit 1
fi

# Clean on the fail-set. Surface the WARN-severity residue as a one-line
# advisory so the eval doc's punch-list residue stays visible.
if [ "$warn_findings" -gt 0 ]; then
    echo "Passed: 1  Failed: 0  (${warn_findings} non-blocking finding(s) at --severity=warning; not in fail-set $FAIL_RULES)"
else
    echo "Passed: 1  Failed: 0"
fi
exit 0
