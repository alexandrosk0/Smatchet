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
    ( cd "$WORK" && ARCHIVE_SKIP_LINK_CHECK=1 bash "$SCRIPT" "$@" )
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
