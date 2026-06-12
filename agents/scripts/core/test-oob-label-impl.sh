#!/usr/bin/env bash
# test-oob-label-impl.sh — assert every documented `*-out-of-band` override label
# has a real implementation that READS it.
#
# Class of bug this kills (preventing gate from postmortems.md 2026-06-07
# coverage prose-promise): an escape-hatch label is documented in a workflow
# comment / AGENTS.md § Merge gates at gate-graduation time but never wired into
# any code that reads it — so the "escape" silently does nothing until the first
# PR that needs it (discovered only mid-flight, e.g. coverage-out-of-band on
# #939). A label that is only ever mentioned in prose/comments, never read by a
# non-comment line of a workflow or merge-gates.sh, FAILS this gate.
#
# Definitions:
#   DOCUMENTED  — a `<prefix>-out-of-band` token appears anywhere (comment, prose,
#                 or code) in the discovery corpus: .github/workflows/*.yml,
#                 AGENTS.md, docs/agent-rules/merge-gates.md.
#   IMPLEMENTED — the same token appears on at least one NON-comment line (first
#                 non-space char is not '#') of an impl file: .github/workflows/*.yml
#                 or agents/scripts/core/merge-gates.sh. That is a step/branch that
#                 actually reads the label.
#
# Override root (testing): SMATCHET_OOB_ROOT points the corpus at a fixture tree.
#
# Usage:
#   bash agents/scripts/core/test-oob-label-impl.sh            # scan the real tree
#   bash agents/scripts/core/test-oob-label-impl.sh --selftest # fixture dogfood
#
# Exit: 0 every documented label is implemented · 1 a documented-but-unimplemented
#       label (or --selftest failure) · 2 infra error.
# Goes through test-shell-lint.sh + the test-gate-selftests.sh marker check.
#
# selftest: asserts-failure
set -euo pipefail

LABEL_RE='[a-z][a-z0-9]*(-[a-z0-9]+)*-out-of-band'

# Print discovery files (label NAMES come from here — comments included).
_discovery_files() {
    local root="$1"
    find "$root/.github/workflows" -maxdepth 1 -name '*.yml' 2>/dev/null || true
    [ -f "$root/AGENTS.md" ] && echo "$root/AGENTS.md"
    [ -f "$root/docs/agent-rules/merge-gates.md" ] && echo "$root/docs/agent-rules/merge-gates.md"
}

# Print impl files (a NON-comment line here counts as an implementation).
_impl_files() {
    local root="$1"
    find "$root/.github/workflows" -maxdepth 1 -name '*.yml' 2>/dev/null || true
    [ -f "$root/agents/scripts/core/merge-gates.sh" ] && echo "$root/agents/scripts/core/merge-gates.sh"
}

# Collect the set of documented label names under $root.
_documented_labels() {
    local root="$1" f
    while IFS= read -r f; do
        [ -n "$f" ] && [ -r "$f" ] || continue
        grep -hoE "$LABEL_RE" "$f" 2>/dev/null || true
    done < <(_discovery_files "$root") | sort -u
}

# Is $label referenced by a NON-comment line in any impl file under $root?
_label_is_implemented() {
    local root="$1" label="$2" f
    while IFS= read -r f; do
        [ -n "$f" ] && [ -r "$f" ] || continue
        # grep lines containing the label, then drop pure-comment lines
        # (first non-space char == '#'). A surviving line = a real read.
        if grep -nF "$label" "$f" 2>/dev/null \
            | sed 's/^[0-9]*://' \
            | grep -vqE '^[[:space:]]*#' ; then
            return 0
        fi
    done < <(_impl_files "$root")
    return 1
}

# Core check over $root. Echoes findings; returns 0 clean / 1 a violation.
_run_check() {
    local root="$1" label rc=0 n=0 impl=0
    local -a missing=()
    while IFS= read -r label; do
        [ -n "$label" ] || continue
        n=$((n + 1))
        if _label_is_implemented "$root" "$label"; then
            impl=$((impl + 1))
        else
            missing+=("$label")
            rc=1
        fi
    done < <(_documented_labels "$root")

    if [ "$n" -eq 0 ]; then
        echo "test-oob-label-impl: WARN — zero documented *-out-of-band labels found under $root" >&2
        # Not a failure: a repo may legitimately have none. (The real tree has several.)
        return 0
    fi
    if [ "$rc" -ne 0 ]; then
        echo "test-oob-label-impl: FAIL — documented override label(s) with NO implementation:" >&2
        local m
        for m in "${missing[@]}"; do
            echo "  - $m   (documented in a comment/prose but no non-comment workflow/merge-gates line reads it)" >&2
        done
        echo "  Wire each label into a workflow step or a merge-gates.sh downgrade branch, or remove the prose that promises it." >&2
        return 1
    fi
    echo "test-oob-label-impl: PASS — all $impl documented *-out-of-band label(s) are implemented."
    return 0
}

# --selftest — fixture dogfood. A documented-but-unimplemented label MUST fail;
# adding an impl line MUST pass (the asserted-failure case the gate-selftests
# meta-gate requires). No network; a throwaway tree.
run_selftest() {
    local tmp
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    mkdir -p "$tmp/.github/workflows" "$tmp/agents/scripts/core"

    # 1. A workflow that DOCUMENTS phantom-out-of-band only in a comment -> FAIL.
    cat > "$tmp/.github/workflows/sample.yml" <<'YML'
# Applying the phantom-out-of-band label downgrades the gate to a warning.
name: Sample
on: { pull_request: { branches: [develop] } }
jobs:
  g:
    runs-on: ubuntu-latest
    steps:
      - run: echo "no label read here"
YML
    if SMATCHET_OOB_ROOT="$tmp" _run_check "$tmp" >/dev/null 2>&1; then
        echo "test-oob-label-impl --selftest: FAIL — unimplemented label was not caught" >&2
        return 1
    fi

    # 2. Add a non-comment line that READS the label -> PASS.
    cat >> "$tmp/.github/workflows/sample.yml" <<'YML'
      - run: |
          if echo ",$labels," | grep -q ',phantom-out-of-band,'; then override=true; fi
YML
    if ! SMATCHET_OOB_ROOT="$tmp" _run_check "$tmp" >/dev/null 2>&1; then
        echo "test-oob-label-impl --selftest: FAIL — implemented label still flagged" >&2
        return 1
    fi

    echo "test-oob-label-impl --selftest: PASS — catches a documented-but-unimplemented label; passes an implemented one."
    return 0
}

case "${1:-}" in
    --selftest)
        run_selftest
        exit $?
        ;;
    "")
        ROOT="${SMATCHET_OOB_ROOT:-}"
        if [ -z "$ROOT" ]; then
            ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
        fi
        _run_check "$ROOT"
        exit $?
        ;;
    *)
        echo "usage: test-oob-label-impl.sh [--selftest]" >&2
        exit 2
        ;;
esac
