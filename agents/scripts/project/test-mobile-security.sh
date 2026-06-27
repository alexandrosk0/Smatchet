#!/usr/bin/env bash
# test-mobile-security.sh — Android security guardrails (WS1, Issues #1067 + #1068;
# gradle wrapper pin from the security-audit follow-up).
# ----------------------------------------------------------------------------
# Three invariants this gate locks in, all file-content regressions that the
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
#   wrapper  gradle/wrapper/gradle-wrapper.properties must pin distributionSha256Sum
#          to the official Gradle 8.7 -bin zip, AND the committed gradle-wrapper.jar
#          must match the official Gradle 8.7 wrapper-jar sha256. The jar runs at
#          build time and the wrapper fetches+executes the distribution, so an
#          unpinned distribution or a swapped jar is build-time code execution.
#          (Security-audit follow-up: "gradle-wrapper-jar integrity".)
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
# EXIT  0 all invariants hold · 1 a violation · 2 infra error.
# Goes through test-shell-lint.sh (5 rules) + the shellcheck -S warning fail-set.
# Requires GNU grep (-P/-z) + coreutils sha256sum — both present on the
# ubuntu-latest CI runner and git-bash.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

MANIFEST_REL="Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml"
CMAKE_REL="cmake/SmatchetThirdParty.cmake"
WRAPPER_PROPS_REL="Source/Mobile/AndroidApp/gradle/wrapper/gradle-wrapper.properties"
WRAPPER_JAR_REL="Source/Mobile/AndroidApp/gradle/wrapper/gradle-wrapper.jar"

# Official Gradle 8.7 checksums (https://gradle.org/release-checksums/). The dist
# sum is what distributionSha256Sum must pin; the wrapper-jar sum attests the
# committed gradle-wrapper.jar is the genuine, unmodified Gradle 8.7 wrapper.
# Bump BOTH in lockstep with any gradle-x.y-bin.zip version change in the wrapper
# properties (refresh the values from the release-checksums page above).
GRADLE_DIST_SHA256='544c35d6bd849ae8a5ed0bcea39ba677dc40f49df7d1835561582da2009b961d'
GRADLE_WRAPPER_JAR_SHA256='cb0da6751c2b753a16ac168bb354870ebb1e162e9083f116729cec9c781156b8'

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

# check_wrapper <properties> <jar> — 0 if the Gradle wrapper supply chain is
# pinned, 1 otherwise. Two distinct controls (audit: gradle-wrapper-jar integrity):
#   (a) gradle-wrapper.properties pins distributionSha256Sum to the official 8.7
#       -bin zip — without it the wrapper executes whatever the distributionUrl
#       serves, unverified (a redirected/substituted distribution runs arbitrarily).
#   (b) the committed gradle-wrapper.jar matches the official Gradle 8.7 wrapper
#       sha256 — the jar runs at build time, so a tampered jar is arbitrary code
#       execution on every developer/CI build.
check_wrapper() {
    local props="$1" jar="$2" rc=0 got
    if [ ! -r "$props" ]; then
        echo "test-mobile-security: unreadable gradle-wrapper.properties: $props" >&2
        return 2
    fi
    # (a) distributionSha256Sum present AND pinning the exact official 8.7 dist sum.
    if ! grep -Eq "^[[:space:]]*distributionSha256Sum[[:space:]]*=[[:space:]]*${GRADLE_DIST_SHA256}[[:space:]]*\$" "$props"; then
        echo "FAIL (wrapper): $props does not pin distributionSha256Sum=${GRADLE_DIST_SHA256}" >&2
        echo "             without it the wrapper runs whatever zip distributionUrl serves, unverified." >&2
        rc=1
    fi
    # (b) committed gradle-wrapper.jar == genuine, unmodified Gradle 8.7 wrapper jar.
    if [ ! -r "$jar" ]; then
        echo "test-mobile-security: unreadable gradle-wrapper.jar: $jar" >&2
        return 2
    fi
    got="$(sha256sum "$jar" 2>/dev/null | cut -d' ' -f1)" || {
        echo "test-mobile-security: sha256sum failed on $jar" >&2
        return 2
    }
    if [ "$got" != "$GRADLE_WRAPPER_JAR_SHA256" ]; then
        echo "FAIL (wrapper): $jar sha256 $got != official Gradle 8.7 $GRADLE_WRAPPER_JAR_SHA256" >&2
        echo "             a tampered gradle-wrapper.jar runs arbitrary code at build time." >&2
        rc=1
    fi
    return "$rc"
}

run_check() {
    local rc=0
    check_manifest "$ROOT/$MANIFEST_REL" || rc=$?
    check_cmake    "$ROOT/$CMAKE_REL"    || rc=$?
    check_wrapper  "$ROOT/$WRAPPER_PROPS_REL" "$ROOT/$WRAPPER_JAR_REL" || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "PASS — Android security guardrails hold (#1067 allowBackup=false, #1068 OpenSSL fail-fast, gradle wrapper pinned)."
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

    # 8. gradle-wrapper.properties WITHOUT the distributionSha256Sum pin must FAIL.
    printf 'distributionUrl=https\\://services.gradle.org/distributions/gradle-8.7-bin.zip\n' > "$tmp/no-pin.properties"
    if check_wrapper "$tmp/no-pin.properties" "$ROOT/$WRAPPER_JAR_REL" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: missing distributionSha256Sum pin was NOT detected" >&2
        miss=1
    fi
    # 9. Correct pin + the genuine committed wrapper jar must PASS (also re-attests the real jar).
    printf 'distributionSha256Sum=%s\n' "$GRADLE_DIST_SHA256" > "$tmp/ok.properties"
    if ! check_wrapper "$tmp/ok.properties" "$ROOT/$WRAPPER_JAR_REL" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: correct pin + genuine wrapper jar was wrongly rejected" >&2
        miss=1
    fi
    # 10. Correct pin + a tampered jar (wrong sha256) must FAIL.
    printf 'tampered-wrapper-jar\n' > "$tmp/bad.jar"
    if check_wrapper "$tmp/ok.properties" "$tmp/bad.jar" >/dev/null 2>&1; then
        echo "SELFTEST FAIL: tampered gradle-wrapper.jar slipped past the sha256 check" >&2
        miss=1
    fi

    if [ "$miss" -eq 0 ]; then
        echo "selftest: all three invariants enforced (manifest: live-attr + whitespace + comment-literal cases; cmake: WARNING-fallback + FATAL_ERROR→WARNING downgrade caught; wrapper: missing-pin + tampered-jar caught; clean fixtures pass)."
        return 0
    fi
    return 1
}

case "${1:---check}" in
    --check)    run_check ;;
    --selftest) run_selftest ;;
    *) echo "usage: $0 [--check|--selftest]" >&2; exit 2 ;;
esac
