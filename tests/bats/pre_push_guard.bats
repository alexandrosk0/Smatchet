#!/usr/bin/env bats
# tests/bats/pre_push_guard.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/git-hooks/pre-push (A) — the direct-push-to-protected-
# branch hard-stop — plus (C) plan-lock collision and (E) review-verdict marker.
# Pre-push ref updates are fed on stdin
# ("<local_ref> <local_sha> <remote_ref> <remote_sha>"); `gh` is stubbed to a
# no-op so the (B) merged-PR check makes no network call and cleanly exits 0.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/scripts/git-hooks/pre-push"
    TMP="$(mktemp -d)"
    git -C "$TMP" init -q -b feature
    git -C "$TMP" config user.email t@local
    git -C "$TMP" config user.name t
    ( cd "$TMP" && echo x > f && git add -A && git commit -qm x )
    STUB="$TMP/stub"; mkdir -p "$STUB"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$STUB/gh"; chmod +x "$STUB/gh"
    SHA="1111111111111111111111111111111111111111"
    ZERO="0000000000000000000000000000000000000000"
}

teardown() { rm -rf "$TMP"; }

# run_push <refline> [env-assignment]
# (E) is neutralised by default — the fixture repo's HEAD legitimately has no
# review-verdict marker (stage-neutraliser discipline); the (E) tests below
# re-enable it via a later `SMATCHET_SKIP_REVIEW_MARKER=0` (last env wins).
run_push() {
    printf '%s\n' "$1" | ( cd "$TMP" && env SMATCHET_SKIP_REVIEW_MARKER=1 ${2:-} PATH="$STUB:$PATH" bash "$HOOK" origin url )
}

@test "push to develop is REFUSED" {
    run run_push "refs/heads/feature $SHA refs/heads/develop $SHA"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING direct push to 'develop'"* ]]
}

@test "push to main is REFUSED" {
    run run_push "refs/heads/feature $SHA refs/heads/main $SHA"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING direct push to 'main'"* ]]
}

@test "HEAD:develop (renamed destination) is REFUSED" {
    run run_push "HEAD $SHA refs/heads/develop $SHA"
    [ "$status" -eq 1 ]
}

@test "SMATCHET_ALLOW_DEVELOP_PUSH=1 overrides the refusal" {
    run run_push "refs/heads/feature $SHA refs/heads/develop $SHA" "SMATCHET_ALLOW_DEVELOP_PUSH=1"
    [ "$status" -eq 0 ]
}

@test "push to a feature branch is allowed" {
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA"
    [ "$status" -eq 0 ]
}

@test "a develop branch DELETE is ignored (not a content push)" {
    run run_push "(delete) $ZERO refs/heads/develop $SHA"
    [ "$status" -eq 0 ]
}

# ---- (C) UNLOCKED-CONTENDED-PUSH (Layer B of plan-lock enforcement) ----------
# Stand up the shared lib inside $TMP, an origin/develop base, a changed file on
# the feature branch, and inject the lock table via LTC_ROWS_OVERRIDE.

# $1 = lock branch for the row covering the changed file (feat/other = cross,
#      feature = my own). Sets ROWS + advances HEAD with the changed file.
planlock_fixture() {
    mkdir -p "$TMP/agents/scripts/core"
    for x in lock-table-cache.sh _lock-json.py session-registry-lib.sh locks-show.sh; do
        cp "$REPO_ROOT/agents/scripts/core/$x" "$TMP/agents/scripts/core/$x"
    done
    git -C "$TMP" update-ref refs/remotes/origin/develop HEAD       # base = current commit
    ( cd "$TMP" && mkdir -p Source/Core/src/Sync && echo y > Source/Core/src/Sync/Foo.cpp \
        && git add -A && git commit -qm change )
    ROWS="$TMP/rows"
    # Dynamic "fresh" epoch (now) so the <14-day-cutoff cases never expire on
    # calendar time — a hardcoded date would flip stale and break these tests.
    printf '%s\t%s\totherslug\tSource/Core/src/Sync/Foo.cpp\n' "${1:-feat/other}" "$(date -u +%s)" > "$ROWS"
}

@test "(C) cross-branch overlap with a fresh lock is REFUSED" {
    planlock_fixture feat/other
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS"
    [ "$status" -eq 1 ]
    [[ "$output" == *"plan-lock collision"* ]]
    [[ "$output" == *"otherslug"* ]]
}

@test "(C) a lock on MY OWN branch does not block (mode=other excludes self)" {
    planlock_fixture feature
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS"
    [ "$status" -eq 0 ]
}

@test "(C) SMATCHET_ALLOW_UNLOCKED_PUSH=1 overrides the collision" {
    planlock_fixture feat/other
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS SMATCHET_ALLOW_UNLOCKED_PUSH=1"
    [ "$status" -eq 0 ]
}

@test "(C) a STALE (>14d) cross-branch lock is non-blocking (F3 symmetry)" {
    planlock_fixture feat/other
    # epoch 1 = 1970 -> far older than the 14-day cutoff -> skipped.
    printf 'feat/other\t1\toldslug\tSource/Core/src/Sync/Foo.cpp\n' > "$ROWS"
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS"
    [ "$status" -eq 0 ]
}

@test "(C) fails open when the lock table is unavailable (no override, no remote)" {
    planlock_fixture feat/other
    # No LTC_ROWS_OVERRIDE -> locks-show runs, finds no 'origin' remote URL ->
    # exit 2 -> undetermined -> allow. (A cross-branch row exists but is unused.)
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA"
    [ "$status" -eq 0 ]
}

@test "(C) fails open when merge-base can't resolve (no origin/develop)" {
    planlock_fixture feat/other
    git -C "$TMP" update-ref -d refs/remotes/origin/develop
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS"
    [ "$status" -eq 0 ]
}

@test "(C) runs BEFORE (B): a collision blocks even with an OPEN-PR feature branch" {
    # gh stub returns an OPEN PR state. (B) would exit 0 on OPEN; the collision
    # must still block, proving (C) sits before (B)'s open-PR exit 0.
    printf '#!/usr/bin/env bash\necho OPEN\nexit 0\n' > "$STUB/gh"; chmod +x "$STUB/gh"
    planlock_fixture feat/other
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "LTC_ROWS_OVERRIDE=$ROWS"
    [ "$status" -eq 1 ]
    [[ "$output" == *"plan-lock collision"* ]]
}

# ---- (E) REVIEW-VERDICT MARKER ----------------------------------------------
# Pushed refs/heads/* tips need a `$GIT_DIR/review-verdict-<pushed sha>` marker
# (stamped by record-review-verdict.sh). Judged per stdin update record, so the
# reflines here carry REAL shas. The marker is stamped/withheld directly — the
# recorder's own selftest covers the stamping path.

@test "(E) feature push with NO review-verdict marker is REFUSED" {
    HEADSHA="$(git -C "$TMP" rev-parse HEAD)"
    run run_push "refs/heads/feature $HEADSHA refs/heads/feature $SHA" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no review-verdict marker"* ]]
    [[ "$output" == *"record-review-verdict.sh"* ]]
}

@test "(E) feature push WITH a marker for the pushed sha is allowed" {
    HEADSHA="$(git -C "$TMP" rev-parse HEAD)"
    touch "$TMP/.git/review-verdict-$HEADSHA"
    run run_push "refs/heads/feature $HEADSHA refs/heads/feature $SHA" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 0 ]
}

@test "(E) a marker for an EARLIER commit does not cover a new tip" {
    # Stamp for the current HEAD, then advance — the new commit is exactly the
    # unreviewed-push hole the binding exists to close.
    touch "$TMP/.git/review-verdict-$(git -C "$TMP" rev-parse HEAD)"
    ( cd "$TMP" && echo z > f && git add -A && git commit -qm advance )
    NEWSHA="$(git -C "$TMP" rev-parse HEAD)"
    run run_push "refs/heads/feature $NEWSHA refs/heads/feature $SHA" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no review-verdict marker"* ]]
}

@test "(E) a marked checkout cannot smuggle an UNMARKED sibling ref" {
    # The first cut keyed on HEAD, so `git push origin side` from a marked
    # checkout slid through unreviewed (CodeRabbit on PR #2003). Per-record
    # judging must refuse and NAME the unmarked ref.
    HEADSHA="$(git -C "$TMP" rev-parse HEAD)"
    touch "$TMP/.git/review-verdict-$HEADSHA"
    ( cd "$TMP" && git checkout -qb side && echo s > s && git add -A \
        && git commit -qm side && git checkout -q feature )
    SIDESHA="$(git -C "$TMP" rev-parse side)"
    run run_push "refs/heads/feature $HEADSHA refs/heads/feature $SHA
refs/heads/side $SIDESHA refs/heads/side $ZERO" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"no review-verdict marker for side"* ]]
}

@test "(E) a branch DELETE from an unmarked checkout is allowed" {
    # A delete pushes no new commits — nothing to review. The HEAD-keyed first
    # cut would have blocked this spuriously.
    run run_push "(delete) $ZERO refs/heads/feature $SHA" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 0 ]
}

@test "(E) SMATCHET_SKIP_REVIEW_MARKER=1 overrides with a logged note" {
    run run_push "refs/heads/feature $SHA refs/heads/feature $SHA" "SMATCHET_SKIP_REVIEW_MARKER=1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"skipping the review-verdict marker gate"* ]]
}

@test "(E) runs AFTER the protected-branch stop: develop refusal wins" {
    # A develop push with no marker must still report the (A) refusal — (E)
    # must not re-label a protected-branch accident as a review-process miss.
    run run_push "refs/heads/feature $SHA refs/heads/develop $SHA" "SMATCHET_SKIP_REVIEW_MARKER=0"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSING direct push to 'develop'"* ]]
}
