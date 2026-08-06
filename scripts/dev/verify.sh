#!/usr/bin/env bash
# scripts/dev/verify.sh — build a target, then run the delta-lint gates a bare
# build skips.
#
# Bash port of the retired verify.ps1.
#
# Closes the "build-verify shortcut bypasses the lint gate" gap: a plain
# `cmake --build` (or build-and-run.sh --build-only) proves the code compiles but
# does NOT run the comment-noise / function-size / strict-zone delta gates CI
# enforces. This builds first, then runs exactly those gates against the
# merge-base with <base> (default origin/develop), so a comment-blank-run or a
# clang-format-reflowed comment is caught locally, not three minutes later in CI.
#
# Steps:
#   1. scripts/dev/local/build-and-run.sh --build-only
#   2. comment_audit.py --diff <base>
#   3. test-lint-rules.sh --diff <base>
#
# For the full pre-push gate (clang-format -i, markdown lint, doc-validation,
# review ack) use `bash scripts/dev/pre-ship.sh` — this is the
# build-then-delta-lint shortcut, not a pre-ship replacement.
#
# Exit codes: 0 = build + delta-lint clean; non-zero = build or a gate failed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# shellcheck source=scripts/common/smatchet-cmake-common.sh
. "$REPO_ROOT/scripts/common/smatchet-cmake-common.sh"

PRESET="ninja-iter-msvc"
TARGET="SmatchetStandalone"
BASE="origin/develop"
SKIP_BUILD=0

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/verify.sh [options]

  --preset <name>   CMake preset (default: ninja-iter-msvc)
  --target <name>   CMake target (default: SmatchetStandalone)
  --base <ref>      Delta base for the lint gates (default: origin/develop)
  --skip-build      Run the lint gates only; do not compile
  -h, --help        Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)     PRESET="${2:-}"; shift 2 ;;
        --target)     TARGET="${2:-}"; shift 2 ;;
        --base)       BASE="${2:-}"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)
            echo "verify: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

die() { echo "verify: $*" >&2; exit 1; }

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "verify: building $TARGET ($PRESET) ..."
    bash "$REPO_ROOT/scripts/dev/local/build-and-run.sh" \
        --preset "$PRESET" --target "$TARGET" --build-only \
        || die "build failed (exit $?)."
else
    echo "verify: --skip-build set - running delta-lint gates only."
fi

PY="$(smatchet_python)" || exit 1

cd "$REPO_ROOT" || die "cannot cd to $REPO_ROOT."

# Comment-noise delta gate (exit 1 = violations, >=2 = infra failure).
echo "verify: comment-noise delta gate vs $BASE ..."
"$PY" agents/scripts/core/comment_audit.py --diff "$BASE" \
    || die "comment-noise delta gate found violations (exit $?) - fix before pushing."

# Full delta lint gate (comment-noise + function-size + strict-zone).
echo "verify: delta lint gate (test-lint-rules.sh) vs $BASE ..."
bash agents/scripts/project/test-lint-rules.sh --diff "$BASE" \
    || die "delta lint gate failed (exit $?) - fix before pushing."

echo "verify: PASS - build green + comment-noise + delta lint gate clean."
