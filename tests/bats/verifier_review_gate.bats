#!/usr/bin/env bats
# verifier_review_gate.bats — the VERDICT half of the commit-time code-review
# gate: attaching a `verifier-sidecar.py aggregate` result to a `.review-ack`
# record, and what the gate does with it.
#
# Plan: docs/plans/verifier-scored-code-review-gate.md.
#
# Contract under test:
#   review-ack.sh --record --verdict <f>   attaches score/recommendation/hard_veto
#   review-ack.sh --check
#     0 — acked; a recorded verdict is REPORTED (score + recommendation) but the
#         score never blocks — it is advisory until verifier-calibrate.py
#         justifies promoting it (verifier-sidecar.md § Operating rules, rule 5)
#     1 — no/stale ack (unchanged from the presence-only gate)
#     2 — infra, incl. an unreadable/malformed --verdict file
#     3 — ack current but the verdict carries hard_veto
#   pre-commit renders rc 3 as a veto refusal, distinct from "no code review"
#
# The blocking asymmetry under test: `recommendation` is NOT a gate. The sidecar
# returns "block" for a low SCORE as well as for a veto, and "escalate" for an
# ordinary single sample — gating on either would either trust an uncalibrated
# score or wedge nearly every commit.

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    git init --quiet -b develop "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    mkdir -p "$REPO_TMP/scripts/git-hooks" \
        "$REPO_TMP/agents/scripts/core/lib" \
        "$REPO_TMP/Source/Core/src/Sync" \
        "$REPO_TMP/docs"
    cp "$ROOT/scripts/git-hooks/pre-commit" "$REPO_TMP/scripts/git-hooks/pre-commit"
    cp "$ROOT/agents/scripts/core/review-ack.sh" "$REPO_TMP/agents/scripts/core/review-ack.sh"
    cp "$ROOT/agents/scripts/core/lib/review-ack.sh" "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    chmod +x "$REPO_TMP/scripts/git-hooks/pre-commit"
    printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > "$REPO_TMP/project.config.json"
    echo "// base" > "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m base
    git -C "$REPO_TMP" config --local core.hooksPath scripts/git-hooks
    # Stage a strict-zone edit so every case below is scored against a
    # substantive diff.
    echo "int touched() { return 1; }" >> "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add Source/Core/src/Sync/S.cpp
}

teardown() {
    rm -rf "${REPO_TMP:-}"
}

# verdict <name> <score> <recommendation> <hard_veto> [veto_reason] — write a
# file shaped like `verifier-sidecar.py aggregate` output.
verdict() {
    local name="$1" score="$2" rec="$3" veto="$4" reason="${5:-}"
    local reasons="[]"
    [ -n "$reason" ] && reasons="[\"$reason\"]"
    cat > "$REPO_TMP/$name" <<JSON
{"schemaVersion":2,"ranking":[],"pivot_tournament":{},
 "candidates":[{"id":"review","overall_score":$score,"recommendation":"$rec",
   "hard_veto":$veto,"veto_reasons":$reasons,"uncertainty":0.1,"samples":3}]}
JSON
}

record() {
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --record --staged "$@" 2>&1)
}

check() {
    (cd "$REPO_TMP" && bash agents/scripts/core/review-ack.sh --check --staged 2>&1)
}

commit_in_fixture() {
    (cd "$REPO_TMP" && git commit --quiet -m "$1" 2>&1)
}

@test "a clean verdict records and the gate reports it" {
    verdict v.json 0.93 accept false
    run record --verdict v.json
    [ "$status" -eq 0 ]
    [[ "$output" == *"hard_veto=false"* ]]
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"score=0.93"* ]]
    [[ "$output" == *"ADVISORY"* ]]
}

@test "hard_veto blocks with rc 3 and names the reason" {
    verdict v.json 0.9 block true "path traversal in new arg handling"
    record --verdict v.json
    run check
    [ "$status" -eq 3 ]
    [[ "$output" == *"HARD VETO"* ]]
    [[ "$output" == *"path traversal in new arg handling"* ]]
}

@test "a veto blocks even when the overall score is high" {
    # Rule 2: veto beats average. A 0.9 score must not dilute a real finding.
    verdict v.json 0.95 block true "secret written to the log"
    record --verdict v.json
    run check
    [ "$status" -eq 3 ]
}

@test "recommendation=block WITHOUT a veto does NOT block" {
    # The sidecar returns "block" for a low score too. Gating on it would trust
    # an uncalibrated score — rule 5 forbids that until calibration.
    verdict v.json 0.25 block false
    record --verdict v.json
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"ADVISORY"* ]]
}

@test "recommendation=escalate does NOT block" {
    # `escalate` is the ordinary single-sample outcome; blocking on it would
    # wedge nearly every commit.
    verdict v.json 0.82 escalate false
    record --verdict v.json
    run check
    [ "$status" -eq 0 ]
}

@test "an edit after a clean verdict re-arms the presence gate" {
    verdict v.json 0.93 accept false
    record --verdict v.json
    echo "int more() { return 2; }" >> "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add Source/Core/src/Sync/S.cpp
    run check
    [ "$status" -eq 1 ]
    [[ "$output" == *"stale"* ]]
}

@test "recording with no verdict leaves the presence-only behaviour intact" {
    run record
    [ "$status" -eq 0 ]
    run check
    [ "$status" -eq 0 ]
    [[ "$output" != *"ADVISORY"* ]]
}

@test "an unreadable verdict file is infra (rc 2), never a silent presence-only ack" {
    run record --verdict does-not-exist.json
    [ "$status" -eq 2 ]
    # And nothing was recorded, so the gate still reports the diff unacked.
    run check
    [ "$status" -eq 1 ]
}

@test "a malformed verdict file is infra (rc 2)" {
    echo '{"candidates":[]}' > "$REPO_TMP/bad.json"
    run record --verdict bad.json
    [ "$status" -eq 2 ]
}

@test "a candidate missing hard_veto is rejected, not defaulted to false" {
    # The veto-dropping case: `if .hard_veto then` renders a MISSING key as
    # "false", so a truncated or foreign-schema file would silently report
    # "no veto" — losing the only signal this gate blocks on.
    echo '{"candidates":[{"overall_score":0.9,"recommendation":"accept"}]}' > "$REPO_TMP/p.json"
    run record --verdict p.json
    [ "$status" -eq 2 ]
    run check
    [ "$status" -eq 1 ]
}

@test "a candidate missing overall_score is rejected" {
    echo '{"candidates":[{"hard_veto":false,"recommendation":"accept"}]}' > "$REPO_TMP/p.json"
    run record --verdict p.json
    [ "$status" -eq 2 ]
}

@test "a non-boolean hard_veto is rejected rather than coerced" {
    # jq truthiness would treat the STRING "false" as true. Rejecting is both
    # safer and honest: the file does not mean what the schema says.
    echo '{"candidates":[{"overall_score":0.9,"hard_veto":"false"}]}' > "$REPO_TMP/p.json"
    run record --verdict p.json
    [ "$status" -eq 2 ]
}

@test "a non-numeric overall_score is rejected" {
    echo '{"candidates":[{"overall_score":"0.9","hard_veto":false}]}' > "$REPO_TMP/p.json"
    run record --verdict p.json
    [ "$status" -eq 2 ]
}

@test "--verdict= with an empty path is rejected, not silently presence-only" {
    run bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --record --staged --verdict= 2>&1"
    [ "$status" -eq 2 ]
    # And nothing was recorded.
    run check
    [ "$status" -eq 1 ]
}

@test "--verdict=<path> attaches the verdict" {
    verdict v.json 0.91 accept false
    run bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --record --staged --verdict=v.json 2>&1"
    [ "$status" -eq 0 ]
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"score=0.91"* ]]
}

@test "--verdict requires an argument" {
    run bash -c "cd '$REPO_TMP' && bash agents/scripts/core/review-ack.sh --record --staged --verdict 2>&1"
    [ "$status" -eq 2 ]
}

@test "a veto reason containing a tab cannot shift the record's fields" {
    printf '%s\n' '{"schemaVersion":2,"candidates":[{"id":"r","overall_score":0.5,' \
        '"recommendation":"block","hard_veto":true,' \
        '"veto_reasons":["tab\there and\nnewline"],"uncertainty":0.1}]}' \
        > "$REPO_TMP/t.json"
    record --verdict t.json
    run check
    [ "$status" -eq 3 ]
    # The marker must still hold exactly one record line for this mode.
    run bash -c "grep -c '^staged' '$REPO_TMP/.review-ack'"
    [ "$output" = "1" ]
}

@test "pre-commit renders a veto as a veto, not as a missing review" {
    verdict v.json 0.9 block true "unbounded retry loop on a network path"
    record --verdict v.json
    run commit_in_fixture "feat: vetoed"
    [ "$status" -ne 0 ]
    [[ "$output" == *"HARD VETO"* ]]
    [[ "$output" != *"REFUSING a commit with no code review"* ]]
}

@test "pre-commit allows a commit whose verdict is clean" {
    verdict v.json 0.93 accept false
    record --verdict v.json
    run commit_in_fixture "feat: reviewed and scored"
    [ "$status" -eq 0 ]
}

@test "the bypass still clears a veto" {
    verdict v.json 0.9 block true "some finding"
    record --verdict v.json
    run env SMATCHET_SKIP_REVIEW_GATE=1 bash -c "cd '$REPO_TMP' && git commit --quiet -m bypass 2>&1"
    [ "$status" -eq 0 ]
}
# ---------- panel verdicts end-to-end (Phase 4, absorb-whip-process) ----------
# The full pipeline the addresser runs (address-review-feedback skill step 6):
# panel leg trailers -> panel_verdicts.py -> the REAL verifier-sidecar.py
# aggregate -> review-ack --record --verdict -> gate/pre-commit. This is the
# proof for plan item 21: review-ack consumes the panel verdict with ZERO code
# changes, because the adapter+sidecar emit exactly the shape it validates.

# Probe-execute a python interpreter (agent_size.bats:25-29 canonical form) and
# resolve native-python-safe C:/ paths for the two scripts (cygpath -m — native
# python rejects MSYS /c/... arguments, agent_eval_score.bats:55 precedent).
_panel_py() {
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    [ -n "$PY" ] || skip "no working python interpreter"
    PV="$ROOT/agents/scripts/core/panel_verdicts.py"
    SIDECAR="$ROOT/scripts/dev/verifier-sidecar.py"
    if command -v cygpath >/dev/null 2>&1; then
        PV="$(cygpath -m "$PV")"; SIDECAR="$(cygpath -m "$SIDECAR")"
    fi
    export PY PV SIDECAR
}

# _panel_leg <file> <verdict-json> — a panel review file with a Verdict trailer
# inside the fixture repo's item folder.
_panel_leg() {
    mkdir -p "$REPO_TMP/docs/work/items/07-sample-item"
    {
        printf '# 07 · Sample — Post-implementation review (pass 1)\n\n'
        printf 'Problems only. Reviewed commit `abc1234`.\n\n'
        printf '## Verdict\n\n```json\n%s\n```\n' "$2"
    } > "$REPO_TMP/docs/work/items/07-sample-item/$1"
}

@test "panel pipeline end-to-end: leg veto -> adapter -> sidecar -> record -> rc 3" {
    _panel_py
    _panel_leg 5-review-1-claude-opus.md \
        '{"criteria": {"correctness": 0.9, "security": 0.9}, "confidence": 0.8, "hard_veto": false}'
    _panel_leg 5-review-1-codex-sol.md \
        '{"criteria": {"correctness": 0.3, "security": 0.2}, "confidence": 0.9, "hard_veto": true, "veto_reason": "auth bypass in new endpoint"}'
    run bash -c "cd '$REPO_TMP' \
        && \"\$PY\" \"\$PV\" --subject 7 --round 1 --items-dir docs/work/items > samples.json 2>adapter.err \
        && \"\$PY\" \"\$SIDECAR\" aggregate samples.json > verdict.json \
        && bash agents/scripts/core/review-ack.sh --record --staged --verdict verdict.json"
    [ "$status" -eq 0 ]
    run check
    [ "$status" -eq 3 ]
    [[ "$output" == *"HARD VETO"* ]]
    [[ "$output" == *"auth bypass in new endpoint"* ]]
}

@test "panel pipeline end-to-end: clean legs pass the gate and pre-commit" {
    _panel_py
    _panel_leg 5-review-1-claude-opus.md \
        '{"criteria": {"correctness": 0.9, "security": 0.95, "project_invariants": 0.9}, "confidence": 0.8, "hard_veto": false}'
    _panel_leg 5-review-1-codex-sol.md \
        '{"criteria": {"correctness": 0.85, "security": 0.9, "project_invariants": 0.85}, "confidence": 0.7, "hard_veto": false}'
    run bash -c "cd '$REPO_TMP' \
        && \"\$PY\" \"\$PV\" --subject 7 --round 1 --items-dir docs/work/items > samples.json 2>/dev/null \
        && \"\$PY\" \"\$SIDECAR\" aggregate samples.json > verdict.json \
        && bash agents/scripts/core/review-ack.sh --record --staged --verdict verdict.json"
    [ "$status" -eq 0 ]
    run check
    [ "$status" -eq 0 ]
    [[ "$output" == *"ADVISORY"* ]]
    run commit_in_fixture "feat: panel-scored"
    [ "$status" -eq 0 ]
}

@test "panel veto reaches pre-commit as a veto refusal" {
    _panel_py
    _panel_leg 5-review-1-claude-opus.md \
        '{"criteria": {"correctness": 0.8}, "hard_veto": true, "veto_reason": "gate script fails open"}'
    run bash -c "cd '$REPO_TMP' \
        && \"\$PY\" \"\$PV\" --subject 7 --round 1 --items-dir docs/work/items 2>/dev/null \
        | \"\$PY\" \"\$SIDECAR\" aggregate - > verdict.json \
        && bash agents/scripts/core/review-ack.sh --record --staged --verdict verdict.json"
    [ "$status" -eq 0 ]
    run commit_in_fixture "feat: vetoed by panel"
    [ "$status" -ne 0 ]
    [[ "$output" == *"HARD VETO"* ]]
    [[ "$output" != *"REFUSING a commit with no code review"* ]]
}
