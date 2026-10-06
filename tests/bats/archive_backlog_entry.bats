#!/usr/bin/env bats
# tests/bats/archive_backlog_entry.bats
# ----------------------------------------------------------------------------
# Coverage for agents/scripts/core/archive-backlog-entry.sh
# (per-entry-archival-breaks-relative-links, tooling P2).
#
# THE POINT OF THESE TESTS is that the bug being fixed was SILENT in both
# directions, so "it ran without error" proves nothing:
#   - OUTBOUND: `cat` cannot fail, so a body appended at the wrong depth looks
#     like a clean archival. The only signal was the docs gate, later.
#   - INBOUND: the `git rm` orphans referrers the archiver never opened. Those
#     files are unmodified, so a diff-scoped check cannot see it — it surfaced as
#     a red required check.
# Every test therefore asserts on the RESULTING LINK TARGETS, not on exit status.
#
# Each test runs in a throwaway git repo laid out like the real docs tree, so the
# archival is exercised end to end (append + git rm + staging) without touching
# the real backlog. ARCHIVE_SKIP_LINK_CHECK=1 because the built-in self-check
# scans the REAL repo, which a fixture archival must not depend on.
# ARCHIVE_SKIP_ROTATE=1 because rotation would move the fixture's dated entries out
# of applied.md — and, before rotate-applied-md.sh resolved its root from cwd, it
# rotated the REAL applied.md on every run of this suite.
#
# Requires: bash, git, python3, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/archive-backlog-entry.sh"
    export SCRIPT
    [ -r "$SCRIPT" ]
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR/repo"; export WORK
    mkdir -p "$WORK/docs/self-improvement/categories/process" \
             "$WORK/docs/self-improvement/categories/tooling" \
             "$WORK/docs/agent-rules" \
             "$WORK/agents/scripts/core/lib"
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t.t
    git -C "$WORK" config user.name t

    # Link targets that must still resolve after the move.
    : > "$WORK/docs/agent-rules/ship-loops.md"
    : > "$WORK/agents/scripts/core/lib/script-freshness.sh"
    : > "$WORK/docs/self-improvement/categories/tooling/2026-01-01-sibling.md"
    printf '# applied ledger\n' > "$WORK/docs/self-improvement/categories/applied.md"

    ENTRY="docs/self-improvement/categories/process/2026-01-02-subject.md"
    export ENTRY
    cat > "$WORK/$ENTRY" <<'MD'
- 2026-01-02 · claude-code · [process] · P2 — a subject

  Details: see [`ship-loops.md`](../../../agent-rules/ship-loops.md) and
  [`script-freshness.sh`](../../../../agents/scripts/core/lib/script-freshness.sh)
  and the sibling [entry](../tooling/2026-01-01-sibling.md) and the
  [archive](../applied.md). External [x](https://example.com) stays put.

  Status: applied
MD
    git -C "$WORK" add -A
    git -C "$WORK" commit -qm init
}

_run_archive() {  # <args...>
    ( cd "$WORK" && ARCHIVE_SKIP_LINK_CHECK=1 ARCHIVE_SKIP_ROTATE=1 bash "$SCRIPT" "$@" )
}

# ── Outbound: the body is re-depthed for its new home ────────────────────────

@test "outbound: a parent-escaping link loses exactly one level" {
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    # Authored from categories/process/, now read from categories/.
    grep -q '](../../agent-rules/ship-loops.md)' "$WORK/docs/self-improvement/categories/applied.md"
    grep -q '](../../../agents/scripts/core/lib/script-freshness.sh)' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "outbound: a sibling-category link drops its ../" {
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](tooling/2026-01-01-sibling.md)' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "outbound: a link to applied.md itself becomes same-dir" {
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](applied.md)' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "outbound: absolute/external links are left alone" {
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](https://example.com)' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "outbound: every re-depthed link actually resolves from applied.md" {
    # The assertion that would have caught the original bug outright: resolve each
    # relative target against applied.md's own directory and require it to exist.
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    cd "$WORK/docs/self-improvement/categories"
    missing=0
    while IFS= read -r target; do
        case "$target" in http*|"#"*) continue ;; esac
        t="${target%%#*}"
        [ -n "$t" ] || continue
        if [ ! -e "$t" ]; then echo "DANGLING: $t" >&2; missing=$((missing+1)); fi
    done < <(grep -o '](\([^)]*\))' applied.md | sed 's/^](//; s/)$//')
    [ "$missing" -eq 0 ]
}

# ── Inbound: referrers are repointed, never orphaned ─────────────────────────

@test "inbound: a referring doc is repointed at applied.md at its own depth" {
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md) for detail.
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    # From docs/self-improvement/, applied.md is categories/applied.md.
    grep -q '](categories/applied.md)' "$WORK/docs/self-improvement/postmortems.md"
    ! grep -q '2026-01-02-subject.md)' "$WORK/docs/self-improvement/postmortems.md"
}

@test "inbound: a sibling entry at a different depth gets its own correct path" {
    cat > "$WORK/docs/self-improvement/categories/tooling/2026-01-03-other.md" <<'MD'
Related: [subject](../process/2026-01-02-subject.md).
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    # From categories/tooling/, applied.md is ../applied.md — NOT the same string
    # the postmortems.md referrer needed. A fixed replacement would break one.
    grep -q '](../applied.md)' "$WORK/docs/self-improvement/categories/tooling/2026-01-03-other.md"
}

@test "inbound: no inbound reference survives pointing at the deleted file" {
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md).
MD
    cat > "$WORK/docs/self-improvement/categories/tooling/2026-01-03-other.md" <<'MD'
Related: [subject](../process/2026-01-02-subject.md).
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm refs
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    [ ! -e "$WORK/$ENTRY" ]
    # Nothing anywhere still LINKS to the removed path.
    ! grep -rq '](.*2026-01-02-subject\.md)' "$WORK/docs"
}

@test "inbound: a prose/code-span mention is reported but does not block" {
    # Code spans are not followed by the link gate, so they cannot dangle — only a
    # human can restate them. Advisory, never a refusal.
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See `2026-01-02-subject.md` for detail.
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    [[ "$output" == *"INBOUND-PROSE"* ]]
}

# ── Mechanics ────────────────────────────────────────────────────────────────

@test "the source file is removed and both sides are staged" {
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    [ ! -e "$WORK/$ENTRY" ]
    staged="$(git -C "$WORK" diff --cached --name-only)"
    [[ "$staged" == *"categories/applied.md"* ]]
    [[ "$staged" == *"2026-01-02-subject.md"* ]]
}

@test "staging does not sweep in unrelated modified files" {
    # `git add -u` would have staged this; the script stages only the referrers it
    # actually rewrote.
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md).
MD
    echo "unrelated in-progress edit" >> "$WORK/docs/agent-rules/ship-loops.md"
    git -C "$WORK" add "$WORK/docs/self-improvement/postmortems.md"
    git -C "$WORK" commit -qm ref -- docs/self-improvement/postmortems.md
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    ! git -C "$WORK" diff --cached --name-only | grep -q 'ship-loops.md'
}

@test "--dry-run writes nothing" {
    before="$(cat "$WORK/docs/self-improvement/categories/applied.md")"
    run _run_archive --dry-run "$ENTRY"
    [ "$status" -eq 0 ]
    [ -e "$WORK/$ENTRY" ]
    [ "$(cat "$WORK/docs/self-improvement/categories/applied.md")" = "$before" ]
}

@test "--dry-run does not rewrite inbound referrers either" {
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md).
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive --dry-run "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '2026-01-02-subject.md)' "$WORK/docs/self-improvement/postmortems.md"
}

@test "a path outside the per-entry layout is refused" {
    run _run_archive docs/self-improvement/categories/applied.md
    [ "$status" -eq 2 ]
}

@test "a missing file is refused" {
    run _run_archive docs/self-improvement/categories/process/no-such.md
    [ "$status" -eq 2 ]
}

@test "--selftest passes" {
    run _run_archive --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "an entry with uncommitted local modifications still archives" {
    # THE NORMAL CASE, and the one that failed on first real use: you flip
    # `Status: open` -> `applied` and archive in the same session, so the file is
    # dirty when `git rm` runs. Plain `git rm` refuses that, and it refused AFTER
    # the append had already landed — leaving the tree half-archived.
    printf '\n  Status: applied\n' >> "$WORK/$ENTRY"
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    [ ! -e "$WORK/$ENTRY" ]
    # The WORKING-TREE body is what must be preserved, not the committed one.
    grep -q 'Status: applied' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "an untracked entry archives without invoking git rm" {
    cp "$WORK/$ENTRY" "$WORK/docs/self-improvement/categories/process/2026-01-04-untracked.md"
    run _run_archive docs/self-improvement/categories/process/2026-01-04-untracked.md
    [ "$status" -eq 0 ]
    [ ! -e "$WORK/docs/self-improvement/categories/process/2026-01-04-untracked.md" ]
}

@test "a refused archival leaves applied.md untouched" {
    # The half-archived state is the failure mode worth pinning: a late refusal
    # must not have already appended, or a retry double-appends.
    before="$(cat "$WORK/docs/self-improvement/categories/applied.md")"
    run _run_archive docs/self-improvement/categories/process/no-such.md
    [ "$status" -ne 0 ]
    [ "$(cat "$WORK/docs/self-improvement/categories/applied.md")" = "$before" ]
}

# ── Regressions from the pre-first-push review ───────────────────────────────

@test "titled links [x](path \"Title\") are re-depthed, not skipped" {
    # A parser that matches only `([^)\s]+)\)` silently SKIPS the CommonMark
    # titled form, leaving the target at the wrong depth at exit 0. The repo's own
    # test-markdown-links.sh does handle titles, so the two would disagree.
    cat > "$WORK/$ENTRY" <<'MD'
- entry

  See [ship-loops](../../../agent-rules/ship-loops.md "The Ship Loops") for detail.
MD
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](../../agent-rules/ship-loops.md "The Ship Loops")' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "a titled inbound link is repointed, not orphaned" {
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md "Subject") for detail.
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](categories/applied.md "Subject")' "$WORK/docs/self-improvement/postmortems.md"
    ! grep -q '2026-01-02-subject.md' "$WORK/docs/self-improvement/postmortems.md"
}

@test "an UNTRACKED referrer is repointed too" {
    # A doc written and archived in the same session is not in git ls-files yet.
    # Missing it orphans exactly the link this script exists to protect.
    cat > "$WORK/docs/self-improvement/brand-new.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md).
MD
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '](categories/applied.md)' "$WORK/docs/self-improvement/brand-new.md"
}

@test "a refusal happens before ANY file is written" {
    # The inbound pass rewrites referrers on disk; ordering it before the refusal
    # checks leaves the tree half-mutated at a non-zero exit.
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry](categories/process/2026-01-02-subject.md).
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    : > "$WORK/$ENTRY"          # empty body -> refused
    before_ref="$(cat "$WORK/docs/self-improvement/postmortems.md")"
    before_applied="$(cat "$WORK/docs/self-improvement/categories/applied.md")"
    run _run_archive "$ENTRY"
    [ "$status" -ne 0 ]
    [ -e "$WORK/$ENTRY" ]
    [ "$(cat "$WORK/docs/self-improvement/postmortems.md")" = "$before_ref" ]
    [ "$(cat "$WORK/docs/self-improvement/categories/applied.md")" = "$before_applied" ]
}

@test "reference-definition links [id]: path are re-depthed too" {
    # REFDEF_RE rewrites these, but nothing pinned the shape — an untested branch
    # of the parser is exactly where the depth bug reappears.
    cat > "$WORK/$ENTRY" <<'MD'
- entry

  See [ship-loops][sl] and [sibling][sib].

  [sl]: ../../../agent-rules/ship-loops.md
  [sib]: ../tooling/2026-01-01-sibling.md
MD
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '\[sl\]: ../../agent-rules/ship-loops.md' "$WORK/docs/self-improvement/categories/applied.md"
    grep -q '\[sib\]: tooling/2026-01-01-sibling.md' "$WORK/docs/self-improvement/categories/applied.md"
}

@test "an inbound reference-definition link is repointed too" {
    cat > "$WORK/docs/self-improvement/postmortems.md" <<'MD'
See [the entry][e] for detail.

[e]: categories/process/2026-01-02-subject.md
MD
    git -C "$WORK" add -A && git -C "$WORK" commit -qm ref
    run _run_archive "$ENTRY"
    [ "$status" -eq 0 ]
    grep -q '\[e\]: categories/applied.md' "$WORK/docs/self-improvement/postmortems.md"
}

@test "exit 4: a post-archive link check failure is reported, not swallowed" {
    # The self-check is the backstop that would have caught both halves of the
    # original bug. If it can fail without changing the exit code, it is
    # decorative. Runs WITHOUT ARCHIVE_SKIP_LINK_CHECK, against a stub checker, so
    # this pins the script's handling of a red self-check rather than the
    # checker's own logic. (Verified non-vacuous: a stub that exits 0 makes the
    # whole archival exit 0, so the 4 genuinely comes from the check.)
    mkdir -p "$WORK/fake"
    cat > "$WORK/fake/test-markdown-links.sh" <<'STUB'
#!/usr/bin/env bash
echo "FAKE-LINK-CHECK: dangling"
exit 1
STUB
    cp "$SCRIPT" "$WORK/fake/archive-backlog-entry.sh"
    run bash -c "cd '$WORK' && bash '$WORK/fake/archive-backlog-entry.sh' '$ENTRY'"
    [ "$status" -eq 4 ]
    [[ "$output" == *"post-archive link check FAILED"* ]]
}

# ── Rotation + sort: both entry shapes (applied-md-rotation-duplicates-and-misfiles) ──
# applied.md holds legacy `- YYYY-MM-DD · …` blocks AND per-entry-file blocks
# (`# <title>` + a `**Date**:` metadata paragraph) appended by this archiver. The
# legacy-only splitter glued each per-entry block onto the dated block above it,
# so it rotated and sorted under that block's date. ROTATE_APPLIED_TODAY pins the
# month boundary: with 2026-05-15 the head keeps 2026-05 + 2026-04.

ROT_TODAY=2026-05-15

_cat_dir() { echo "$WORK/docs/self-improvement/categories"; }

_run_rotate() {  # <args...>
    ( cd "$WORK" && ROTATE_APPLIED_TODAY="$ROT_TODAY" \
        bash "$REPO_ROOT/agents/scripts/core/rotate-applied-md.sh" "$@" )
}

_run_sort() {  # <args...>
    SMATCHET_APPLIED_MD="$(_cat_dir)/applied.md" \
        bash "$REPO_ROOT/agents/scripts/core/sort-applied-md.sh" "$@"
}

# A bare `! grep` mid-test never fails a bats test (errexit ignores negated
# commands); a function whose status is the negation does.
_absent() {  # <pattern> <file...>
    ! grep -q "$@"
}

# Fixture blocks. A per-entry block carries `## ` sections and a fenced
# `# comment`, neither of which may split it.
_legacy() {  # <date> <slug>
    printf -- '- %s · agent · [tooling] · P2 — %s\n  Status: applied\n' "$1" "$2"
}
_per_entry() {  # <date> <slug>
    cat <<MD
# Per-entry $2

- **Category**: tooling
- **Date**: $1

## What happened

\`\`\`bash
# a comment line inside a fence, not a title
echo $2
\`\`\`

## Status

Applied.
MD
}

_head() {  # <blocks...> — writes applied.md: header + blank-separated blocks
    {
        printf '# applied ledger\n\n> Header prose.\n\n<!-- Latest first. -->\n'
        local b
        for b in "$@"; do printf '\n%s\n' "$b"; done
    } > "$(_cat_dir)/applied.md"
}

_partition() {  # <YYYY-MM> <blocks...>
    local month="$1"; shift
    {
        printf '# Agent self-improvement — applied (archive partition %s)\n\n> Rotated slice.\n' "$month"
        local b
        for b in "$@"; do printf '\n%s\n' "$b"; done
    } > "$(_cat_dir)/applied-$month.md"
}

@test "rotate: a per-entry block rotates by its own date, not the legacy block above it" {
    # An old (2026-02) per-entry block glued below a current legacy entry, and a
    # current per-entry block glued below an old legacy entry.
    _head "$(_legacy 2026-05-02 current-legacy)" "$(_per_entry 2026-02-11 old-block)" \
          "$(_legacy 2026-03-01 old-legacy)" "$(_per_entry 2026-04-20 current-block)"
    run _run_rotate
    [ "$status" -eq 0 ]
    cat="$(_cat_dir)"
    grep -q 'Per-entry old-block' "$cat/applied-2026-02.md"
    grep -q 'old-legacy' "$cat/applied-2026-03.md"
    _absent 'Per-entry' "$cat/applied-2026-03.md"
    grep -q 'current-legacy' "$cat/applied.md"
    grep -q 'Per-entry current-block' "$cat/applied.md"
    _absent 'old-block\|old-legacy' "$cat/applied.md"
    # The block moved whole: its fenced `# comment` and `## ` sections came along.
    grep -q 'a comment line inside a fence' "$cat/applied-2026-02.md"
    grep -q '^## Status' "$cat/applied-2026-02.md"
}

@test "rotate: each per-entry metadata shape is read for the date" {
    # `- **Date:**`, a combined `**Category** · **Filed**:` line, bare
    # `**Date**:` under a `## ` title, and a legacy summary line under a title.
    _head "$(_legacy 2026-05-02 anchor)" \
          "$(printf '# Colon-inside\n\n- **Category:** infra\n- **Date:** 2026-01-05\n\n## Gap\n\nprose\n')" \
          "$(printf '# Combined\n\n- **Category**: debt · **Priority**: P3 · **Filed**: 2026-02-06 (migrated)\n\n## Problem\n\nprose\n')" \
          "$(printf '## [P1] Bare\n\n**Category**: tooling\n**Date**: 2026-03-07\n\n### What\n\nprose\n')" \
          "$(printf -- '# Summary-line\n\n- 2026-03-08 · agent · [test] · P1 — summary\n\n## Friction\n\nprose\n')"
    run _run_rotate
    [ "$status" -eq 0 ]
    cat="$(_cat_dir)"
    grep -q '^# Colon-inside' "$cat/applied-2026-01.md"
    grep -q '^# Combined' "$cat/applied-2026-02.md"
    grep -q '^## \[P1\] Bare' "$cat/applied-2026-03.md"
    grep -q '^# Summary-line' "$cat/applied-2026-03.md"
    # The summary line stayed under its title rather than opening an entry of its own.
    grep -A2 '^# Summary-line' "$cat/applied-2026-03.md" | grep -q '^- 2026-03-08 · agent'
    [ "$(grep -c '^- 2026\|^#' "$cat/applied.md")" -eq 2 ]   # header title + anchor
}

@test "rotate: an entry whose only copy is in the head survives the duplicate-drop" {
    # Same title line as a partitioned entry but a different body (a later
    # Status update): not a copy, so it lands in the partition, never vanishes.
    _partition 2026-03 "$(_legacy 2026-03-01 shared)"
    _head "$(_legacy 2026-05-02 anchor)" \
          "$(printf -- '- 2026-03-01 · agent · [tooling] · P2 — shared\n  Status: applied (updated later)\n')" \
          "$(_per_entry 2026-03-09 head-only)"
    run _run_rotate
    [ "$status" -eq 0 ]
    part="$(_cat_dir)/applied-2026-03.md"
    grep -q 'updated later' "$part"
    grep -q 'Per-entry head-only' "$part"
    [ "$(grep -c '^- 2026-03-01 · agent' "$part")" -eq 2 ]
    _absent 'shared\|head-only' "$(_cat_dir)/applied.md"
}

@test "rotate: an already-partitioned duplicate is dropped, not written twice" {
    # The 2026-10-03 state: the head still held entries already in their
    # partitions, and held one entry twice.
    _partition 2026-03 "$(_legacy 2026-03-01 partitioned)" "$(_per_entry 2026-03-02 partitioned-block)"
    _head "$(_legacy 2026-05-02 anchor)" \
          "$(_legacy 2026-03-01 partitioned)" "$(_per_entry 2026-03-02 partitioned-block)" \
          "$(_legacy 2026-02-03 twice)" "$(_legacy 2026-02-03 twice)"
    run _run_rotate
    [ "$status" -eq 0 ]
    [[ "$output" == *"2 already present, skipped"* ]]
    cat="$(_cat_dir)"
    [ "$(grep -c 'partitioned$' "$cat/applied-2026-03.md")" -eq 1 ]
    [ "$(grep -c '^# Per-entry partitioned-block' "$cat/applied-2026-03.md")" -eq 1 ]
    [ "$(grep -c 'twice$' "$cat/applied-2026-02.md")" -eq 1 ]
    _absent 'partitioned\|twice' "$cat/applied.md"
}

@test "rotate: a block misfiled in a partition is re-homed under its own month" {
    # What the legacy-only splitter left behind: a 2026-03 per-entry block filed
    # in 2026-02, and a 2026-04 one (still a head month) filed in 2026-01.
    _partition 2026-02 "$(_legacy 2026-02-01 feb)" "$(_per_entry 2026-03-04 misfiled-march)"
    _partition 2026-01 "$(_legacy 2026-01-01 jan)" "$(_per_entry 2026-04-04 misfiled-april)"
    _head "$(_legacy 2026-05-02 may)" "$(_legacy 2026-04-01 april)"
    run _run_rotate --check
    [ "$status" -eq 1 ]
    [[ "$output" == *"2 partition entr(ies) filed under another month"* ]]
    run _run_rotate
    [ "$status" -eq 0 ]
    cat="$(_cat_dir)"
    _absent 'misfiled' "$cat/applied-2026-02.md" "$cat/applied-2026-01.md"
    grep -q 'Per-entry misfiled-march' "$cat/applied-2026-03.md"
    grep -q 'feb$' "$cat/applied-2026-02.md"
    grep -q 'jan$' "$cat/applied-2026-01.md"
    # Re-homed into the head in date order: 05-02, 04-04, 04-01.
    order="$(grep -o '^- 2026-0[0-9]-0[0-9]\|^# Per-entry [a-z-]*' "$cat/applied.md" | tr '\n' ' ')"
    [ "$order" = "- 2026-05-02 # Per-entry misfiled-april - 2026-04-01 " ]
}

@test "rotate: a crash between file writes never loses a moving block (destination first)" {
    # Blocks move partition -> partition (2026-02 -> new 2026-03), partition ->
    # head (2026-01 -> head) and head -> partition (2026-03-05). The pre-fix order
    # wrote 2026-01 without misfiled-april before the head gained it, so a crash
    # right there lost the block. Crash after every possible write in turn.
    _crash_fixture() {
        rm -f "$(_cat_dir)"/applied*.md
        _partition 2026-02 "$(_legacy 2026-02-01 feb)" "$(_per_entry 2026-03-04 misfiled-march)"
        _partition 2026-01 "$(_legacy 2026-01-01 jan)" "$(_per_entry 2026-04-04 misfiled-april)"
        _head "$(_legacy 2026-05-02 may)" "$(_legacy 2026-04-01 april)" "$(_legacy 2026-03-05 head-march)"
    }
    _copies() { cat "$(_cat_dir)"/applied*.md | grep -c -e "$1"; }
    pats=('— feb$' '— jan$' '— may$' '— april$' '— head-march$'
          '^# Per-entry misfiled-march' '^# Per-entry misfiled-april')
    n=1
    while :; do
        _crash_fixture
        ROTATE_APPLIED_CRASH_AFTER=$n run _run_rotate
        [ "$status" -eq 3 ] || break
        for p in "${pats[@]}"; do [ "$(_copies "$p")" -ge 1 ]; done
        # The next ordinary run converges: every block exactly once.
        run _run_rotate
        [ "$status" -eq 0 ]
        for p in "${pats[@]}"; do [ "$(_copies "$p")" -eq 1 ]; done
        n=$(( n + 1 ))
    done
    [ "$status" -eq 0 ]
    [ "$n" -gt 3 ]   # several writes, each one crash-tested
    grep -q 'Per-entry misfiled-april' "$(_cat_dir)/applied.md"
    grep -q 'Per-entry misfiled-march' "$(_cat_dir)/applied-2026-03.md"
    grep -q 'head-march$' "$(_cat_dir)/applied-2026-03.md"
}

@test "rotate: a legacy-only ledger rotates exactly as before, and a second run is a no-op" {
    _head "$(_legacy 2026-05-02 may)" "$(_legacy 2026-04-01 april)" \
          "$(_legacy 2026-03-02 march-b)" "$(_legacy 2026-03-01 march-a)"
    run _run_rotate
    [ "$status" -eq 0 ]
    cat="$(_cat_dir)"
    expected="$(printf -- '- 2026-03-02 · agent · [tooling] · P2 — march-b\n  Status: applied\n\n- 2026-03-01 · agent · [tooling] · P2 — march-a\n  Status: applied')"
    [ "$(sed -n '/^- 2026-03-02/,$p' "$cat/applied-2026-03.md")" = "$expected" ]
    grep -q '^# Agent self-improvement — applied (archive partition 2026-03)' "$cat/applied-2026-03.md"
    [ "$(grep -c '^- 2026' "$cat/applied.md")" -eq 2 ]
    before="$(cat "$cat/applied.md" "$cat/applied-2026-03.md")"
    run _run_rotate
    [ "$status" -eq 0 ]
    [[ "$output" == *"head is bounded"* ]]
    [ "$(cat "$cat/applied.md" "$cat/applied-2026-03.md")" = "$before" ]
    run _run_rotate --check
    [ "$status" -eq 0 ]
}

@test "rotate: section headings that are not entries stay with their block" {
    # A `## Parked` divider has prose (not entry metadata) under it, so it does
    # not open an entry — it stays glued to the current entry above it.
    _head "$(_legacy 2026-05-02 may)" \
          "$(printf '## Parked\n\n> P3 entries with no owner.\n')" \
          "$(_legacy 2026-04-01 april)"
    run _run_rotate --check
    [ "$status" -eq 0 ]
    run _run_rotate
    [ "$status" -eq 0 ]
    [ -z "$(find "$(_cat_dir)" -name 'applied-2026-*.md')" ]
    grep -q '^## Parked' "$(_cat_dir)/applied.md"
}

@test "sort: a per-entry block sorts by its own date" {
    _head "$(_legacy 2026-05-10 ten)" "$(_per_entry 2026-05-12 twelve)" "$(_legacy 2026-05-01 one)"
    run _run_sort --check
    [ "$status" -eq 1 ]
    run _run_sort
    [ "$status" -eq 0 ]
    order="$(grep -o 'ten$\|Per-entry twelve$\|one$' "$(_cat_dir)/applied.md" | tr '\n' ' ')"
    [ "$order" = "Per-entry twelve ten one " ]
    # The block moved whole, fence and sections included.
    [ "$(sed -n '/^# Per-entry twelve/,/^- 2026-05-10/p' "$(_cat_dir)/applied.md" | grep -c '^## ')" -eq 2 ]
    run _run_sort --check
    [ "$status" -eq 0 ]
}

@test "sort: a legacy-only file sorts exactly as before" {
    _head "$(_legacy 2026-05-01 one)" "$(_legacy 2026-05-03 three)" "$(_legacy 2026-05-02 two)"
    run _run_sort
    [ "$status" -eq 0 ]
    order="$(grep -o 'one$\|two$\|three$' "$(_cat_dir)/applied.md" | tr '\n' ' ')"
    [ "$order" = "three two one " ]
    head -1 "$(_cat_dir)/applied.md" | grep -q '^# applied ledger'
    run _run_sort --check
    [ "$status" -eq 0 ]
}
