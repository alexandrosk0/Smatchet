#!/usr/bin/env bash
# test-archive-plan.sh — hermetic selftest for archive-plan.sh.
#
# Builds a THROWAWAY git repo fixture (never touches a real plan), seeds a plan
# in active/ plus inbound refs across >=3 file kinds, runs archive-plan.sh
# inside it, and asserts the move + the tier-less plain-text rewrite + the
# markdown-hyperlink dangle detection + the clean-fixture gate-pass behaviours.
#
# Modeled on test-plan-index.sh / test-plan-ref-integrity.sh's --selftest: the
# fixture is a self-contained git repo with the core scripts copied in, run via
# the script's own `git rev-parse --show-toplevel` re-rooting, asserted by exit
# code + grepping the moved tree. Hermetic + idempotent; leaves no residue.
#
# Auto-enrolled by scripts/dev/test-all.sh via the test-*.sh glob.
#
# Exit codes (test-author convention):
#   0 — all assertions passed.
#   1 — at least one assertion failed.
#   2 — missing prerequisite (git / python / sibling scripts).

set -uo pipefail

cd "$(git rev-parse --show-toplevel)" || exit 2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUT="$SCRIPT_DIR/archive-plan.sh"
IDX="$SCRIPT_DIR/test-plan-index.sh"
REF="$SCRIPT_DIR/test-plan-ref-integrity.sh"
MDL="$SCRIPT_DIR/test-markdown-links.sh"
for s in "$SUT" "$IDX" "$REF" "$MDL"; do
    [ -f "$s" ] || { echo "test-archive-plan: missing script $s" >&2; exit 2; }
done

command -v git >/dev/null 2>&1 || { echo "test-archive-plan: git not found" >&2; exit 2; }

PASS=0
FAIL=0
ok()   { echo "  ok   - $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL - $1" >&2; FAIL=$((FAIL + 1)); }

# scaffold_repo <dir> — lay down a self-contained fixture repo with the SUT +
# its sibling gate scripts copied to their canonical paths, an INDEX.md with the
# auto-plan-index markers, and the three plan tiers.
scaffold_repo() {
    local d="$1"
    (
        cd "$d" || exit 99
        git init -q
        git config user.email t@t
        git config user.name t
        git config commit.gpgsign false
        mkdir -p docs/plans/active docs/plans/shipped docs/plans/deferred agents/scripts/core
        cp "$SUT" agents/scripts/core/archive-plan.sh
        cp "$IDX" agents/scripts/core/test-plan-index.sh
        cp "$REF" agents/scripts/core/test-plan-ref-integrity.sh
        cp "$MDL" agents/scripts/core/test-markdown-links.sh
        chmod +x agents/scripts/core/*.sh
        # INDEX.md with the markers test-plan-index.sh rewrites between.
        cat > docs/plans/INDEX.md <<'IDXEOF'
# Plan index

<!-- BEGIN auto-plan-index -->
<!-- END auto-plan-index -->

End.
IDXEOF
    )
}

# A clean fixture: target plan exists in shipped/, no dangling refs. Used to
# prove the gates PASS on a well-formed tree (independent of archival).
make_clean_fixture() {
    local d="$1"
    scaffold_repo "$d"
    (
        cd "$d" || exit 99
        printf '# Plan — already shipped\n\nbody\n' > docs/plans/shipped/already.md
        printf '# Sibling\n\nsee docs/plans/already.md (tier-less, resolves)\n' > docs/plans/active/sibling.md
        # origin/develop ref needed by test-markdown-links --diff: create a
        # branch and point a fake origin/develop at the initial commit so the
        # diff-scope has a baseline.
        git add -A
        git commit -qm init
        git branch -f develop
        git update-ref refs/remotes/origin/develop refs/heads/develop
    )
}

# The archival fixture: a plan in active/ with inbound refs in >=3 file kinds.
make_archive_fixture() {
    local d="$1"
    scaffold_repo "$d"
    (
        cd "$d" || exit 99

        # The plan to archive (committed, so it's a tracked file that gets
        # git-mv'd to shipped/).
        cat > docs/plans/active/target.md <<'PLAN'
# Plan — target

body of the plan being archived.
PLAN

        # --- inbound refs, >=3 file kinds -----------------------------------
        # (1) .md PLAIN-TEXT tier-ful ref -> must become tier-less.
        printf '# Notes\n\nWork tracked in docs/plans/active/target.md henceforth.\n' \
            > docs/plans/active/notes.md

        # (3) .cpp comment PLAIN-TEXT ref -> must become tier-less.
        mkdir -p tests/Core
        cat > tests/Core/Target.test.cpp <<'CPP'
// See docs/plans/active/target.md for the spec behind these cases.
int target_test() { return 0; }
CPP

        # (4 bonus) tests/CMakeLists.txt plain-text ref -> tier-less.
        printf '# ref: docs/plans/active/target.md\n' > tests/CMakeLists.txt

        git add -A
        git commit -qm init
        git branch -f develop
        git update-ref refs/remotes/origin/develop refs/heads/develop

        # (2) .md MARKDOWN HYPERLINK — the wave-3 / #1155 bug shape. A STILL-ACTIVE
        # referrer plan with a BARE same-dir hyperlink `(target.md)` to the plan
        # being archived. While target.md lives in active/, `active/referrer.md`'s
        # `(target.md)` resolves to active/target.md (exists). The instant target.md
        # moves to shipped/, that bare href dangles (no active/target.md, and a
        # bare `target.md` is NOT the tier-less plain-text form so it does not
        # resolve via tier fallback). The referrer is UNTRACKED — untracked files
        # are in test-markdown-links' diff scope, so the dangle is detected
        # deterministically without needing the referrer in the committed diff.
        # The correct fix is a tier-ful `../shipped/target.md`, which depends on
        # BOTH files' tiers — so the script must DETECT and report, never guess.
        cat > docs/plans/active/referrer.md <<'REF'
# Referrer

Depends on [the target plan](target.md).
REF
    )
}

# ---------------------------------------------------------------------------
# Test 1 — clean fixture: gates pass standalone.
# ---------------------------------------------------------------------------
t1="$(mktemp -d)"
make_clean_fixture "$t1"
(
    cd "$t1" || exit 99
    bash agents/scripts/core/test-plan-index.sh --fix >/dev/null 2>&1
    bash agents/scripts/core/test-plan-index.sh        >/dev/null 2>&1 || exit 11
    bash agents/scripts/core/test-plan-ref-integrity.sh >/dev/null 2>&1 || exit 12
)
rc=$?
case "$rc" in
    0)  ok "clean fixture: index + ref-integrity gates pass" ;;
    11) bad "clean fixture: test-plan-index reported drift after --fix" ;;
    12) bad "clean fixture: test-plan-ref-integrity flagged a clean tree" ;;
    *)  bad "clean fixture: unexpected failure (rc=$rc)" ;;
esac
rm -rf "$t1"

# ---------------------------------------------------------------------------
# Test 2 — archive fixture: run the SUT, expect FAIL(1) due to the deliberate
# dangling markdown hyperlink (hub.md's `(active/target.md)`), and assert the
# move + plain-text rewrites happened regardless (they precede the gate).
# ---------------------------------------------------------------------------
t2="$(mktemp -d)"
make_archive_fixture "$t2"
out2="$(cd "$t2" && bash agents/scripts/core/archive-plan.sh target 2>&1)"; rc2=$?

# 2a. moved to shipped/.
if [ -f "$t2/docs/plans/shipped/target.md" ] && [ ! -f "$t2/docs/plans/active/target.md" ]; then
    ok "archive: plan moved active/ -> shipped/"
else
    bad "archive: plan not moved as expected"
fi

# 2b. plain-text .md ref rewritten to tier-less.
if grep -q 'docs/plans/target.md' "$t2/docs/plans/active/notes.md" \
   && ! grep -q 'docs/plans/active/target.md' "$t2/docs/plans/active/notes.md"; then
    ok "archive: .md plain-text ref rewritten to tier-less"
else
    bad "archive: .md plain-text ref NOT rewritten"
fi

# 2c. .cpp comment ref rewritten to tier-less.
if grep -q 'docs/plans/target.md' "$t2/tests/Core/Target.test.cpp" \
   && ! grep -q 'docs/plans/active/target.md' "$t2/tests/Core/Target.test.cpp"; then
    ok "archive: .cpp comment ref rewritten to tier-less"
else
    bad "archive: .cpp comment ref NOT rewritten"
fi

# 2d. CMakeLists ref rewritten to tier-less.
if grep -q 'docs/plans/target.md' "$t2/tests/CMakeLists.txt"; then
    ok "archive: CMakeLists plain-text ref rewritten to tier-less"
else
    bad "archive: CMakeLists ref NOT rewritten"
fi

# 2e. the SUT FAILED (exit 1) and reported the dangling markdown hyperlink.
if [ "$rc2" -eq 1 ]; then
    ok "archive: SUT exited 1 (refused on dangling hyperlink)"
else
    bad "archive: SUT exit=$rc2 (expected 1 — should refuse on dangling hyperlink)"
fi
if printf '%s\n' "$out2" | grep -qiE 'BROKEN_LINK|by hand|context-dependent'; then
    ok "archive: dangling hyperlink detected + reported"
else
    bad "archive: dangling hyperlink NOT reported (output: $(printf '%s' "$out2" | tr '\n' ' ' | tail -c 200))"
fi

# 2f. the dangling markdown hyperlink was NOT silently auto-edited: the active
# referrer still carries its original bare `(target.md)` href — we detect,
# never guess the (tier-dependent) fix.
if grep -q '(target.md)' "$t2/docs/plans/active/referrer.md"; then
    ok "archive: markdown hyperlink left intact for human fixup (no blind sed)"
else
    bad "archive: markdown hyperlink was unexpectedly auto-edited"
fi
rm -rf "$t2"

# ---------------------------------------------------------------------------
# Test 3 — archive fixture with NO dangling hyperlink: SUT must SUCCEED (exit 0)
# and leave all gates green. Proves the happy path, not just the refusal path.
# ---------------------------------------------------------------------------
t3="$(mktemp -d)"
scaffold_repo "$t3"
(
    cd "$t3" || exit 99
    # A plan with ONLY a plain-text inbound ref (no dangling hyperlink anywhere).
    printf '# Plan — clean\n\nbody, no outbound links\n' > docs/plans/active/clean.md
    printf '# Notes\n\ntracked in docs/plans/active/clean.md\n' > docs/plans/active/notes.md
    git add -A
    git commit -qm init
    git branch -f develop
    git update-ref refs/remotes/origin/develop refs/heads/develop
)
out3="$(cd "$t3" && bash agents/scripts/core/archive-plan.sh clean --tier shipped 2>&1)"; rc3=$?
if [ "$rc3" -eq 0 ]; then
    ok "happy path: SUT exited 0 (all gates green)"
else
    bad "happy path: SUT exit=$rc3 (expected 0). Tail: $(printf '%s' "$out3" | tr '\n' ' ' | tail -c 200)"
fi
if [ -f "$t3/docs/plans/shipped/clean.md" ] \
   && grep -q 'docs/plans/clean.md' "$t3/docs/plans/active/notes.md"; then
    ok "happy path: moved + plain-text ref rewritten"
else
    bad "happy path: move/rewrite incomplete"
fi
rm -rf "$t3"

# ---------------------------------------------------------------------------
# Test 4 — bad-arg validation: nonexistent slug -> exit 2, nothing moved.
# ---------------------------------------------------------------------------
t4="$(mktemp -d)"
scaffold_repo "$t4"
(cd "$t4" && git add -A && git commit -qm init >/dev/null 2>&1)
(cd "$t4" && bash agents/scripts/core/archive-plan.sh no-such-slug >/dev/null 2>&1); rc4=$?
if [ "$rc4" -eq 2 ]; then
    ok "validation: nonexistent slug -> exit 2"
else
    bad "validation: nonexistent slug exit=$rc4 (expected 2)"
fi
(cd "$t4" && bash agents/scripts/core/archive-plan.sh >/dev/null 2>&1); rc4b=$?
if [ "$rc4b" -eq 2 ]; then
    ok "validation: missing slug -> exit 2"
else
    bad "validation: missing slug exit=$rc4b (expected 2)"
fi
(cd "$t4" && bash agents/scripts/core/archive-plan.sh foo --tier bogus >/dev/null 2>&1); rc4c=$?
if [ "$rc4c" -eq 2 ]; then
    ok "validation: bad --tier -> exit 2"
else
    bad "validation: bad --tier exit=$rc4c (expected 2)"
fi
rm -rf "$t4"

# ---------------------------------------------------------------------------
echo
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0
exit 1
