#!/usr/bin/env bash
# test-required-context-parity.sh — assert every branch-protection REQUIRED
# context emits UNCONDITIONALLY (no paths-filtered deadlock).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (infra.md 2026-06-05 P1 · postmortems.md "required-but-path-
# filtered check")
#   `Doc anchors + agent contract` was a REQUIRED branch-protection context
#   (project.config.json § branch_protection.required_contexts) whose emitting
#   workflow doc-validation.yml was `paths:`-filtered. A PR touching none of the
#   filtered paths never produced the context, so GitHub held it BLOCKED forever
#   — deadlocking #880/#881/#882 (pure product/test diffs). PR #884 fixed the one
#   workflow (runs on every PR + self-gates), but NOTHING prevents the NEXT
#   required context added with a `paths:` filter from recreating the deadlock.
#
# WHAT THIS GATE ASSERTS
#   For each `branch_protection.required_contexts` entry in project.config.json:
#     1. Resolve it to its emitting workflow+job via the shared resolution map
#        (lib/ci-check-resolve.sh). FAIL CLOSED if unresolvable — an
#        unresolvable required context IS the deadlock risk (it can never report
#        on a PR whose paths the orphaned/renamed workflow no longer matches).
#     2. Assert the emitting workflow's `on.pull_request` has NO `paths:` /
#        `paths-ignore:` filter — UNLESS that workflow carries the documented
#        self-gate marker `# ci-required-context: self-gated` (the #884 shape:
#        the workflow runs unconditionally but the job internally short-circuits,
#        so the required context is ALWAYS reported exactly once).
#
# WHY FAIL-CLOSED ON UNRESOLVABLE
#   The whole bug class is "a required check that silently never reports." A
#   parity gate that SKIPPED names it can't resolve would reintroduce the exact
#   blind spot. Unresolvable => FAIL, forcing the author to fix the name or
#   document the self-gate.
#
# MODES
#   (no args) | --check   resolve every live required_contexts entry against the
#                         live .github/workflows; FAIL (1) on any path-filtered
#                         or unresolvable required context. This is what
#                         scripts/dev/test-all.sh runs (no-arg).
#   --selftest            self-contained proof against synthetic fixtures: a
#                         clean context PASSES, a path-filtered context FAILS, an
#                         unresolvable context FAILS, a self-gate-marked
#                         path-filtered context PASSES. Asserts failure cases.
#
# EXIT  0 every required context emits unconditionally (or self-gated) ·
#       1 a path-filtered / unresolvable required context · 2 infra error.
#
# Goes through test-shell-lint.sh (5 rules) + the shellcheck warning fail-set.
# selftest: asserts-failure — the --selftest path feeds a path-filtered + an
# unresolvable fixture and asserts the gate FAILS on each (its core duty).
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || { echo "test-required-context-parity: cannot cd to repo root" >&2; exit 2; }

# shellcheck source=agents/scripts/core/lib/ci-check-resolve.sh
. "$ROOT/agents/scripts/core/lib/ci-check-resolve.sh"

CONFIG="${CI_PARITY_CONFIG:-project.config.json}"

# Probe a working python interpreter (reused for the config read). The shared lib
# resolver also probes; this one only needs JSON (stdlib), so no yaml requirement.
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -n "$PY" ] || { echo "test-required-context-parity: no working python interpreter" >&2; exit 2; }

# Emit each branch_protection.required_contexts entry, one per line, raw LF
# (text-mode print() emits CRLF on Windows; a trailing \r would break the
# exact-string match against the rendered job name).
list_required_contexts() {
    local cfg="$1"
    [ -f "$cfg" ] || { echo "test-required-context-parity: missing $cfg" >&2; return 2; }
    "$PY" - "$cfg" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], encoding="utf-8"))
ctxs = cfg.get("branch_protection", {}).get("required_contexts", [])
for c in ctxs:
    sys.stdout.buffer.write((str(c) + "\n").encode("utf-8"))  # raw LF
PY
}

# parity_check_one "<name>" — resolve + assert. Echoes a status line; returns
# 0 OK / 1 violation / 2 infra error.
parity_check_one() {
    local name="$1" rec status wf job paths self templated rc
    rec="$(ci_resolve_check "$name")"; rc=$?
    if [ "$rc" -eq 2 ]; then
        echo "  INFRA  '$name' — resolver infra error" >&2
        return 2
    fi
    # rec is TAB-separated: status wf job has_pr_paths_filter self_gated templated
    IFS=$'\t' read -r status wf job paths self templated <<EOF
$rec
EOF
    if [ "$status" != "RESOLVED" ]; then
        echo "  FAIL   '$name' — UNRESOLVABLE: no workflow job renders this required context." >&2
        echo "         A required context that no workflow emits can never report -> the PR" >&2
        echo "         deadlocks BLOCKED forever. Fix the name in project.config.json §" >&2
        echo "         branch_protection.required_contexts, or rename/restore the emitting job." >&2
        return 1
    fi
    if [ "$paths" = "true" ] && [ "$self" != "true" ]; then
        echo "  FAIL   '$name' — emitted by $wf job '$job' whose on.pull_request carries a" >&2
        echo "         paths:/paths-ignore: filter. A path-filtered REQUIRED context deadlocks" >&2
        echo "         every PR that touches none of those paths (#880/#881/#882 class)." >&2
        echo "         Remove the pull_request paths filter (self-gate the job internally), or" >&2
        echo "         add the marker '# ci-required-context: self-gated' to $wf." >&2
        return 1
    fi
    local note=""
    [ "$self" = "true" ] && note=" [self-gated]"
    [ "$templated" = "true" ] && note="$note [templated-prefix]"
    echo "  OK     '$name' -> $wf job '$job' (unconditional pull_request)$note"
    return 0
}

run_check() {
    local list rc=0 n=0 ok=0 bad=0 name line crc
    list="$(list_required_contexts "$CONFIG")" || return 2
    if [ -z "$list" ]; then
        echo "test-required-context-parity: FAIL — zero required_contexts in $CONFIG" >&2
        echo "    (branch_protection.required_contexts is empty/missing — fail-closed)." >&2
        return 2
    fi
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        name="$line"
        n=$((n + 1))
        parity_check_one "$name"; crc=$?
        if [ "$crc" -eq 2 ]; then return 2; fi
        if [ "$crc" -eq 0 ]; then ok=$((ok + 1)); else bad=$((bad + 1)); rc=1; fi
    done <<EOF
$list
EOF
    if [ "$rc" -eq 0 ]; then
        echo "PASS — all $n required context(s) emit unconditionally (or are self-gated)."
    fi
    # Summary line in the scripts/dev/test-all.sh aggregate format (exactly two
    # spaces between fields) so the no-arg test-all glob run aggregates this gate.
    echo "Passed: $ok  Failed: $bad"
    return "$rc"
}

# ---- selftest: synthetic fixtures proving fire + clean ----------------------
run_selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)" || { echo "selftest: mktemp failed" >&2; return 2; }
    trap 'rm -rf "$tmp"' RETURN
    mkdir -p "$tmp/wf"

    # Clean: runs on pull_request with NO paths filter.
    cat > "$tmp/wf/clean.yml" <<'YML'
name: Clean
on:
  pull_request:
    branches: [develop]
jobs:
  clean-job:
    name: Clean Context
    runs-on: ubuntu-latest
    steps:
      - run: true
YML
    # Path-filtered: a required context here MUST fail.
    cat > "$tmp/wf/filtered.yml" <<'YML'
name: Filtered
on:
  pull_request:
    branches: [develop]
    paths:
      - '**/*.md'
jobs:
  filtered-job:
    name: Filtered Context
    runs-on: ubuntu-latest
    steps:
      - run: true
YML
    # Self-gated path-filtered: the marker makes it parity-OK despite the filter.
    cat > "$tmp/wf/selfgated.yml" <<'YML'
name: SelfGated
# ci-required-context: self-gated
on:
  pull_request:
    branches: [develop]
    paths:
      - '**/*.md'
jobs:
  selfgated-job:
    name: SelfGated Context
    runs-on: ubuntu-latest
    steps:
      - run: true
YML

    export CI_WORKFLOWS_DIR="$tmp/wf"

    # selftest: asserts-failure — a path-filtered required context MUST be caught.
    if parity_check_one "Filtered Context" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: path-filtered required context was NOT flagged" >&2
        rc=1
    fi
    # An unresolvable required context MUST be caught (fail-closed).
    if parity_check_one "No Such Context Anywhere" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: unresolvable required context was NOT flagged" >&2
        rc=1
    fi
    # A clean context MUST pass.
    if ! parity_check_one "Clean Context" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: clean unconditional context was wrongly flagged" >&2
        rc=1
    fi
    # A self-gate-marked path-filtered context MUST pass.
    if ! parity_check_one "SelfGated Context" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: self-gate-marked context was wrongly flagged" >&2
        rc=1
    fi

    unset CI_WORKFLOWS_DIR
    if [ "$rc" -eq 0 ]; then
        echo "test-required-context-parity --selftest: PASS (fire on path-filtered + unresolvable; clean on unconditional + self-gated)"
    fi
    return "$rc"
}

case "${1:---check}" in
    --check)    run_check ;;
    --selftest) run_selftest ;;
    -h|--help)  echo "usage: $0 [--check|--selftest]" ;;
    *) echo "usage: $0 [--check|--selftest]" >&2; exit 2 ;;
esac
