#!/usr/bin/env bash
# Fixture: rule 1 deps preflight — graceful-degradation shapes (KNOWN-GOOD).
#
# Every allowlisted tool below is used WITHOUT a `command -v` preflight, yet the
# gate must NOT fire SHELL_LINT_DEPS: each use self-handles a missing/failed
# tool inline. Locks in the rule-1 relaxation (follow-up to PR #624) so it can't
# silently regress into firing on these graceful patterns again. Mirrors the
# real shapes in lock-claim / setup-harness / clear-session-context.
set -euo pipefail

# Shape A — same-line trailing `|| <fallback>` chain (lock-script pattern):
# python3, then python, then a literal — each tool's failure handled inline.
# The `-c` arg carries a `;` INSIDE the quotes (as the real lock scripts do);
# the trailing-|| span must stay quote-aware and not stop at that semicolon.
prefix=$(python3 -c 'import sys;sys.stdout.write("x")' 2>/dev/null || python -c 'import sys;sys.stdout.write("x")' 2>/dev/null || printf 'fallback')
echo "$prefix"

# Shape A — single trailing fallback (setup-harness cygpath pattern).
winpath=$(cygpath -w "$PWD" 2>/dev/null || echo "$PWD")
echo "$winpath"

# Shape B — if CONDITION is itself the availability test (session-context p4).
if info=$(p4 info 2>/dev/null); then
    echo "$info"
fi

# Shape B — while CONDITION availability/retry test.
tries=0
while ! jq --version >/dev/null 2>&1 && [ "$tries" -lt 1 ]; do
    tries=$((tries + 1))
done
echo "$tries"
