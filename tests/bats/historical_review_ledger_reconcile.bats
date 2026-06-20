#!/usr/bin/env bats
# historical_review_ledger_reconcile.bats — regression suite for
# agents/scripts/core/historical-review-ledger-reconcile.sh (PR-8: the ledger
# staleness reconcile pass + SessionStart freshness nudge).
#
# Covers: --selftest passes; --nudge SILENT when the ledger is fresh and FIRES
# when the newest dated section ages past LEDGER_STALE_DAYS; --reconcile parses
# only the STILL-OPEN block. No network — the reconcile probes are not exercised
# here (covered by the script's own --selftest of its pure parsers); these tests
# pin the nudge age-gate + mode dispatch. @test names are ASCII-only (Windows
# bats silently skips a non-ASCII test name).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    SCRIPT="$REPO_ROOT/agents/scripts/core/historical-review-ledger-reconcile.sh"
    TMP="$(mktemp -d)"
    LEDGER="$TMP/ledger.md"
}

teardown() {
    rm -rf "$TMP"
}

# write_ledger <newest-section-date> — a minimal ledger with one ## section dated
# <date> and a STILL-OPEN block holding one finding (#329).
write_ledger() {
    cat > "$LEDGER" <<EOF
# Historical code-review findings

## Verification pass ($1)
text

**STILL OPEN (NOT DONE) — re-verified alive at develop $1:**

_Tooling backlog:_
- **#329** (HIGH) \`baz.sh:30\` — leak gate.

**Not individually re-verified this pass:** the rest.
EOF
}

@test "selftest passes" {
    run bash "$SCRIPT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "nudge is silent when the ledger is fresh" {
    write_ledger "2026-06-20"
    HRL_LEDGER="$LEDGER" HRL_TODAY="2026-06-21" LEDGER_STALE_DAYS=14 \
        run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "nudge fires when the newest section is older than the threshold" {
    write_ledger "2026-06-01"
    HRL_LEDGER="$LEDGER" HRL_TODAY="2026-06-30" LEDGER_STALE_DAYS=14 \
        run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"historical-review ledger stale"* ]]
    [[ "$output" == *"--reconcile"* ]]
}

@test "nudge silent on a missing ledger" {
    HRL_LEDGER="$TMP/does-not-exist.md" run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "reconcile reports nothing when STILL-OPEN block is absent" {
    cat > "$LEDGER" <<'EOF'
# Findings
## Batch 1 — PRs #947-951 (swept 2026-06-07)
- **#947** (HIGH) a finding.
EOF
    HRL_LEDGER="$LEDGER" run bash "$SCRIPT" --reconcile
    [ "$status" -eq 0 ]
    [[ "$output" == *"no STILL-OPEN findings parsed"* ]]
}

@test "bad mode arg exits 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}
