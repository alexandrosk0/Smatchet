#!/usr/bin/env bats
# tests/bats/pr_body_edit.bats
# ----------------------------------------------------------------------------
# Regression suite for the marker-preserving PR-body edit path:
#   - agents/scripts/core/record-review-verdict.sh --sync-pr <n> writes the
#     freshly stamped `adversarial-code-review: … (head=<sha>)` line into the
#     PR body (replace the live line, or append), so the head= hex is never
#     hand-copied;
#   - agents/scripts/core/lib/pr-body-edit.sh REFUSES a body that lost a
#     `lock-slug:` / `holds-lock:` line the current body carries.
#
# gh is a stub on PATH: `api … pulls/<n>` (GET) prints $GH_BODY; `api -X
# PATCH … --input <f>` copies <f> to $GH_PATCHED. Every call is logged to
# $GH_CALLS. record-review-verdict runs in a throwaway repo against base-ref
# HEAD (an empty diff, so no review artifact is required).
#
# Requires: bash, git, python3, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export RRV="$REPO_ROOT/agents/scripts/core/record-review-verdict.sh"
    export PBE="$REPO_ROOT/agents/scripts/core/lib/pr-body-edit.sh"
    export CHECKER="$REPO_ROOT/agents/scripts/core/check-pr-intent.sh"
    unset SMATCHET_SKIP_REVIEW_GATE

    TMP="$(mktemp -d)"
    export TMP
    export WORK="$TMP/work"
    export STUB_BIN="$TMP/bin"
    export GH_BODY="$TMP/body.md"
    export GH_PATCHED="$TMP/patched.json"
    export GH_CALLS="$TMP/gh.calls"
    export REPO="test/repo"
    mkdir -p "$STUB_BIN"

    git init -q "$WORK"
    git -C "$WORK" -c user.email=t@t -c user.name=t -c commit.gpgsign=false \
        commit -q --allow-empty -m seed
    HEAD12="$(git -C "$WORK" rev-parse HEAD | cut -c1-12)"
    export HEAD12

    cat > "$STUB_BIN/gh" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$GH_CALLS"
[ "$1" = "api" ] || exit 0
shift
method=GET
input=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -X) method="$2"; shift 2 ;;
        --input) input="$2"; shift 2 ;;
        --jq) shift 2 ;;
        *) shift ;;
    esac
done
if [ "$method" = "PATCH" ]; then
    cp "$input" "$GH_PATCHED"
    printf '{}\n'
    exit 0
fi
[ "${GH_FAIL_GET:-0}" = "1" ] && { echo "HTTP 502" >&2; exit 1; }
# GET n (1-based) serves $GH_BODY.<n> when that file exists, else $GH_BODY —
# a test stages a concurrent edit by writing $GH_BODY.2, $GH_BODY.3, ...
n=$(( $(cat "$GH_BODY.gets" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$GH_BODY.gets"
if [ -f "$GH_BODY.$n" ]; then cat "$GH_BODY.$n"; else cat "$GH_BODY"; fi
STUB
    chmod +x "$STUB_BIN/gh"
}

teardown() {
    rm -rf "${TMP:-}"
}

# rrv <args…> — record-review-verdict from the throwaway repo, gh stub first.
rrv() {
    run bash -c 'cd "$1" && shift && PATH="$STUB_BIN:$PATH" bash "$RRV" "$@"' _ "$WORK" "$@"
}

# patched_body — the body the stub received in the PATCH payload.
patched_body() {
    python3 -c 'import json, sys; sys.stdout.write(json.load(open(sys.argv[1]))["body"])' "$GH_PATCHED"
}

@test "--sync-pr replaces the live verdict line and keeps the lock-slug line" {
    printf '## Intent\n\nShip it.\n\nadversarial-code-review: 2 findings, fixed (head=000000000000)\n\nlock-slug: keep-me\n' > "$GH_BODY"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 0 ]
    grep -q 'repos/test/repo/pulls/42' "$GH_CALLS"
    run patched_body
    [[ "$output" == *"adversarial-code-review: 0 findings (head=${HEAD12})"* ]]
    [[ "$output" != *'head=000000000000'* ]]
    [[ "$output" == *$'\n''lock-slug: keep-me'* ]]
    [ "$(grep -c 'adversarial-code-review:' <<<"$output")" -eq 1 ]
}

@test "--sync-pr replaces a list-form verdict instead of appending a second one" {
    printf '## Intent\n\nShip it.\n\n- **adversarial-code-review**: n/a — old (head=000000000000)\n' > "$GH_BODY"
    rrv "0 findings" HEAD --sync-pr=7
    [ "$status" -eq 0 ]
    run patched_body
    [ "$(grep -c 'adversarial-code-review' <<<"$output")" -eq 1 ]
    [[ "$output" == *"adversarial-code-review: 0 findings (head=${HEAD12})"* ]]
}

@test "--sync-pr appends when only the template's commented placeholder exists" {
    printf '## Intent\n\nShip it.\n\n<!-- REQUIRED:\nadversarial-code-review: N findings, <disposition> (head=<sha>)\n-->\n' > "$GH_BODY"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 0 ]
    run patched_body
    # The placeholder inside the comment is left alone ...
    [[ "$output" == *$'\n''adversarial-code-review: N findings, <disposition> (head=<sha>)'$'\n''-->'* ]]
    # ... and the appended line satisfies the real Intent checker for this head.
    printf '%s' "$output" > "$TMP/new.md"
    run env PR_HEAD_SHA="$(git -C "$WORK" rev-parse HEAD)" bash "$CHECKER" "$TMP/new.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *"head-bound"* ]]
}

@test "without --sync-pr the script never calls gh" {
    printf '## Intent\n\nShip it.\n' > "$GH_BODY"
    rrv "0 findings" HEAD
    [ "$status" -eq 0 ]
    [[ "$output" == *"(head=${HEAD12})"* ]]
    [ ! -e "$GH_CALLS" ]
    [ ! -e "$GH_PATCHED" ]
}

@test "a failed marker write exits 2 and prints no verdict line" {
    # main runs `_record … || exit $?`, which disables set -e inside _record:
    # the write must be checked explicitly or the verdict prints with exit 0
    # and no marker behind it. A directory at the marker path forces the fail.
    mkdir "$(git -C "$WORK" rev-parse --absolute-git-dir)/review-verdict-$(git -C "$WORK" rev-parse HEAD)"
    rrv "0 findings" HEAD
    [ "$status" -eq 2 ]
    [[ "$output" == *"could not write the verdict marker"* ]]
    [[ "$output" != *"adversarial-code-review: 0 findings (head="* ]]
}

@test "--sync-pr with a non-number is a usage error and stamps nothing" {
    rrv "0 findings" HEAD --sync-pr abc
    [ "$status" -eq 2 ]
    [[ "$output" == *"takes a PR number"* ]]
    [ ! -e "$(git -C "$WORK" rev-parse --absolute-git-dir)/review-verdict-$(git -C "$WORK" rev-parse HEAD)" ]
    [ ! -e "$GH_CALLS" ]
}

@test "a failed body read exits 3 but the local marker stands" {
    printf '## Intent\n' > "$GH_BODY"
    GH_FAIL_GET=1 rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 3 ]
    [[ "$output" == *"NOT updated"* ]]
    [ ! -e "$GH_PATCHED" ]
    [ -f "$(git -C "$WORK" rev-parse --absolute-git-dir)/review-verdict-$(git -C "$WORK" rev-parse HEAD)" ]
}

@test "pr-body-edit.sh refuses a rewrite that drops a bare lock-slug line" {
    printf '## Intent\n\nShip it.\n\nlock-slug: keep-me\n' > "$GH_BODY"
    printf '## Intent\n\nShip it, rewritten.\n' > "$TMP/new.md"
    run bash -c 'PATH="$STUB_BIN:$PATH" bash "$PBE" 42 "$1"' _ "$TMP/new.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REFUSED"* ]]
    [[ "$output" == *"lock-slug: keep-me"* ]]
    [ ! -e "$GH_PATCHED" ]
}

@test "pr-body-edit.sh refuses a rewrite that comments out a bare holds-lock line" {
    printf 'holds-lock: shared-lock\n' > "$GH_BODY"
    printf '<!-- holds-lock: shared-lock -->\n' > "$TMP/new.md"
    run bash -c 'PATH="$STUB_BIN:$PATH" bash "$PBE" 42 "$1"' _ "$TMP/new.md"
    [ "$status" -eq 1 ]
    [[ "$output" == *"holds-lock: shared-lock"* ]]
    [ ! -e "$GH_PATCHED" ]
}

@test "pr-body-edit.sh writes a rewrite that keeps its markers (template placeholders may go)" {
    printf '## Intent\n\nlock-slug: keep-me\n<!-- lock-slug: your-slug-here -->\n' > "$GH_BODY"
    printf '## Intent\n\nNew text.\n\nlock-slug: keep-me\n' > "$TMP/new.md"
    run bash -c 'PATH="$STUB_BIN:$PATH" bash "$PBE" 42 "$1"' _ "$TMP/new.md"
    [ "$status" -eq 0 ]
    run patched_body
    [[ "$output" == *'New text.'* ]]
    [[ "$output" == *'lock-slug: keep-me'* ]]
}

# ---------- round-trip exactness + concurrency guard ----------
# The stub prints a GET body the way `gh --jq` does: the body plus ONE newline.

@test "--sync-pr PATCHes the body byte-exact (no trailing newline added per sync)" {
    printf '## Intent\n\nShip it.\n\nadversarial-code-review: old (head=000000000000)\n' > "$GH_BODY"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 0 ]
    run patched_body
    [ "$output" = "$(printf '## Intent\n\nShip it.\n\nadversarial-code-review: 0 findings (head=%s)' "$HEAD12")" ]
    # Re-sync the patched body (gh prints it + one newline): nothing may grow.
    { patched_body; printf '\n'; } > "$GH_BODY"
    rm -f "$GH_BODY.gets"
    python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["body"]))' "$GH_PATCHED" > "$TMP/len1"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 0 ]
    python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["body"]))' "$GH_PATCHED" > "$TMP/len2"
    [ "$(cat "$TMP/len1")" = "$(cat "$TMP/len2")" ]
}

@test "pr-body-edit.sh refuses to overwrite a body edited since it was read (exit 3, no PATCH)" {
    printf '## Intent\n\nOriginal.\n' > "$GH_BODY"
    printf '## Intent\n\nSomeone else edited this.\n' > "$GH_BODY.2"
    printf '## Intent\n\nMy rewrite.\n' > "$TMP/new.md"
    run bash -c 'PATH="$STUB_BIN:$PATH" bash "$PBE" 42 "$1"' _ "$TMP/new.md"
    [ "$status" -eq 3 ]
    [[ "$output" == *"changed since it was read"* ]]
    [ ! -e "$GH_PATCHED" ]
}

@test "--sync-pr re-reads once after a concurrent edit and keeps that edit" {
    printf '## Intent\n\nOriginal.\n' > "$GH_BODY"
    # GET 1 = sync read, GET 2 = pre-PATCH re-read: the body moved on. The
    # retry reads the new body (GETs 3 and 4) and writes the verdict onto it.
    printf '## Intent\n\nConcurrent edit kept.\n' > "$GH_BODY.2"
    printf '## Intent\n\nConcurrent edit kept.\n' > "$GH_BODY.3"
    printf '## Intent\n\nConcurrent edit kept.\n' > "$GH_BODY.4"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 0 ]
    [[ "$output" == *"re-reading it once"* ]]
    run patched_body
    [[ "$output" == *"Concurrent edit kept."* ]]
    [[ "$output" == *"adversarial-code-review: 0 findings (head=${HEAD12})"* ]]
}

@test "--sync-pr gives up (exit 3, no PATCH) when the body keeps changing" {
    printf '## Intent\n\nA.\n' > "$GH_BODY"
    printf '## Intent\n\nB.\n' > "$GH_BODY.2"
    printf '## Intent\n\nB.\n' > "$GH_BODY.3"
    printf '## Intent\n\nC.\n' > "$GH_BODY.4"
    rrv "0 findings" HEAD --sync-pr 42
    [ "$status" -eq 3 ]
    [[ "$output" == *"NOT updated"* ]]
    [ ! -e "$GH_PATCHED" ]
}

@test "pbe_upsert_line keeps CRLF line endings" {
    printf 'a\r\nadversarial-code-review: old (head=000000000000)\r\nb\r\n' > "$TMP/in.md"
    run bash -c '. "$PBE" && pbe_upsert_line "^adversarial-code-review:" "adversarial-code-review: new" "$1" "$2"' _ "$TMP/in.md" "$TMP/out.md"
    [ "$status" -eq 0 ]
    [ "$(cat "$TMP/out.md")" = "$(printf 'a\r\nadversarial-code-review: new\r\nb\r')" ]
}
