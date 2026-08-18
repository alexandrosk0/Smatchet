#!/usr/bin/env bats
# setup_branch_protection.bats — regression suite for the PUT body built by
# agents/scripts/core/setup-branch-protection.sh.
#
# WHY this suite exists. PUT /repos/{owner}/{repo}/branches/{branch}/protection
# is a full-object REPLACE, not a patch: an optional protection flag omitted
# from the body is reset to its API default (false). Omission is therefore a
# silent REMOVAL, not a no-op. The body once omitted
# `required_conversation_resolution` while live had it true, so an apply would
# have switched that gate OFF in the same call that tightened enforce_admins
# (docs/plans/branch-protection-config-completeness.md).
#
# The contract under test: project.config.json § branch_protection is the
# COMPLETE desired state, and every flag it states reaches the wire.
#   1-2. The body is valid JSON and the four structural keys are present.
#   3.   required_conversation_resolution survives into the body (the regression).
#   4-6. enforce_admins / review count / contexts transcribe the config.
#   7.   COMPLETENESS — every boolean in the config block appears somewhere in
#        the body. This is the assertion that catches a future config flag added
#        without a matching line in the script's projection.
#   8.   restrictions is present-and-null (explicitly cleared, not dropped).
#   9.   --dry-run performs no gh call at all (safe to run anywhere).
#   10.  Every required check carries the pinned app_id — the second silent
#        loosening this endpoint affords: the deprecated contexts[] spelling has
#        no app id, so a body built from it leaves every context unpinned.
#   11-12. Case 7 is blind to a DELETED config key: it diffs the keys present in
#        config against the body, so removing one removes its own assertion and
#        the apply then writes the API default (false) to the wire as a real
#        change. 11 pins the expected key set; 12 proves the script hard-fails
#        (exit 2, no PUT) on an incomplete block rather than defaulting.
#
# Every case drives `--dry-run` with $REPO set, so resolve_repo short-circuits
# and no network call is possible. A stub `gh` sits on PATH only to satisfy the
# script's `command -v gh` precondition; it records any invocation so case 9 can
# assert it stayed unused.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    cd "$REPO_ROOT" || return 1
    SCRIPT="agents/scripts/core/setup-branch-protection.sh"
    TMPDIR_T="$(mktemp -d)"
    FAKEBIN="$TMPDIR_T/bin"
    mkdir -p "$FAKEBIN"
    GH_CALLED="$TMPDIR_T/gh.called"
    BODY_FILE="$TMPDIR_T/body.json"
    cat > "$FAKEBIN/gh" <<GH
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$GH_CALLED"
exit 0
GH
    chmod +x "$FAKEBIN/gh"
}

teardown() {
    rm -rf "${TMPDIR_T:-}"
}

# Run --dry-run and leave just the JSON body in $BODY_FILE (the script prints a
# "would PUT to …" banner on line 1 first).
dry_run_body() {
    REPO="acme/widget" PATH="$FAKEBIN:$PATH" bash "$SCRIPT" --dry-run \
        | tail -n +2 > "$BODY_FILE"
}

@test "dry-run body is valid JSON" {
    dry_run_body
    run jq -e . "$BODY_FILE"
    [ "$status" -eq 0 ]
}

@test "body carries the four structural protection keys" {
    dry_run_body
    for k in required_status_checks enforce_admins required_pull_request_reviews restrictions; do
        run jq -e --arg k "$k" 'has($k)' "$BODY_FILE"
        [ "$status" -eq 0 ]
    done
}

@test "required_conversation_resolution reaches the body as true (the regression)" {
    dry_run_body
    [ "$(jq -r '.required_conversation_resolution' "$BODY_FILE")" = "true" ]
}

@test "enforce_admins transcribes the config value" {
    dry_run_body
    WANT="$(jq -r '.branch_protection.enforce_admins' project.config.json)"
    [ "$(jq -r '.enforce_admins' "$BODY_FILE")" = "$WANT" ]
}

@test "required_approving_review_count transcribes required_review_count" {
    dry_run_body
    WANT="$(jq -r '.branch_protection.required_review_count' project.config.json)"
    [ "$(jq -r '.required_pull_request_reviews.required_approving_review_count' "$BODY_FILE")" = "$WANT" ]
}

@test "required_status_checks.checks[].context is set-equal to the config contexts" {
    dry_run_body
    # Set-equality, not order: the API does not treat the array as ordered, and
    # live is in chronological-append order while the config is curated.
    # tr -d '\r': the Windows jq build emits CRLF, and $() strips only the
    # TRAILING one, so a multi-line capture keeps a CR on every interior line.
    WANT="$(jq -r '.branch_protection.required_contexts | sort | .[]' project.config.json | tr -d '\r')"
    GOT="$(jq -r '[.required_status_checks.checks[].context] | sort | .[]' "$BODY_FILE" | tr -d '\r')"
    [ "$WANT" = "$GOT" ]
}

@test "every required check pins required_checks_app_id (no 'any app' widening)" {
    dry_run_body
    WANT="$(jq -r '.branch_protection.required_checks_app_id' project.config.json | tr -d '\r')"
    # The deprecated contexts[] spelling must not reappear: it carries no app id,
    # so a body using it silently unpins every context to "any app may report
    # this check" — a widening applied by the script that exists to tighten.
    # Probe the NESTED object: contexts[] and checks[] are both children of
    # required_status_checks, so a top-level has("contexts") is vacuously true
    # and would pass against the very body it is meant to reject.
    run jq -e '.required_status_checks | has("contexts") | not' "$BODY_FILE"
    [ "$status" -eq 0 ]
    run jq -e '.required_status_checks | has("checks")' "$BODY_FILE"
    [ "$status" -eq 0 ]
    if [ "$WANT" = "null" ]; then
        # Deliberate AUTO-SELECTION: the key is omitted, not sent as an explicit
        # null and not sent as -1. The three spellings are distinct — omitted
        # means GitHub picks the app that most recently provided each context,
        # -1 means any app may set the status, a positive id pins. Assert the
        # omission is total.
        [ "$(jq -r '[.required_status_checks.checks[] | has("app_id")] | any' "$BODY_FILE" | tr -d '\r')" = "false" ]
        # Floor guard (fail-open shape): `any` over an EMPTY array is also
        # false, so without this an empty checks[] passes vacuously.
        N="$(jq -r '.required_status_checks.checks | length' "$BODY_FILE" | tr -d '\r')"
        [ "${N:-0}" -ge 10 ]
    else
        # Every entry, not just the first — a partial pin is the same hole.
        MISPINNED="$(jq -r --argjson want "$WANT" \
            '[.required_status_checks.checks[] | select(.app_id != $want) | .context] | join(" ")' \
            "$BODY_FILE" | tr -d '\r')"
        if [ -n "$MISPINNED" ]; then
            echo "checks not pinned to app_id $WANT: $MISPINNED" >&2
            return 1
        fi
        # Floor guard (fail-open shape): an empty MISPINNED is also what an empty
        # checks[] produces. Assert the pin check had entries to walk.
        N="$(jq -r '.required_status_checks.checks | length' "$BODY_FILE" | tr -d '\r')"
        [ "${N:-0}" -ge 10 ]
    fi
}

@test "COMPLETENESS: every boolean flag in the config block reaches the body" {
    dry_run_body
    # The set difference is computed INSIDE jq, deliberately: the shell-side
    # spelling of this (`for k in $(jq …); do … | grep -qx -- "$k"`) is a trap
    # on Windows. The jq build emits CRLF, msys grep strips the CR from its
    # stdin but not from the pattern, and $() strips only the trailing one — so
    # every key but the last false-negatives and the assertion inverts.
    #
    # `..` walks recursively because `strict` nests under required_status_checks
    # and the review flags under required_pull_request_reviews; a flat top-level
    # key check would miss them.
    MISSING="$(jq -r --slurpfile cfg project.config.json '
        ([.. | objects | keys[]] | unique) as $body
        | ($cfg[0].branch_protection
           | to_entries | map(select(.value | type == "boolean")) | map(.key))
          - $body
        | join(" ")
    ' "$BODY_FILE" | tr -d '\r')"
    # Non-empty MISSING = project.config.json states a flag the script does not
    # project, so the next apply resets it to false on the wire. Fix: add the
    # key to TOP_LEVEL_FLAGS or REVIEW_FLAGS in the script.
    if [ -n "$MISSING" ]; then
        echo "config flags absent from the PUT body: $MISSING" >&2
        return 1
    fi
    # Floor guard (fail-open shape): an empty MISSING is also what a jq error or
    # a gutted config block produces, and both would pass vacuously. Assert the
    # comparison actually had flags to compare.
    COUNT="$(jq -r '.branch_protection | to_entries | map(select(.value | type == "boolean")) | length' project.config.json | tr -d '\r')"
    if [ "${COUNT:-0}" -lt 10 ]; then
        echo "completeness check ran against only ${COUNT:-0} config flags — expected the full optional-flag set" >&2
        return 1
    fi
}

@test "config block states every key the projection reads (deletion cannot hide)" {
    # Case 8 diffs the keys PRESENT in the config against the body, so it is
    # blind in exactly one direction: DELETE a config key and its own assertion
    # leaves with it, the (former) `.get(k, False)` fallback supplies false, and
    # the next apply writes that false to the wire as a real protection change.
    # This case closes that hole from the other side — the expected key set is
    # pinned here, so a deletion fails the suite instead of passing quietly.
    #
    # Adding a key is meant to fail here too: the new key needs a line in the
    # script's projection (TOP_LEVEL_FLAGS / REVIEW_FLAGS / the structural
    # block), a REQUIRED_KEYS entry, a schema `required` entry, and a line
    # below. `_doc` is prose, carries nothing to the wire, and is the one key
    # excluded from REQUIRED_KEYS — so it is excluded here too.
    WANT="$(printf '%s\n' \
        allow_deletions allow_force_pushes allow_fork_syncing block_creations \
        branch dismiss_stale_reviews enforce_admins lock_branch \
        require_code_owner_reviews require_last_push_approval \
        required_checks_app_id required_contexts \
        required_conversation_resolution required_linear_history \
        required_review_count strict | sort)"
    GOT="$(jq -r '.branch_protection | keys[] | select(. != "_doc")' project.config.json | tr -d '\r' | sort)"
    if [ "$WANT" != "$GOT" ]; then
        echo "branch_protection key set drifted from the projection:" >&2
        diff <(printf '%s\n' "$WANT") <(printf '%s\n' "$GOT") >&2 || true
        return 1
    fi
}

@test "a missing config key fails the apply loudly (exit 2, no PUT)" {
    # The pinned-key-set case above catches a deletion in THIS repo's config; this
    # one proves the script itself refuses to run against an incomplete block, so
    # a reused project that trims a key gets an error instead of a silent false
    # written to its branch protection.
    MUTATED="$TMPDIR_T/config-missing-key.json"
    jq 'del(.branch_protection.required_conversation_resolution)' project.config.json > "$MUTATED"

    run env REPO="acme/widget" SMATCHET_BP_CONFIG="$MUTATED" PATH="$FAKEBIN:$PATH" \
        bash "$SCRIPT" --dry-run
    [ "$status" -eq 2 ]
    [[ "$output" == *"missing required key(s): required_conversation_resolution"* ]]
    # Hard-fails BEFORE any network call, on the dry-run path and the apply path
    # alike — the body build is the first thing either does.
    [ ! -f "$GH_CALLED" ]
}

@test "restrictions is present and null (explicitly cleared, not dropped)" {
    dry_run_body
    [ "$(jq -r '.restrictions | type' "$BODY_FILE")" = "null" ]
}

@test "--dry-run invokes gh zero times" {
    dry_run_body
    [ ! -f "$GH_CALLED" ]
}
