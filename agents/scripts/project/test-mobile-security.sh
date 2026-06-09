#!/usr/bin/env bash
# test-mobile-security.sh — Android security guardrails (WS1, Issues #1067 + #1068).
# ----------------------------------------------------------------------------
# Two invariants this gate locks in, both file-content regressions that the
# advisory mobile CI jobs (posix-core / android-ndk / apk) do NOT catch:
#
#   #1067  AndroidManifest.xml must keep android:allowBackup="false". The app's
#          private files dir holds the extracted CA bundle (and, once Keystore
#          lands, credential material); allowBackup="true" would let Auto Backup
#          / adb backup copy it off-device. A one-character regression silently
#          re-opens the hole — only a content gate catches it.
#
#   #1068  cmake/SmatchetThirdParty.cmake must FAIL-FAST (FATAL_ERROR) when a
#          SMATCHET_ANDROID_OPENSSL_BASE is set but the per-ABI static OpenSSL is
#          incomplete. The prior message(WARNING)+sysroot-fallback shipped a
#          TLS-broken APK silently.
#
# CONTROL-FLOW, NOT PHRASE-PRESENCE. The #1068 threat IS a FATAL_ERROR→WARNING
# downgrade, so a gate that merely greps for the marker SENTENCE is bypassed by a
# downgrade that keeps the sentence (message(WARNING "...Android requires a pinned
# static OpenSSL...")). check_cmake therefore binds the marker to a
# message(FATAL_ERROR ...) call and FAILS if the marker sits inside a
# message(WARNING ...). check_manifest strips XML comments first so a literal
# parked in a comment can neither satisfy nor trip the attribute checks, and
# matches whitespace-tolerantly so `allowBackup = "true"` (spaced) is still caught.
#
# Routed onto the merge-gate poller BLOCKING path (agents/scripts/core/
# merge-gates.sh allow-list) via the workflow check name "Android security gate",
# NOT left advisory — the advisory mobile jobs let a green develop ship mobile
# breakage (precedent #1021/#1064).
#
# KNOWN LIMITATION (#3, deferred to backlog): the poller blocks this gate only
# when it is PRESENT-and-red. A PR that deletes/renames .github/workflows/
# mobile-security.yml in its own diff makes the check absent, and the poller
# treats an absent NON-required check as pass. That self-disable is visible in
# diff review; closing it needs a poller-wide present-assertion for allow-listed
# checks (out of WS1 scope).
#
# MODES
#   (no args) / --check   fail if either invariant is violated in the real tree.
#   --selftest            self-contained proof on synthesized fixtures, incl. the
#                         FATAL_ERROR→WARNING downgrade and the comment-literal
#                         false-PASS that the naive grep version missed.
#
# EXIT  0 both invariants hold · 1 a violation · 2 infra error.
# Goes through test-shell-lint.sh (5 rules) + the shellcheck -S warning fail-set.
# Requires GNU grep (-P/-z) — present on the ubuntu-latest CI runner and git-bash.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

MANIFEST_REL="Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml"
CMAKE_REL="cmake/SmatchetThirdParty.cmake"

# Whitespace-tolerant attribute matchers (grep -E). XML allows S? '=' S? around
# the equals, so `android:allowBackup = "true"` is valid and must still be caught.
BACKUP_BAD_RE='android:allowBackup[[:space:]]*=[[:space:]]*"true"'
BACKUP_GOOD_RE='android:allowBackup[[:space:]]*=[[:space:]]*"false"'

# The fail-fast marker sentence and the retired silent-fallback warning.
OSSL_MARKER='Android requires a pinned static OpenSSL'
OSSL_RETIRED_WARNING='OpenSSL find_package will fall back to probing'

# Control-flow PCRE (GNU grep -Pz, dotall): the marker must follow a
# message(FATAL_ERROR with NO intervening message( — so a far-off message() can't
# satisfy it by accident. The WARN variant proves the marker is NOT inside a
# downgraded message(WARNING ...).
OSSL_FATAL_RE="(?s)message\\s*\\(\\s*FATAL_ERROR(?:(?!message\\s*\\().)*?${OSSL_MARKER}"
OSSL_WARN_RE="(?s)message\\s*\\(\\s*WARNING(?:(?!message\\s*\\().)*?${OSSL_MARKER}"

# strip_xml_comments <path> — emit the file with <!-- ... --> spans (incl.
# multiline) removed, so a literal living in a comment cannot affect the gate.
# Pure awk — no extra dependency.
strip_xml_comments() {
    awk '
    {
        line = $0
        while (1) {
            if (in_cmt) {
                p = index(line, "-->")
                if (p == 0) { line = ""; break }
                line = substr(line, p + 3); in_cmt = 0
            } else {
                p = index(line, "<!--")
                if (p == 0) break
                rest = substr(line, p + 4)
                q = index(rest, "-->")
                if (q == 0) { line = substr(line, 1, p - 1); in_cmt = 1; break }
                line = substr(line, 1, p - 1) substr(rest, q + 3)
            }
        }
        print line
    }' "$1"
}

# check_manifest <path> — 0 if allowBackup is locked false on a LIVE (non-comment)
# attribute, 1 otherwise.
check_manifest() {
    local f="$1" rc=0 body
    if [ ! -r "$f" ]; then
        echo "test-mobile-security: unreadable manifest: $f" >&2
        return 2
    fi
    body="$(strip_xml_comments "$f")" || {
        echo "test-mobile-security: awk comment-strip failed on $f" >&2
        return 2
    }
    if printf '%s\n' "$body" | grep -Eq "$BACKUP_BAD_RE"; then
        echo "FAIL (#1067): a live android:allowBackup=\"true\" in $f — backups copy the private files dir (CA bundle / creds) off-device." >&2
        echo "             Restore android:allowBackup=\"false\"." >&2
        rc=1
    fi
    if ! printf '%s\n' "$body" | grep -Eq "$BACKUP_GOOD_RE"; then
        echo "FAIL (#1067): no live android:allowBackup=\"false\" in $f (a literal inside an XML comment does not count) — the explicit opt-out must stay present." >&2
        rc=1
    fi
    return "$rc"
}

# check_cmake <path> — 0 if the OpenSSL discovery fails-fast (marker bound to a
# message(FATAL_ERROR call), 1 otherwise.
check_cmake() {
    local f="$1" rc=0
    if [ ! -r "$f" ]; then
        echo "test-mobile-security: unreadable cmake: $f" >&2
        return 2
    fi
    # The marker must live inside a message(FATAL_ERROR ...) call.
    if ! grep -Pzq "$OSSL_FATAL_RE" "$f"; then
        echo "FAIL (#1068): the fail-fast marker '$OSSL_MARKER' is not bound to a message(FATAL_ERROR ...) in $f —" >&2
        echo "             the per-ABI OpenSSL miss must FATAL_ERROR, not WARNING+sysroot fallback." >&2
        rc=1
    fi
    # ...and must NOT have been downgraded into a message(WARNING ...) call (the exact #1068 regression).
    if grep -Pzq "$OSSL_WARN_RE" "$f"; then
        echo "FAIL (#1068): the fail-fast marker is inside a message(WARNING ...) in $f — severity downgraded." >&2
        echo "             A missing per-ABI OpenSSL must FATAL_ERROR (fail the configure), not warn-and-probe." >&2
        rc=1
    fi
    # Belt-and-suspenders: the exact retired silent-fallback warning must stay gone.
    if grep -qF "$OSSL_RETIRED_WARNING" "$f"; then
        echo "FAIL (#1068): retired silent-fallback warning '$OSSL_RETIRED_WARNING' is back in $f." >&2
        rc=1
    fi
    return "$rc"
}

run_check() {
    local rc=0
    check_manifest "$ROOT/$MANIFEST_REL" || rc=$?
    check_cmake    "$ROOT/$CMAKE_REL"    || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "PASS — Android security guardrails hold (#1067 allowBackup=false, #1068 OpenSSL fail-fast)."
    fi
    return "$rc"
}

run_selftest() {
    local tmp miss=0
    tmp="$(mktemp -d)" || { echo "test-mobile-security: mktemp failed" >&2; return 2; }
    # shellcheck disable=SC2064  # expand $tmp now so the trap removes the right dir.
    trap "rm -rf '$tmp'" RETURN

    # selftest: asserts-failure — every planted regression below must be detected
    # (a live allowBackup="true", a spaced-equals "true", a comment-only "false",
    # a WARNING+marker downgrade, a retired-warning fallback); clean fixtures pass.

    # 1. Bad manifest (live allowBackup="true") must FAIL.
    printf '<application android:allowBackup="true"/>\n' > "$tmp/bad-manifest.xml"
    if check_manifest "$tmp/bad-manifest.xml" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: planted live allowBackup=\"true\" was NOT detected" >&2
        miss=1
    fi
    # 2. Clean manifest must PASS.
    printf '<application android:allowBackup="false"/>\n' > "$tmp/ok-manifest.xml"
    if ! check_manifest "$tmp/ok-manifest.xml" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: clean manifest (allowBackup=\"false\") was wrongly rejected" >&2
        miss=1
    fi
    # 3. Spaced-equals true (android:allowBackup = "true") must FAIL (whitespace-tolerant).
    printf '<application android:allowBackup = "true"/>\n' > "$tmp/spaced-manifest.xml"
    if check_manifest "$tmp/spaced-manifest.xml" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: spaced-equals allowBackup = \"true\" slipped past the BAD matcher" >&2
        miss=1
    fi
    # 4. Comment-only "false" with no live attribute must FAIL (literal in a comment does not count).
    printf '<!-- android:allowBackup="false" was here -->\n<application android:label="x"/>\n' > "$tmp/comment-manifest.xml"
    if check_manifest "$tmp/comment-manifest.xml" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: comment-only allowBackup=\"false\" (no live attribute) was wrongly accepted" >&2
        miss=1
    fi

    # 5. Bad cmake (retired WARNING, no FATAL marker) must FAIL.
    printf 'message(WARNING "%s")\n' "$OSSL_RETIRED_WARNING" > "$tmp/bad.cmake"
    if check_cmake "$tmp/bad.cmake" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: planted WARNING-fallback cmake was NOT detected" >&2
        miss=1
    fi
    # 6. Clean cmake (marker inside message(FATAL_ERROR ...)) must PASS.
    printf 'message(FATAL_ERROR\n  "%s — silent fallback ships a TLS-broken build. (Issue #1068)")\n' "$OSSL_MARKER" > "$tmp/ok.cmake"
    if ! check_cmake "$tmp/ok.cmake" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: clean fail-fast cmake was wrongly rejected" >&2
        miss=1
    fi
    # 7. DOWNGRADE: marker kept but moved into message(WARNING ...) must FAIL (the #1068 threat).
    printf 'message(WARNING\n  "%s — falling back to sysroot probe")\n' "$OSSL_MARKER" > "$tmp/downgrade.cmake"
    if check_cmake "$tmp/downgrade.cmake" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: FATAL_ERROR→WARNING downgrade (marker preserved) slipped past the gate" >&2
        miss=1
    fi

    if [ "$miss" -eq 0 ]; then
        echo "selftest: both invariants enforced (manifest: live-attr + whitespace + comment-literal cases; cmake: WARNING-fallback + FATAL_ERROR→WARNING downgrade caught; clean fixtures pass)."
        return 0
    fi
    return 1
}

case "${1:---check}" in
    --check)    run_check ;;
    --selftest) run_selftest ;;
    *) echo "usage: $0 [--check|--selftest]" >&2; exit 2 ;;
esac
