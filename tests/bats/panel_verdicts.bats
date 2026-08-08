#!/usr/bin/env bats
# tests/bats/panel_verdicts.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/panel_verdicts.py — the panel-verdict
# -> verifier-sample adapter (Phase 4 of docs/plans/absorb-whip-process.md).
#
# Covers: trailer extraction (last `## Verdict` heading, first ```json fence),
# subject resolution (NN prefix / exact / ambiguous), the failure asymmetry
# (missing trailer = skip+warn, malformed trailer = exit 2, zero samples =
# exit 2), sample type checks (hard_veto strict bool, criteria numbers), and
# a live pipe into scripts/dev/verifier-sidecar.py aggregate so the emitted
# shape is proven against the real consumer, not a copy of its spec.
#
# All tests use --items-dir with paths relative to $BATS_TEST_TMPDIR — native
# Windows python rejects MSYS /tmp/... absolutes, relative paths are portable
# (agent_eval_score.bats:55 precedent).
#
# Requires: bash, bats, a working python interpreter (python3/python/py).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    PV="$REPO_ROOT/agents/scripts/core/panel_verdicts.py"
    SIDECAR="$REPO_ROOT/scripts/dev/verifier-sidecar.py"
    # Native Windows python rejects MSYS /c/... absolutes — hand it C:/ paths
    # (verifier_review_gate.bats does the same for these two scripts).
    if command -v cygpath >/dev/null 2>&1; then
        PV="$(cygpath -m "$PV")"
        SIDECAR="$(cygpath -m "$SIDECAR")"
    fi
    export PV SIDECAR
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
    cd "$BATS_TEST_TMPDIR"
    mkdir -p items/07-sample-item
}

# Write a leg review file $1 (relative to items/07-sample-item) whose verdict
# trailer body is $2; no trailer when $2 is empty.
_leg() {
    local name="$1" verdict="${2-}"
    {
        printf '# Post-implementation review (pass 1)\n\n'
        printf 'Problems only. Reviewed commit `abc1234`.\n\n## 1. Finding\n\nBody.\n'
        if [ -n "$verdict" ]; then
            printf '\n## Verdict\n\n```json\n%s\n```\n' "$verdict"
        fi
    } > "items/07-sample-item/$name"
}

OK_VERDICT='{"criteria": {"correctness": 0.9, "security": 0.8}, "confidence": 0.7, "hard_veto": false}'
VETO_VERDICT='{"criteria": {"correctness": 0.2}, "confidence": 0.9, "hard_veto": true, "veto_reason": "deterministic gate broken"}'

# ---------- happy path ----------

@test "two legs with trailers -> one candidate, two samples, derived id" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-codex-sol.md "$VETO_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
cands = doc["candidates"]
assert len(cands) == 1, cands
assert cands[0]["id"] == "07-sample-item-5-round-1", cands[0]["id"]
assert len(cands[0]["samples"]) == 2, cands[0]["samples"]
assert cands[0]["samples"][1]["hard_veto"] is True
'
}

@test "emitted samples feed the REAL sidecar; veto propagates to verdict" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-codex-sol.md "$VETO_VERDICT"
    "$PY" "$PV" --subject 7 --round 1 --items-dir items > samples.json
    run bash -c "\"$PY\" \"$SIDECAR\" aggregate samples.json"
    [ "$status" -eq 0 ]
    echo "$output" | "$PY" -c '
import json, sys
cand = json.load(sys.stdin)["candidates"][0]
assert cand["hard_veto"] is True, cand
assert cand["recommendation"] == "block", cand
assert "deterministic gate broken" in " ".join(cand.get("veto_reasons", [])), cand
'
}

@test "no-veto samples aggregate clean through the real sidecar" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    run bash -c "\"$PY\" \"$PV\" --subject 7 --round 1 --items-dir items | \"$PY\" \"$SIDECAR\" aggregate -"
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
cand = json.load(sys.stdin)["candidates"][0]
assert cand["hard_veto"] is False, cand
'
}

@test "subject resolves by exact folder name too" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 07-sample-item --round 1 --items-dir items
    [ "$status" -eq 0 ]
}

@test "round scoping: only the requested round's legs are read" {
    _leg 5-review-1-claude-opus.md "$VETO_VERDICT"
    _leg 5-review-2-claude-opus.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 2 --items-dir items
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
samples = doc["candidates"][0]["samples"]
assert len(samples) == 1 and samples[0]["hard_veto"] is False, samples
'
}

@test "pre-gate 4-review files are ignored (post gate only)" {
    _leg 4-review-1-claude-opus.md "$VETO_VERDICT"
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
assert len(doc["candidates"][0]["samples"]) == 1
assert doc["candidates"][0]["samples"][0]["hard_veto"] is False
'
}

@test "LAST Verdict heading wins (review body may discuss earlier drafts)" {
    {
        printf '## Verdict\n\n```json\n{"overall_score": 0.1}\n```\n'
        printf '\ntext between\n\n## Verdict\n\n```json\n{"overall_score": 0.9}\n```\n'
    } > items/07-sample-item/5-review-1-claude-opus.md
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
assert doc["candidates"][0]["samples"][0]["overall_score"] == 0.9
'
}

# ---------- degradation: missing trailer skips, never fails ----------

@test "leg without trailer is SKIPPED with a warning; others still emit" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-cursor-sonnet.md ""
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    [[ "$output" == *"SKIPPED"* ]]
    [[ "$output" == *"5-review-1-cursor-sonnet.md"* ]]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
assert len(doc["candidates"][0]["samples"]) == 1
'
}

# ---------- fail closed: exit 2 shapes ----------

@test "ALL legs missing trailers -> exit 2 (never invent a verdict)" {
    _leg 5-review-1-claude-opus.md ""
    _leg 5-review-1-codex-sol.md ""
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"0 usable verdicts"* ]]
}

@test "no leg files at all for the round -> exit 2" {
    run "$PY" "$PV" --subject 7 --round 3 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"no 5-review-3-"* ]]
}

@test "Verdict heading with no json fence -> exit 2 (malformed, may hide veto)" {
    printf '## Verdict\n\nno fence here\n' > items/07-sample-item/5-review-1-claude-opus.md
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"no \`\`\`json fence"* ]]
}

@test "unparseable verdict JSON -> exit 2, names the leg file" {
    _leg 5-review-1-claude-opus.md '{"criteria": nope}'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"5-review-1-claude-opus.md"* ]]
    [[ "$output" == *"does not parse"* ]]
}

@test "one malformed leg fails the run even when a sibling is clean" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-codex-sol.md '{"hard_veto": "true", "overall_score": 0.5}'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"'hard_veto' must be a JSON boolean"* ]]
}

@test "string hard_veto rejected (bool coercion would eat a veto)" {
    _leg 5-review-1-claude-opus.md '{"overall_score": 0.9, "hard_veto": "false"}'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"'hard_veto' must be a JSON boolean, not str"* ]]
}

@test "verdict with neither criteria nor overall_score -> exit 2" {
    _leg 5-review-1-claude-opus.md '{"hard_veto": false, "confidence": 0.8}'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"neither 'criteria' nor 'overall_score'"* ]]
}

@test "non-numeric criteria value -> exit 2" {
    _leg 5-review-1-claude-opus.md '{"criteria": {"correctness": "high"}}'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"criteria.correctness must be a number"* ]]
}

@test "verdict that is a JSON array, not object -> exit 2" {
    _leg 5-review-1-claude-opus.md '[0.9]'
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"must be a JSON object"* ]]
}

# ---------- subject resolution errors ----------

@test "ambiguous subject prefix -> exit 2" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    mkdir -p items/07-second-item
    cp items/07-sample-item/5-review-1-claude-opus.md items/07-second-item/
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"exactly one"* ]]
}

@test "unknown subject -> exit 2; missing items dir -> exit 2" {
    run "$PY" "$PV" --subject 99 --round 1 --items-dir items
    [ "$status" -eq 2 ]
    [[ "$output" == *"matched 0"* ]]
    run "$PY" "$PV" --subject 7 --round 1 --items-dir does-not-exist
    [ "$status" -eq 2 ]
    [[ "$output" == *"items dir not found"* ]]
}

@test "NN prefix does not cross the hyphen boundary (7 never matches 71-)" {
    mkdir -p items/71-other-item
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    cp items/07-sample-item/5-review-1-claude-opus.md items/71-other-item/
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
doc = json.load(sys.stdin)
assert doc["candidates"][0]["id"].startswith("07-sample-item-")
'
}

# ---------- CLI hygiene ----------

@test "--round out of range and missing required args -> exit 2" {
    run "$PY" "$PV" --subject 7 --round 0 --items-dir items
    [ "$status" -eq 2 ]
    run "$PY" "$PV" --round 1 --items-dir items
    [ "$status" -eq 2 ]
    run "$PY" "$PV" --subject 7 --items-dir items
    [ "$status" -eq 2 ]
}

# ---------- independence (--authoring-harness) ----------

@test "authoring-harness leg is dropped from the score; independent leg survives" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-codex-sol.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items --authoring-harness claude
    [ "$status" -eq 0 ]
    [[ "$output" == *"DROPPED from the score"*"5-review-1-claude-opus.md"* ]]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
samples = json.load(sys.stdin)["candidates"][0]["samples"]
assert len(samples) == 1, samples
'
}

@test "all legs from the authoring harness, none vetoing -> exit 3, no score emitted" {
    # The pilot item shape: 2/2 live legs on the author own harness. Scoring
    # those is the self-scoring the verifier plan forbids.
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    _leg 5-review-1-claude-sonnet.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items --authoring-harness claude
    [ "$status" -eq 3 ]
    [[ "$output" == *"no independent leg"* ]]
    [[ "$output" == *"presence-only ack"* ]]
    # `! grep -q`, not `grep -qv`: -v inverts PER LINE, so the diagnostic lines
    # would satisfy it even if a candidates blob were emitted alongside them.
    ! echo "$output" | grep -q '"candidates"'
}

@test "authoring-harness leg that VETOES is kept (self-reported bad news is trusted)" {
    _leg 5-review-1-claude-opus.md "$VETO_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items --authoring-harness claude
    [ "$status" -eq 0 ]
    [[ "$output" == *"VETOES — kept"* ]]
    echo "$output" | grep -v '^panel-verdicts:' | "$PY" -c '
import json, sys
samples = json.load(sys.stdin)["candidates"][0]["samples"]
assert len(samples) == 1 and samples[0]["hard_veto"] is True, samples
'
}

@test "unreadable harness tag counts as NOT independent (fail closed)" {
    # Off-grammar name: provenance unknown, so it must not be trusted as an
    # independent backend just because it fails to match the author tag.
    _leg 5-review-1-weird_name.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items --authoring-harness claude
    [ "$status" -eq 3 ]
    [[ "$output" == *"harness tag unreadable"* ]]
}

@test "without --authoring-harness: back-compatible, but says independence is unchecked" {
    _leg 5-review-1-claude-opus.md "$OK_VERDICT"
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items
    [ "$status" -eq 0 ]
    [[ "$output" == *"independence UNCHECKED"* ]]
}

@test "all legs missing trailers WITH --authoring-harness -> exit 2, not the rc-3 degraded path" {
    # rc 3 means "legs existed, none independent" (record a presence-only ack).
    # Zero PARSED legs is the FATAL case and must stay exit 2 — otherwise a
    # round where the panel wrote no verdicts at all reads as a routine
    # degraded roster. Only reachable with the flag set, which is why the
    # no-flag sibling case above did not catch it.
    _leg 5-review-1-claude-opus.md ""
    _leg 5-review-1-codex-sol.md ""
    run "$PY" "$PV" --subject 7 --round 1 --items-dir items --authoring-harness claude
    [ "$status" -eq 2 ]
    [[ "$output" == *"0 usable verdicts"* ]]
    [[ "$output" != *"no independent leg"* ]]
}
