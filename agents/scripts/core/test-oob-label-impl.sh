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
# PARITY (2026-07-08, deps-override-label-parity-test): the no-arg run ALSO
# asserts the CI-downgrade label set wired in merge-gates.sh poll_merge_gates
# ($downgraded) equals the set wired in safe-admin-merge.sh evaluate_rollup —
# the two merge paths a `*-out-of-band` label must reach. ADR-0022's
# intent-out-of-band shipped honoring only the poller (surfaced live on #1435);
# plan-lock-out-of-band repeated the class until 2026-07-08. Both sets are
# extracted mechanically (label→jq-var bindings resolved against each script's
# downgrade expression), so a half-wired addition FAILS here.
#
# Override root (testing): SMATCHET_OOB_ROOT points the corpus at a fixture tree.
# SMATCHET_OOB_MG_FILE / SMATCHET_OOB_SAM_FILE point the parity extraction at
# fixture scripts (default: the real merge-gates.sh / safe-admin-merge.sh).
#
# Usage:
#   bash agents/scripts/core/test-oob-label-impl.sh            # scan the real tree
#   bash agents/scripts/core/test-oob-label-impl.sh --selftest # fixture dogfood
#
# Exit: 0 every documented label is implemented (and the two merge paths agree) ·
#       1 a documented-but-unimplemented label / a poller-vs-admin divergence
#       (or --selftest failure) · 2 infra error.
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
# merge-gates.sh's gate-condition logic is split across the entry point +
# the sourced merge-gates.d/ modules (the GATE_FILTER jq projection — where the
# per-label `any(. == "<label>")` reads live — moved to 10-gate-filter.sh).
# Both count as implementation for the label-wired-somewhere check.
_impl_files() {
    local root="$1"
    find "$root/.github/workflows" -maxdepth 1 -name '*.yml' 2>/dev/null || true
    [ -f "$root/agents/scripts/core/merge-gates.sh" ] && echo "$root/agents/scripts/core/merge-gates.sh"
    [ -f "$root/agents/scripts/core/merge-gates.d/10-gate-filter.sh" ] && echo "$root/agents/scripts/core/merge-gates.d/10-gate-filter.sh"
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

# --- Poller-vs-admin-path CI-downgrade parity --------------------------------

# _mg_ci_downgrade_labels <merge-gates.sh> — the labels merge-gates.sh actually
# applies as CI downgrades: jq vars referenced inside the `$failing → $downgraded`
# select block, resolved back through their `any(. == "<label>")) as $var` binding.
# The GATE_FILTER jq projection (with the $downgraded block + the label bindings)
# lives in the sourced module merge-gates.d/10-gate-filter.sh; if the passed file
# has no inline $downgraded block, fall back to that sibling module (a
# self-contained parity fixture keeps its block inline, so it is read directly).
_mg_ci_downgrade_labels() {
    local f="$1" region v jqf
    jqf="$f"
    if ! grep -q 'as \$downgraded' "$f"; then
        local mod
        mod="$(dirname "$f")/merge-gates.d/10-gate-filter.sh"
        [ -r "$mod" ] && jqf="$mod"
    fi
    # `q` at the closing anchor stops after the FIRST block — later `$failing`
    # mentions (the poller's count expressions) must not extend the region.
    region="$(sed -n '/\[\$failing\[\] | select(/,/as \$downgraded/{p;/as \$downgraded/q;}' "$jqf")"
    [ -n "$region" ] || { echo "test-oob-label-impl: no \$downgraded block in $jqf" >&2; return 2; }
    printf '%s\n' "$region" | grep -oE '\$[A-Za-z_][A-Za-z_0-9]*' | sort -u | while IFS= read -r v; do
        v="${v#\$}"
        sed -n "s/.*any(\\. == \"\\([a-z0-9-]*-out-of-band\\)\")) as \\\$${v}[[:space:]]*\$/\\1/p" "$jqf"
    done | sort -u
}

# _sam_ci_downgrade_labels <safe-admin-merge.sh> — the labels evaluate_rollup
# honors: label→var bindings inside the function body whose var is USED beyond
# the binding line (a bound-but-unused var is not wiring).
_sam_ci_downgrade_labels() {
    local f="$1" body line v label n
    body="$(sed -n '/^evaluate_rollup() {/,/^}/p' "$f")"
    [ -n "$body" ] || { echo "test-oob-label-impl: no evaluate_rollup() in $f" >&2; return 2; }
    while IFS= read -r line; do
        label="$(printf '%s\n' "$line" | sed -n 's/.*any(\. == "\([a-z0-9-]*-out-of-band\)")) as \$\([A-Za-z_0-9]*\).*/\1/p')"
        v="$(printf '%s\n' "$line" | sed -n 's/.*any(\. == "\([a-z0-9-]*-out-of-band\)")) as \$\([A-Za-z_0-9]*\).*/\2/p')"
        [ -n "$label" ] && [ -n "$v" ] || continue
        n="$(printf '%s\n' "$body" | grep -cE "\\\$${v}\b" || true)"
        # `if` (not `&&`): a trailing false && would leak rc 1 through pipefail.
        if [ "$n" -gt 1 ]; then printf '%s\n' "$label"; fi
    done < <(printf '%s\n' "$body" | grep -E 'any\(\. == "[a-z0-9-]*-out-of-band"\)\) as \$') | sort -u
}

# _run_parity <merge-gates.sh> <safe-admin-merge.sh> — assert the SANCTIONED
# relationship between the two downgrade sets. Since the stale-override guard
# (merge-pipeline-01) the sets are deliberately ASYMMETRIC: the poller honours
# the LABEL-REACTIVE pair (tests-/perf-out-of-band — freshness-guarded, their
# workflows re-run on `labeled`) that the admin guard, having no label-event
# timestamps, must NOT honour. So the invariant is:
#   (1) every guard-honoured label is also poller-honoured (subset), AND
#   (2) any poller-only label is one of the sanctioned label-reactive pair
#       (so equal sets — the parity fixtures — still validate, and a NEW label
#       wired into only the poller still fails).
# The reverse regression (the guard re-honouring tests/perf) is pinned by
# safe-admin-merge --selftest CASE4, not here.
_run_parity() {
    local mg="$1" sam="$2" mg_set sam_set
    # Pin collation: the callers' `sort -u` and the `comm` calls below must
    # agree on ordering regardless of the ambient locale.
    local LC_ALL=C
    local reactive=$'perf-out-of-band\ntests-out-of-band'
    [ -r "$mg" ] && [ -r "$sam" ] || { echo "test-oob-label-impl: parity sources unreadable ($mg / $sam)" >&2; return 2; }
    mg_set="$(_mg_ci_downgrade_labels "$mg")" || return 2
    sam_set="$(_sam_ci_downgrade_labels "$sam")" || return 2
    if [ -z "$mg_set" ] || [ -z "$sam_set" ]; then
        echo "test-oob-label-impl: parity extraction found an EMPTY downgrade set (mg: '${mg_set}' sam: '${sam_set}') — refusing (fail-closed; the extraction anchors may have drifted)." >&2
        return 2
    fi
    local sam_only mg_only unsanctioned
    sam_only="$(comm -13 <(printf '%s\n' "$mg_set") <(printf '%s\n' "$sam_set"))"
    mg_only="$(comm -23 <(printf '%s\n' "$mg_set") <(printf '%s\n' "$sam_set"))"
    unsanctioned="$(comm -23 <(printf '%s\n' "$mg_only" | sed '/^$/d') <(printf '%s\n' "$reactive"))"
    if [ -n "$sam_only" ] || [ -n "$unsanctioned" ]; then
        echo "test-oob-label-impl: FAIL — CI-downgrade label sets diverge beyond the sanctioned label-reactive asymmetry:" >&2
        echo "  merge-gates.sh \$downgraded:        $(printf '%s' "$mg_set" | tr '\n' ' ')" >&2
        echo "  safe-admin-merge evaluate_rollup:  $(printf '%s' "$sam_set" | tr '\n' ' ')" >&2
        echo "  Sanctioned poller-only labels:     $(printf '%s' "$reactive" | tr '\n' ' ')" >&2
        echo "  Wire a new label into BOTH paths (a half-wired override refuses labelled PRs on one path — #1435), or add it to the sanctioned label-reactive pair here ONLY with a stale-override-guard rationale (merge-pipeline-01)." >&2
        return 1
    fi
    echo "test-oob-label-impl: PASS — poller/admin downgrade sets match modulo the sanctioned label-reactive pair (poller: $(printf '%s' "$mg_set" | tr '\n' ' ')· admin: $(printf '%s' "$sam_set" | tr '\n' ' '))."
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

    # 3. Parity divergence: mg wires two labels, sam only one -> FAIL.
    cat > "$tmp/mg.sh" <<'SH'
| ($labels | any(. == "tests-out-of-band")) as $tests
| ($labels | any(. == "phantom-out-of-band")) as $ph
| ([$failing[] | select(
      ($tests and .name == "Test-delta gate") or
      ($ph and .name == "Phantom gate"))]) as $downgraded
SH
    cat > "$tmp/sam.sh" <<'SH'
evaluate_rollup() {
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | (($testsOob and $g.name == "Test-delta gate") | not)
}
SH
    if _run_parity "$tmp/mg.sh" "$tmp/sam.sh" >/dev/null 2>&1; then
        echo "test-oob-label-impl --selftest: FAIL — poller-vs-admin divergence was not caught" >&2
        return 1
    fi

    # 4. Wire the second label into sam too -> parity PASS.
    cat > "$tmp/sam.sh" <<'SH'
evaluate_rollup() {
        | ($labels | any(. == "tests-out-of-band")) as $testsOob
        | ($labels | any(. == "phantom-out-of-band")) as $phOob
        | (($testsOob and $g.name == "Test-delta gate") | not)
        | (($phOob and $g.name == "Phantom gate") | not)
}
SH
    if ! _run_parity "$tmp/mg.sh" "$tmp/sam.sh" >/dev/null 2>&1; then
        echo "test-oob-label-impl --selftest: FAIL — equal downgrade sets still flagged as divergent" >&2
        return 1
    fi

    echo "test-oob-label-impl --selftest: PASS — catches a documented-but-unimplemented label; passes an implemented one; catches a poller-vs-admin downgrade divergence."
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
        rc=0
        _run_check "$ROOT" || rc=$?
        MG="${SMATCHET_OOB_MG_FILE:-$ROOT/agents/scripts/core/merge-gates.sh}"
        SAM="${SMATCHET_OOB_SAM_FILE:-$ROOT/agents/scripts/core/safe-admin-merge.sh}"
        if { [ -f "$MG" ] && [ -f "$SAM" ]; } || [ -n "${SMATCHET_OOB_MG_FILE:-}${SMATCHET_OOB_SAM_FILE:-}" ]; then
            _run_parity "$MG" "$SAM" || rc=$?
        else
            # Fixture root without both merge scripts (bats corpus fixtures) —
            # parity has no subject there; the real tree always has both.
            echo "test-oob-label-impl: NOTE — parity skipped (merge-gates.sh + safe-admin-merge.sh not both under $ROOT)" >&2
        fi
        exit "$rc"
        ;;
    *)
        echo "usage: test-oob-label-impl.sh [--selftest]" >&2
        exit 2
        ;;
esac
