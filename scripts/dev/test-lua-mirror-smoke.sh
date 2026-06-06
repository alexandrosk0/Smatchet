#!/usr/bin/env bash
# test-lua-mirror-smoke.sh — supply-chain smoke for the Lua 5.3.6 tarball pin.
#
# Lua is the ONE FetchContent dependency fetched over plain HTTP (lua.org publishes
# no immutable git ref), so it is a single upstream SPOF: a lua.org outage or a
# silently-tampered mirror would only surface mid-build, taking develop red for
# hours (observed: curl error 28 "Timeout was reached" — see
# docs/self-improvement/categories/infra.md). Every other dependency is an
# immutable, SHA-pinned git ref.
#
# This smoke catches that class EARLY. It runs in two modes:
#
#   1. Offline (default) — parse the EXPECTED_HASH pin and the mirror URL list
#      straight out of CMakeLists.txt (the single source of truth — no duplicated
#      literals here), and assert: the sha256 pin is present + well-formed (64 hex),
#      at least one mirror URL is declared, and the github-controlled mirror is the
#      FIRST entry (so the lua.org SPOF stays the fallback, not the primary). Fast,
#      deterministic, network-free — safe under scripts/dev/test-all.sh and the
#      local pre-push gate.
#
#   2. Network (SMATCHET_LUA_SMOKE_NETWORK=1) — additionally download the tarball
#      from EACH declared mirror and verify the bytes hash to the pin. Catches an
#      upstream outage or a tampered mirror before it can break a build. Wired into
#      a dedicated CI job (lua-mirror-smoke) so the local gate stays offline-safe.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 — every assertion passed
#   1 — at least one assertion failed
#   2 — prerequisite missing (CMakeLists.txt unreadable, or curl/sha256 absent in
#       network mode)

set -euo pipefail

cd "$(dirname "$0")/../.."

CMAKELISTS="CMakeLists.txt"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }

if [ ! -r "$CMAKELISTS" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: $CMAKELISTS not readable from $(pwd)"
    exit 2
fi

# --- Parse the pin + mirrors straight out of CMakeLists.txt (single source of truth) ---

# EXPECTED_HASH pin: set(_lua_sha256 "<64 hex>")
EXPECTED_HASH="$(sed -n 's/.*set(_lua_sha256[[:space:]]*"\([0-9a-fA-F]\{64\}\)").*/\1/p' "$CMAKELISTS" | head -n1)"

# Mirror URL list: the quoted https URLs inside the set(_lua_urls ...) block.
# Grab the two lines following the _lua_urls opener and extract their quoted URLs.
mapfile -t LUA_URLS < <(
    sed -n '/set(_lua_urls/,/)/p' "$CMAKELISTS" \
        | sed -n 's/.*"\(https:\/\/[^"]*\)".*/\1/p'
)

# --- Offline assertions (always run) ---

if [ -n "$EXPECTED_HASH" ]; then
    pass "Lua EXPECTED_HASH pin present and well-formed (64-hex sha256): $EXPECTED_HASH"
else
    fail "Lua EXPECTED_HASH pin (_lua_sha256) missing or not a 64-hex sha256 in $CMAKELISTS"
fi

if [ "${#LUA_URLS[@]}" -ge 1 ]; then
    pass "Lua mirror URL list declares ${#LUA_URLS[@]} source(s)"
else
    fail "Lua mirror URL list (_lua_urls) declares no https sources in $CMAKELISTS"
fi

# The github-controlled mirror must be FIRST so the lua.org SPOF is the fallback.
if [ "${#LUA_URLS[@]}" -ge 1 ]; then
    case "${LUA_URLS[0]}" in
        https://raw.githubusercontent.com/*Smatchet*)
            pass "Primary Lua mirror is the github-controlled fork (lua.org is fallback): ${LUA_URLS[0]}" ;;
        *)
            fail "Primary Lua mirror is NOT the github-controlled fork (SPOF risk): ${LUA_URLS[0]}" ;;
    esac
fi

# --- Network assertions (opt-in) ---

if [ "${SMATCHET_LUA_SMOKE_NETWORK:-0}" = "1" ]; then
    if ! command -v curl >/dev/null 2>&1; then
        echo "Passed: $PASS  Failed: $FAIL"
        echo "ERROR: SMATCHET_LUA_SMOKE_NETWORK=1 but curl(1) not found on PATH"
        exit 2
    fi
    # Resolve a sha256 tool (sha256sum on Linux/CI, shasum -a 256 on macOS).
    if command -v sha256sum >/dev/null 2>&1; then
        sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
    elif command -v shasum >/dev/null 2>&1; then
        sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
    else
        echo "Passed: $PASS  Failed: $FAIL"
        echo "ERROR: SMATCHET_LUA_SMOKE_NETWORK=1 but no sha256sum/shasum on PATH"
        exit 2
    fi

    if [ -z "$EXPECTED_HASH" ]; then
        fail "cannot verify mirror bytes — EXPECTED_HASH pin is missing"
    else
        TMP="$(mktemp -d -t lua-mirror-smoke.XXXXXX)" || {
            echo "Passed: $PASS  Failed: $FAIL"
            echo "ERROR: mktemp failed"
            exit 2
        }
        trap 'rm -rf "$TMP"' EXIT

        for url in "${LUA_URLS[@]}"; do
            out="$TMP/lua.tar.gz"
            # -f: fail (non-zero) on HTTP >= 400; -L: follow redirects;
            # --retry: transient-failure resilience; bounded timeouts.
            if curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 30 --max-time 180 \
                    -o "$out" "$url"; then
                got="$(sha256_of "$out")"
                if [ "$got" = "$EXPECTED_HASH" ]; then
                    pass "mirror reachable + sha256 matches pin: $url"
                else
                    fail "mirror reachable but sha256 MISMATCH (got $got, want $EXPECTED_HASH): $url"
                fi
            else
                fail "mirror UNREACHABLE (curl failed): $url"
            fi
            rm -f "$out"
        done
    fi
else
    echo "INFO: network checks skipped (set SMATCHET_LUA_SMOKE_NETWORK=1 to verify mirror reachability + hash)"
fi

echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
