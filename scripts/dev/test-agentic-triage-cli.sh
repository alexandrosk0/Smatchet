#!/usr/bin/env bash
# test-agentic-triage-cli.sh — exercise agent.triage.run argument parsing +
# JSON envelope shape WITHOUT hitting GitHub. Anti-deception posture per
# orchestrator: never embed a real PAT in CI. The handler is exercised up to
# the GitHubClient layer; we assert the command is registered + the args
# parse + the response is valid JSON with the expected top-level keys.
#
# Auto-enrolled by test-all.sh via the test-*.sh glob.
#
# Exit codes:
#   0 — registration + arg-parse + JSON envelope shape OK
#       OR command not registered (AGENTIC=OFF) — clean skip
#   1 — registered but envelope shape is wrong / command crashed
#   2 — exe not found at any known preset location

set -euo pipefail

cd "$(dirname "$0")/../.."

EXE="${SMATCHET_EXE:-./build/ninja-iter-msys2/Smatchet.exe}"
if [ ! -x "$EXE" ]; then
    for alt in \
        ./build/ninja-publish-msys2/Smatchet.exe \
        ./build/ninja-debug-msys2/Smatchet.exe \
        ./build/ninja-test-msys2/Smatchet.exe ; do
        if [ -x "$alt" ]; then
            EXE="$alt"
            break
        fi
    done
fi
if [ ! -x "$EXE" ]; then
    echo "test-agentic-triage-cli: Smatchet.exe not built at any common preset"
    exit 2
fi

# Probe registration. If the command is not registered the build is AGENTIC=OFF;
# clean skip rather than fail (the OFF build still registers a stub via the
# BuiltinCommands_Agentic.cpp #else branch, so this should always match — but
# the runtime check protects against build-time surprises).
COMMANDS_LIST="$("$EXE" cmd commands.list --spawn 2>&1)" || true
if ! echo "$COMMANDS_LIST" | grep -q "agent.triage.run"; then
    echo "[agentic-triage-cli] SKIP — agent.triage.run not registered"
    exit 0
fi

PASSED=0
FAILED=0

# Test 1: --issue mode against a fake issue key. With no GitHub PAT configured
# in CI the controller returns the "configure GitHub PAT" message; with a PAT
# set we'd fail later at the HTTP layer. Either way the JSON envelope must be
# coherent.
OUT="$("$EXE" cmd agent.triage.run --source=github --issue=fake/repo#1 --spawn 2>&1)" || true
if echo "$OUT" | grep -q '"command".*"agent.triage.run"' \
   && (echo "$OUT" | grep -q '"ok"' || echo "$OUT" | grep -q '"error"'); then
    echo "  ok: agent.triage.run --issue surfaced a coherent JSON envelope"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: agent.triage.run --issue envelope not coherent"
    echo "  output:"
    echo "$OUT" | sed 's/^/    /'
    FAILED=$((FAILED + 1))
fi

# Test 2: mutual-exclusion gate — passing both --query and --issue must yield
# a validation error envelope, not crash the binary.
OUT2="$("$EXE" cmd agent.triage.run --source=github --query=fake/repo --issue=fake/repo#1 --spawn 2>&1)" || true
if echo "$OUT2" | grep -q '"command".*"agent.triage.run"' \
   && echo "$OUT2" | grep -q '"error"' ; then
    echo "  ok: agent.triage.run rejects mutually exclusive --query + --issue"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: --query + --issue mutual exclusion not surfaced"
    echo "  output:"
    echo "$OUT2" | sed 's/^/    /'
    FAILED=$((FAILED + 1))
fi

# Test 3: missing --source — required-arg gate.
OUT3="$("$EXE" cmd agent.triage.run --query=fake/repo --spawn 2>&1)" || true
if echo "$OUT3" | grep -q '"command".*"agent.triage.run"' \
   && echo "$OUT3" | grep -q '"error"' ; then
    echo "  ok: agent.triage.run rejects missing --source"
    PASSED=$((PASSED + 1))
else
    echo "  FAIL: missing --source not surfaced"
    echo "  output:"
    echo "$OUT3" | sed 's/^/    /'
    FAILED=$((FAILED + 1))
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
