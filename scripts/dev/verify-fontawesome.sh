#!/usr/bin/env bash
# verify-fontawesome.sh — network-free integrity check of the COMMITTED Font
# Awesome TTF against its pinned sha256.
#
# Called by the `.github/actions/fetch-fontawesome` composite action, which owns
# the pin (FA_TAG + FA_SHA256 — bump them there, in lockstep with the file).
# The TTF is committed (assets/fonts/fa-solid-900.ttf; `.gitattributes` marks
# `*.ttf binary`, so no EOL rewrite on a Windows checkout), so CI no longer
# re-downloads the same immutable bytes from a third-party host once per job: a
# 429 rate-limit or a transient 404 on that host used to red required build jobs
# with no author-actionable cause (backlog entries
# tooling/2026-08-17-required-check-fetches-font-from-unmirrored-external-url +
# infra/2026-08-18-fontawesome-fetch-single-source-no-fallback). The sha256 pin
# stays the trust anchor: this check fails closed when the file is missing,
# empty, or its bytes differ from FA_SHA256.
#
# Env:
#   FA_SHA256  expected sha256, 64 lowercase hex chars (required)
#   FA_TAG     upstream Font Awesome tag the pin was taken from (messages only)
#   FA_FILE    file to verify; default assets/fonts/fa-solid-900.ttf, resolved
#              against the repo root (an absolute path is used as-is)
#
# Exit: 0 verified · 1 missing / empty / sha256 mismatch · 2 usage (malformed
#       FA_SHA256, no sha256 tool on PATH)
set -euo pipefail

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,24p' "$0"
    exit 0
fi

cd "$(dirname "$0")/../.." || exit 2

FA_FILE="${FA_FILE:-assets/fonts/fa-solid-900.ttf}"
FA_TAG="${FA_TAG:-unknown}"
FA_SHA256="${FA_SHA256:-}"
PIN_HOME=".github/actions/fetch-fontawesome/action.yml"

if ! [[ "$FA_SHA256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "::error title=FONT-PIN-INVALID::verify-fontawesome: FA_SHA256 must be 64 lowercase hex chars (got '${FA_SHA256}') — fix the pin in ${PIN_HOME}"
    exit 2
fi

# sha256sum (GNU coreutils; Git Bash on the Windows runners) or shasum (macOS).
# Both read the same `<hash>  <path>` check-line format.
if command -v sha256sum >/dev/null 2>&1; then
    SHA_CMD=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    SHA_CMD=(shasum -a 256)
else
    echo "::error title=FONT-VERIFY-NO-TOOL::verify-fontawesome: neither sha256sum nor shasum is on PATH"
    exit 2
fi

if [ ! -s "$FA_FILE" ]; then
    echo "::error title=FONT-ASSET-MISSING::${FA_FILE} is missing or empty. It is committed to the repo (Font Awesome ${FA_TAG}) and CI no longer downloads it — restore it with: git checkout -- ${FA_FILE}"
    exit 1
fi

if ! printf '%s  %s\n' "$FA_SHA256" "$FA_FILE" | "${SHA_CMD[@]}" -c - >/dev/null 2>&1; then
    actual="$("${SHA_CMD[@]}" "$FA_FILE" | cut -d' ' -f1)"
    echo "::error title=FONT-ASSET-MISMATCH::${FA_FILE} sha256 ${actual} != pinned ${FA_SHA256} (Font Awesome ${FA_TAG}). Restore the pinned bytes (git checkout -- ${FA_FILE}), or bump FA_TAG + FA_SHA256 in ${PIN_HOME} in lockstep with the committed file."
    exit 1
fi

size="$(wc -c < "$FA_FILE" | tr -d '[:space:]')"
echo "verify-fontawesome: OK — ${FA_FILE} (${size} bytes) matches the pinned sha256 for Font Awesome ${FA_TAG}."
exit 0
