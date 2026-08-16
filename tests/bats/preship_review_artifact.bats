#!/usr/bin/env bats
# preship_review_artifact.bats — scripts/dev/pre-ship.sh's code-review ARTIFACT
# requirement (docs/plans/shipped/parallel-review-gate.md).
#
# A fingerprint-pinned .review-ack proves the diff did not move after someone typed
# --ack-review. It does NOT prove a review happened — an empty ack is one keystroke.
# So a substantive diff must also carry .review-findings.json stamped with the
# fingerprint of the diff the reviewer actually read.
#
# The invariants under test:
#   1. --ack-review REFUSES a substantive diff with no matching artifact — the failure
#      lands on the person acking, while the review is still in front of them.
#   2. The gate path re-checks it. The artifact is an ordinary gitignored file; an ack
#      whose evidence has since been deleted is back to being a bare keystroke.
#   3. Fingerprint equality, not mere presence: a stale or unparseable artifact is no
#      artifact.
#   4. It never wedges a push — non-substantive diffs are exempt, and
#      SMATCHET_SKIP_REVIEW_GATE=1 bypasses with a WARN.
#
# Sibling suites: verifier_preship_wiring.bats (the verdict half), review_ack_gate.bats
# (the commit-side ack).

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    git init --quiet -b base "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    mkdir -p "$REPO_TMP/scripts/dev" \
        "$REPO_TMP/agents/scripts/core/lib" \
        "$REPO_TMP/Source/Core/src/Sync"
    cp "$ROOT/scripts/dev/pre-ship.sh" "$REPO_TMP/scripts/dev/pre-ship.sh"
    cp "$ROOT/agents/scripts/core/lib/review-ack.sh" "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > "$REPO_TMP/project.config.json"
    echo "// base" > "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    echo "# readme" > "$REPO_TMP/README.md"
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m base
    # A strict-zone touch is substantive under BOTH classifier paths: python present
    # (strict-zone hit) and python absent (#1116 fail-closed). No env dependency.
    git -C "$REPO_TMP" checkout --quiet -b feature
    printf '// edit\nint touched() { return 1; }\n' >> "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" commit --quiet -am edit
}

teardown() {
    rm -rf "${REPO_TMP:-}"
}

# preship <args...> — run the gate in the fixture repo with the lint stages skipped.
preship() {
    (cd "$REPO_TMP" && SMATCHET_PRESHIP_GATE_ONLY=1 bash scripts/dev/pre-ship.sh "$@" 2>&1)
}

# stamp_artifact [fingerprint] — write .review-findings.json, defaulting to the
# fingerprint of the current diff (i.e. what a real review of this diff would leave).
stamp_artifact() {
    local fp="${1:-}"
    [ -n "$fp" ] || fp="$(preship --review-fingerprint base | tr -d '\r')"
    printf '{"fingerprint":"%s","reviewer":"bats","findings":[]}\n' "$fp" \
        > "$REPO_TMP/.review-findings.json"
}

@test "--ack-review refuses a substantive diff with no review artifact" {
    run preship --ack-review base
    [ "$status" -eq 1 ]
    [[ "$output" == *"no code-review artifact found"* ]]
    # The ack must not be recorded either — a written marker would let the NEXT plain
    # run sail through on a fingerprint match, turning the refusal into a speed bump.
    [ ! -f "$REPO_TMP/.review-ack" ]
}

@test "--ack-review refuses an artifact stamped with a different diff" {
    stamp_artifact "$(printf '%064d' 0)"
    run preship --ack-review base
    [ "$status" -eq 1 ]
    [[ "$output" == *"artifact is stale"* ]]
    [ ! -f "$REPO_TMP/.review-ack" ]
}

@test "--ack-review refuses an artifact carrying no parseable fingerprint" {
    # Presence is not evidence: an artifact the gate cannot read a fingerprint out of
    # pins nothing, so it must be treated as absent rather than accepted on faith.
    printf '{"reviewer":"bats","fingerprint":"not-a-sha"}\n' > "$REPO_TMP/.review-findings.json"
    run preship --ack-review base
    [ "$status" -eq 1 ]
    [[ "$output" == *"no code-review artifact found"* ]]
}

@test "--review-fingerprint yields the stamp that makes the ack pass" {
    # The documented reviewer workflow end to end: read the fingerprint, stamp it into
    # the artifact, ack. If these two ever compute the diff differently, the gate is
    # unsatisfiable and every push wedges.
    stamp_artifact
    run preship --ack-review base
    [ "$status" -eq 0 ]
    [[ "$output" == *"Safe to push"* ]]
    [ -f "$REPO_TMP/.review-ack" ]
}

@test "the gate path refuses when the artifact is deleted after a valid ack" {
    stamp_artifact
    preship --ack-review base
    rm -f "$REPO_TMP/.review-findings.json"
    run preship base
    [ "$status" -eq 1 ]
    [[ "$output" == *"no code-review artifact found"* ]]
    [[ "$output" != *"Safe to push"* ]]
}

@test "the gate path passes with a current ack AND a matching artifact" {
    stamp_artifact
    preship --ack-review base
    run preship base
    [ "$status" -eq 0 ]
    [[ "$output" == *"artifact present and matching"* ]]
    [[ "$output" == *"Safe to push"* ]]
}

@test "a non-substantive diff needs no artifact" {
    git -C "$REPO_TMP" checkout --quiet -b docs base
    echo "more" >> "$REPO_TMP/README.md"
    git -C "$REPO_TMP" commit --quiet -am docs
    run preship base
    [ "$status" -eq 0 ]
    [[ "$output" == *"gate N/A"* ]]
    [ ! -f "$REPO_TMP/.review-findings.json" ]
}

@test "SMATCHET_SKIP_REVIEW_GATE=1 bypasses the artifact requirement with a WARN" {
    # The escape hatch is load-bearing: a gate that can wedge a push outright gets
    # deleted the first time it misfires. It must stay loud, not silent.
    run bash -c "cd '$REPO_TMP' && SMATCHET_PRESHIP_GATE_ONLY=1 SMATCHET_SKIP_REVIEW_GATE=1 bash scripts/dev/pre-ship.sh --ack-review base 2>&1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"bypassed"* ]]
}
