#!/usr/bin/env bats
# tests/bats/capture_intent.bats
# ----------------------------------------------------------------------------
# Bats tests for the prompt-intent capture pipeline:
#   * agents/scripts/core/redact-intent.py          — the security redactor
#   * docs/harness/claude-code/hooks/capture-intent.sh — the UserPromptSubmit hook
#
# This is a SECURITY boundary (a leaked secret/credential or home-dir username
# ends up in a public PR body), so the redactor cases are the load-bearing ones:
# every high-confidence secret class is stripped, home-dir usernames collapse to
# `[user]`, emails are redacted to `[REDACTED-EMAIL]` (the Intent line is a public
# PII surface — pr-intent-capture-hardening #7), output is always a single line.
# The hook cases assert the two hard invariants: it writes a REDACTED line to a
# branch-keyed capture file, and it prints NOTHING to stdout (UserPromptSubmit
# stdout is injected into the model's context). Fail-safe: a missing/erroring
# redactor writes nothing (never the raw prompt) and still exits 0.
#
# Drives the redactor directly over stdin (deterministic, no env seams needed)
# and the hook with a fresh isolated CLAUDE_PROJECT_DIR per test (the real
# redactor copied in, so the capture file lands in a temp tree, not the repo).
#
# Requires: bash, python3 or python (on PATH), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    REDACTOR_SRC="$REPO_ROOT/agents/scripts/core/redact-intent.py"
    export REDACTOR_SRC
    HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/capture-intent.sh"
    export HOOK

    # Resolve a python interpreter by PROBE-EXECUTING each candidate — a bare
    # `command -v python3` on Windows finds the Microsoft Store alias stub that
    # errors on run. (Mirrors the resolver in capture-intent.sh.)
    PY=""
    for _cand in python3 python py; do
        if command -v "$_cand" >/dev/null 2>&1 && "$_cand" -c "import sys" >/dev/null 2>&1; then
            PY="$_cand"; break
        fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter on PATH"

    # Isolated project dir with the real redactor copied in. The hook's capture
    # file lands here (not in the real repo's .session-intent/).
    PROJ_DIR="$(mktemp -d)"
    export PROJ_DIR
    mkdir -p "$PROJ_DIR/agents/scripts/core"
    cp "$REDACTOR_SRC" "$PROJ_DIR/agents/scripts/core/redact-intent.py"
    export CLAUDE_PROJECT_DIR="$PROJ_DIR"
    export CAPTURE_DIR="$PROJ_DIR/.session-intent"
}

teardown() {
    [ -n "${PROJ_DIR:-}" ] && rm -rf "$PROJ_DIR"
}

# Run the redactor over a literal prompt string. Output captured in $output.
redact() { run bash -c 'printf "%s" "$1" | "$PY" "$REDACTOR_SRC"' _ "$1"; }

# Feed an event-JSON (arg $1) to the hook on stdin. A here-string (not a pipe)
# so `run` executes in THIS shell and its $status/$output propagate to the test
# — `printf ... | run_hook` would run `run` in a subshell and lose both.
run_hook() { run bash "$HOOK" <<<"$1"; }

# Concatenate every capture-file line written this test (empty if none).
capture_body() { cat "$CAPTURE_DIR"/*.log 2>/dev/null || true; }

# ============================================================================
# redact-intent.py — secret classes stripped
# ============================================================================

@test "selftest suite passes" {
    run "$PY" "$REDACTOR_SRC" --selftest
    [ "$status" -eq 0 ]
}

@test "GitHub token (ghp_) stripped" {
    redact "deploy token=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 now"
    [ "$status" -eq 0 ]
    [[ "$output" != *"ghp_ABCDEFGHIJKLMNOPQRST"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
}

@test "fine-grained github_pat_ stripped" {
    redact "github_pat_11ABCDEFG0abcdefghijklmnopqrstuvwxyz0123456789"
    [[ "$output" != *"github_pat_11ABCDEFG0"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "JWT stripped" {
    redact "auth eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.dBjftJeZ4CVP done"
    [[ "$output" != *"eyJhbGci"* ]]
    [[ "$output" == *"[REDACTED-JWT]"* ]]
}

@test "AWS access key stripped" {
    redact "creds AKIAIOSFODNN7EXAMPLE used"
    [[ "$output" != *"AKIAIOSFODNN7EXAMPLE"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "Slack token stripped" {
    redact "slack xoxb-1234567890-abcdefghijkl here"
    [[ "$output" != *"xoxb-1234567890"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "Google API key stripped" {
    # Real shape: AIza + exactly 35 chars (39 total).
    redact "key AIzaabcdefghijklmnopqrstuvwxyz012345678 end"
    [[ "$output" != *"AIzaabcdefghij"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "Bearer header value stripped, keyword kept" {
    # Bare-Bearer prose isolates _BEARER (no `authorization` KV-stem collision):
    # the keyword survives, only the token is stripped. The `Authorization: Bearer`
    # form — where the `authorization` KV-stem also eats the `Bearer` keyword (more
    # redaction, the safe direction) — is covered by the selftest at the same input.
    redact "use Bearer abcdef0123456789xyz now"
    [[ "$output" != *"abcdef0123456789"* ]]
    [[ "$output" == *"Bearer [REDACTED]"* ]]
}

@test "key=value secret pair value stripped, key kept" {
    redact "set password: hunter2supersecret please"
    [[ "$output" != *"hunter2supersecret"* ]]
    [[ "$output" == *"password"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
}

@test "PRIVATE KEY block stripped" {
    redact "-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEAabcdef
-----END RSA PRIVATE KEY-----"
    [[ "$output" != *"MIIEowIBAAKCAQEA"* ]]
    [[ "$output" == *"[REDACTED-PRIVATE-KEY]"* ]]
}

@test "long hex blob (>=32) stripped" {
    redact "blob deadbeefdeadbeefdeadbeefdeadbeefdeadbeef tail"
    [[ "$output" != *"deadbeefdeadbeefdeadbeef"* ]]
    [[ "$output" == *"[REDACTED-HEX]"* ]]
}

@test "C1: JSON quoted-key secret value stripped (no _KV bypass)" {
    redact '{"password":"hunter2supersecret"}'
    [[ "$output" != *"hunter2supersecret"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
}

@test "C1: quoted-key with spaces around = stripped" {
    redact "config 'api_key' = 'sneakyvalue123' done"
    [[ "$output" != *"sneakyvalue123"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
}

@test "H1: Stripe sk_live_ key stripped" {
    redact "stripe sk_live_abcdEFGH1234 charge"
    [[ "$output" != *"sk_live_abcdEFGH1234"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "H1: OpenAI sk- key stripped" {
    redact "openai sk-abcdEFGHijklMNOPqrstUV call"
    [[ "$output" != *"sk-abcdEFGHijklMNOP"* ]]
    [[ "$output" == *"[REDACTED-TOKEN]"* ]]
}

@test "H1: connection-URL password stripped, user+host kept" {
    redact "clone https://user:s3cr3ttoken@github.com/x"
    [[ "$output" != *"s3cr3ttoken"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
    [[ "$output" == *"@github.com"* ]]
}

@test "H1: connection-URL empty-username form password stripped" {
    redact "cache redis://:s3cr3tpass@cache:6379/0"
    [[ "$output" != *"s3cr3tpass"* ]]
    [[ "$output" == *"[REDACTED]"* ]]
    [[ "$output" == *"@cache"* ]]
}

# ============================================================================
# redact-intent.py — home-dir collapse, email preserved, single-line
# ============================================================================

@test "Windows home-dir username collapsed to [user]" {
    redact "open C:\\Users\\alexk\\.aws\\credentials file"
    [[ "$output" != *"alexk"* ]]
    [[ "$output" == *"[user]"* ]]
}

@test "POSIX /home username collapsed to [user]" {
    redact "read /home/alexk/.ssh/id_rsa key"
    [[ "$output" != *"/home/alexk"* ]]
    [[ "$output" == *"[user]"* ]]
}

@test "macOS /Users username collapsed to [user]" {
    redact "stat /Users/alexk/Library/foo path"
    [[ "$output" != *"/Users/alexk"* ]]
    [[ "$output" == *"[user]"* ]]
}

@test "H2: UNC \\\\host\\Users\\name username collapsed to [user]" {
    redact "unc \\\\fileserver\\Users\\alexk\\notes.txt"
    [[ "$output" != *"alexk"* ]]
    [[ "$output" == *"[user]"* ]]
}

@test "H2: legacy Documents and Settings username collapsed to [user]" {
    redact "xp C:\\Documents and Settings\\alexk\\app.ini"
    [[ "$output" != *"alexk"* ]]
    [[ "$output" == *"[user]"* ]]
}

@test "#7: email IS redacted (public PR body is a PII surface)" {
    redact "ping alexkonstantonis@gmail.com about it"
    [[ "$output" != *"alexkonstantonis@gmail.com"* ]]
    [[ "$output" == *"[REDACTED-EMAIL]"* ]]
}

@test "#7: ssh-context user@host still collapses to [user] (not an email)" {
    redact "deploy ssh -p 2222 deployacct@host:/srv"
    [[ "$output" != *"deployacct"* ]]
    [[ "$output" == *"[user]@host"* ]]
}

@test "multi-line prompt collapses to a single line" {
    redact "line one
line two	tabbed"
    [[ "$output" == "line one line two tabbed" ]]
    [[ "$output" != *$'\n'* ]]
}

@test "empty input yields empty output" {
    redact ""
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ============================================================================
# capture-intent.sh — end-to-end hook behaviour + invariants
# ============================================================================

@test "hook writes a redacted bullet line to the capture file" {
    run_hook '{"prompt":"fix the login bug"}'
    [ "$status" -eq 0 ]
    [[ "$(capture_body)" == *"- fix the login bug"* ]]
}

@test "hook prints NOTHING to stdout (stdout is injected into model context)" {
    run_hook '{"prompt":"some ordinary prompt"}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "hook redacts a secret before writing (end-to-end)" {
    run_hook '{"prompt":"deploy with token=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}'
    [ "$status" -eq 0 ]
    [[ "$(capture_body)" != *"ghp_ABCDEFGHIJKLMNOPQRST"* ]]
    [[ "$(capture_body)" == *"[REDACTED]"* ]]
}

@test "hook redacts a home-dir username before writing" {
    # JSON \\ parses to a single backslash → redactor sees C:\Users\alexk.
    run_hook '{"prompt":"open C:\\Users\\alexk\\notes.txt"}'
    [ "$status" -eq 0 ]
    [[ "$(capture_body)" != *"alexk"* ]]
    [[ "$(capture_body)" == *"[user]"* ]]
}

@test "empty prompt writes nothing and exits 0" {
    run_hook '{"prompt":""}'
    [ "$status" -eq 0 ]
    [ -z "$(capture_body)" ]
}

@test "non-JSON / no .prompt field writes nothing and exits 0" {
    run_hook 'not json at all'
    [ "$status" -eq 0 ]
    [ -z "$(capture_body)" ]
}

@test "fail-safe: missing redactor writes nothing (never the raw prompt), exits 0" {
    rm -f "$PROJ_DIR/agents/scripts/core/redact-intent.py"
    run_hook '{"prompt":"secret token=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [[ "$(capture_body)" != *"ghp_"* ]]
    [ -z "$(capture_body)" ]
}

@test "fail-safe: an erroring redactor (no output) writes nothing, exits 0" {
    # Replace the redactor with one that emits nothing and exits non-zero.
    printf '#!/usr/bin/env python3\nimport sys; sys.exit(1)\n' \
        > "$PROJ_DIR/agents/scripts/core/redact-intent.py"
    run_hook '{"prompt":"secret token=ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"}'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    [[ "$(capture_body)" != *"ghp_"* ]]
}

# ----------------------------------------------------------------------------
# pr-intent-capture-hardening — #2 branch fix, #1 provenance, #4 cap-on-append
# ----------------------------------------------------------------------------

@test "#2 branch fix: unborn branch resolves to its name, not HEAD-_unknown" {
    # symbolic-ref --short returns the branch on an unborn branch; the old
    # rev-parse --abbrev-ref printed HEAD + exited non-zero -> HEAD-_unknown.log.
    git -C "$PROJ_DIR" init -q
    git -C "$PROJ_DIR" checkout -q -b feature/foo
    run_hook '{"prompt":"on an unborn branch"}'
    [ "$status" -eq 0 ]
    [ -f "$CAPTURE_DIR/feature-foo.log" ]
    [ ! -e "$CAPTURE_DIR/HEAD-_unknown.log" ]
}

@test "#1 provenance: header present and excluded by the orchestrator '- ' filter" {
    run_hook '{"prompt":"the actual ask"}'
    [ "$status" -eq 0 ]
    f="$(ls "$CAPTURE_DIR"/*.log)"
    grep -q "^# capture-intent v" "$f"          # provenance header written
    [ "$(grep -c '^- ' "$f")" -eq 1 ]           # exactly one bullet
    [ "$(grep '^- ' "$f")" = "- the actual ask" ]  # header is not in the '- ' set
}

@test "#4 cap-on-append keeps the header + last N bullets" {
    export CAPTURE_INTENT_CAP=2
    for i in 1 2 3 4 5; do
        run_hook "{\"prompt\":\"p$i\"}"
        [ "$status" -eq 0 ]
    done
    f="$(ls "$CAPTURE_DIR"/*.log)"
    grep -q "^# capture-intent v" "$f"          # header survives the trim
    [ "$(grep -c '^- ' "$f")" -eq 2 ]           # capped to last 2 bullets
    [[ "$(capture_body)" == *"- p5"* ]]         # most recent kept
    [[ "$(capture_body)" != *"- p1"* ]]         # oldest dropped
}

# ----------------------------------------------------------------------------
# Bot exemption — the `pr-intent-lint` job must keep skipping bot-authored PRs.
# A Dependabot body is bot-generated (a hand-added `## Intent` is wiped by
# `@dependabot recreate`), so without the exemption every bump is permanently
# red and the only way through is pinning `intent-out-of-band` on all of them.
# ----------------------------------------------------------------------------

@test "doc-validation: pr-intent-lint exempts dependabot by login, not by user.type" {
    wf="$REPO_ROOT/.github/workflows/doc-validation.yml"
    [ -f "$wf" ]
    # The job's `if:` block: from `pr-intent-lint:` to the next key at job indent.
    job_if="$(awk '/^  pr-intent-lint:/{f=1} f&&/^    runs-on:/{exit} f' "$wf")"
    [ -n "$job_if" ]
    [[ "$job_if" == *"pull_request.user.login != 'dependabot[bot]'"* ]]
    # Login allow-list, deliberately not a blanket bot check: a future bot that
    # opens *product* PRs must stay gated by default.
    [[ "$job_if" != *"user.type"* ]]
}
