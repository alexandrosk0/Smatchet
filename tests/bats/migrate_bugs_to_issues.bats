#!/usr/bin/env bats
# tests/bats/migrate_bugs_to_issues.bats
# Coverage for agents/scripts/project/migrate-bugs-to-issues.sh (issue-triage Slice 3).
# Tests the gh-free surface: the bug-vs-debt classifier + --dry-run (creates nothing).
# The live gh create/dedup path runs only under --apply (not exercised here).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPT="$REPO_ROOT/agents/scripts/project/migrate-bugs-to-issues.sh"
    WORK="$BATS_TEST_TMPDIR"; export WORK
    # Exec-validate the interpreter: a bare `command -v python3` matches the
    # Windows WinStore alias stub, which resolves on PATH but exits non-zero on
    # run — the skip guard then never fires and every verdict assertion fails.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
    # Minimal fixture bug.md: one safety bug (race), one debt (god-object), one ambiguous.
    cat > "$WORK/bug.md" <<'MD'
# bug
- 2026-06-01 · code-review · [bug] · P2 — field mutated unlocked while another thread writes it (data race)
  Details: a race between readers and writers.
  Status: open
- 2026-06-01 · deep-audit · [bug] · P3 — Foo is a ~1000-line god-object (code-health)
  Details: refactor candidate.
  Status: open
- 2026-06-01 · code-review · [bug] · P2 — three pre-existing logic issues surfaced by review
  Details: vague.
  Status: open
MD
    export BUG_MD="$WORK/bug.md"
}

@test "dry-run classifies safety->bug, god-object->debt, vague->ambiguous" {
    BUG_MD="$BUG_MD" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 → GitHub Issues"* ]]
    [[ "$output" == *"1 → debt.md"* ]]
    [[ "$output" == *"1 ambiguous"* ]]
    [[ "$output" == *"ISSUE"*"data race"* ]]
    [[ "$output" == *"DEBT"*"god-object"* ]]
    [[ "$output" == *"AMBIG"* ]]
}

@test "dry-run creates nothing and says so" {
    BUG_MD="$BUG_MD" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"DRY-RUN — no Issues created"* ]]
}

@test "a refactor mention in a bug's body does NOT flip it to debt (safety wins)" {
    cat > "$WORK/b2.md" <<'MD'
- 2026-06-01 · code-review · [bug] · P1 — UB on silent packets crashes capture
  Details: this was left as a behaviour-preserving refactor for now.
  Status: open
MD
    BUG_MD="$WORK/b2.md" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 → GitHub Issues"* ]]
    [[ "$output" == *"0 → debt.md"* ]]
}

@test "bad arg exits 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}
