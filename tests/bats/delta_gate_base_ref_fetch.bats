#!/usr/bin/env bats
# tests/bats/delta_gate_base_ref_fetch.bats
# ----------------------------------------------------------------------------
# Regression lint for the #1227(b) "no merge base" diff-gate false-fail
# (postmortems.md #1227; infra.md ci-infra-flake-reds-masquerade-as-real-
# breakage, next-action (b)).
#
# A delta-gated CI job that diffs against the PR base with three-dot
# `git diff <base>...HEAD` needs the merge-base present in the local repo. A
# shallow `git fetch --depth=1 origin <base_ref>` re-introduces a .git/shallow
# boundary at the base tip even after a fetch-depth:0 checkout, so the three-dot
# diff exits 128 once the PR's branch point predates that depth:
#   - Pillar 2 scanner (fail-closed since #1192) → loud false-fail on docs-only PRs.
#   - cpp-lint range / dup-scan / perf detect (diff masked `2>/dev/null||true`)
#     → silent under-scan / over-build (fail-open / fail-safe).
#
# Invariant: in EVERY workflow under .github/workflows/, a `git fetch` of the PR
# base ref MUST NOT carry --depth (full fetch keeps the common ancestor — the
# pattern the passing Doc-anchors + Coverage jobs already use), and the diff
# gates that consume it MUST checkout with fetch-depth: 0.
#
# selftest: a NEGATIVE fixture (a synthetic fetch line WITH --depth=1) MUST be
# flagged by the lint helper — proves the check fires, not just passes through
# (build-log-regex lesson: a false-pass regression must be caught at authoring).
#
# Requires: bash, grep, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    WF_DIR="$REPO_ROOT/.github/workflows"
    export WF_DIR
}

# bad_base_ref_fetch <file> — echo any line that fetches the PR base ref WITH a
# --depth flag (the defect). Matches both spellings of the base ref: the inline
# `${{ github.base_ref }}` template and the secure `"$BASE_REF"` env form. rc 0
# if at least one offending line exists.
bad_base_ref_fetch() {
    grep -nE 'git[[:space:]]+fetch.*--depth.*(github\.base_ref|BASE_REF)' "$1"
}

# fetches_base_ref <file> — true if the workflow fetches the PR base ref at all.
fetches_base_ref() {
    grep -qE 'git[[:space:]]+fetch.*(github\.base_ref|BASE_REF)' "$1"
}

# Diff-against-base gates that MUST keep the merge-base intact (three-dot
# `<base>...HEAD` consumers).
DELTA_GATES=(pillar2-scan.yml dup-scan.yml build-and-test.yml perf-pr-fast.yml)

# Passing siblings the fix aligns to — already shallow-free; positive controls.
CLEAN_SIBLINGS=(doc-validation.yml coverage.yml)

@test "no delta-gated workflow shallow-fetches the PR base ref (--depth)" {
    for wf in "${DELTA_GATES[@]}"; do
        run bad_base_ref_fetch "$WF_DIR/$wf"
        [ "$status" -ne 0 ] || {
            echo "REGRESSION: $wf shallow-fetches the base ref (breaks merge-base):" >&2
            echo "$output" >&2
            return 1
        }
    done
}

@test "NO workflow anywhere shallow-fetches the base ref (catches a new gate)" {
    # Sweep every workflow, not just the known gates — a freshly added job that
    # copy-pastes the broken `--depth=1 origin <base_ref>` pattern is caught here.
    local offenders=""
    for wf in "$WF_DIR"/*.yml; do
        [ -e "$wf" ] || continue
        if bad_base_ref_fetch "$wf" >/dev/null; then
            offenders="$offenders $(basename "$wf")"
        fi
    done
    [ -z "$offenders" ] || {
        echo "REGRESSION: shallow base-ref fetch in:$offenders" >&2
        for wf in $offenders; do bad_base_ref_fetch "$WF_DIR/$wf" >&2; done
        return 1
    }
}

@test "delta-gated workflows checkout with fetch-depth: 0" {
    # Without a full checkout the merge-base is absent even with a full base-ref
    # fetch, so both halves of the fix must hold together.
    for wf in "${DELTA_GATES[@]}"; do
        run grep -qE 'fetch-depth:[[:space:]]*0' "$WF_DIR/$wf"
        [ "$status" -eq 0 ] || {
            echo "REGRESSION: $wf checkout is not fetch-depth: 0" >&2
            return 1
        }
    done
}

@test "positive control: the passing siblings fetch the base ref shallow-free" {
    for wf in "${CLEAN_SIBLINGS[@]}"; do
        fetches_base_ref "$WF_DIR/$wf" || {
            echo "TEST DRIFT: $wf no longer fetches the base ref — update CLEAN_SIBLINGS" >&2
            return 1
        }
        run bad_base_ref_fetch "$WF_DIR/$wf"
        [ "$status" -ne 0 ] || {
            echo "control broke: $wf now shallow-fetches the base ref" >&2
            echo "$output" >&2
            return 1
        }
    done
}

# Negative-test (build-log-regex lesson): the lint helper MUST flag a synthetic
# fetch line that carries --depth. If this ever passes through, the sweep above
# is dead and a real reintroduction would slip by green.
@test "negative-test: the lint helper flags a synthetic --depth base-ref fetch" {
    local fake; fake="$(mktemp)"
    cat > "$fake" <<'YML'
            git fetch --no-tags --depth=1 origin "${{ github.base_ref }}" 2>/dev/null || true
YML
    run bad_base_ref_fetch "$fake"
    rm -f "$fake"
    [ "$status" -eq 0 ]
    [[ "$output" == *"--depth=1"* ]]
}

@test "negative-test: the lint helper flags the env (\$BASE_REF) spelling too" {
    local fake; fake="$(mktemp)"
    cat > "$fake" <<'YML'
          git fetch --no-tags --depth=1 origin "$BASE_REF" 2>/dev/null || true
YML
    run bad_base_ref_fetch "$fake"
    rm -f "$fake"
    [ "$status" -eq 0 ]
}
