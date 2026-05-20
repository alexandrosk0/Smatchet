#!/usr/bin/env bash
# Claude Code wrapper — runs scripts/dev/pillar2-scan.sh on each file in the
# deferred-lint drain chunk. Sourced from lint-cpp-drain.sh after the cppcheck
# pass. Thin shim by design — the actual scan logic lives at
# scripts/dev/pillar2-scan.sh so it's harness-agnostic (Codex / Cursor invoke
# the same canonical script via their own mechanisms; see
# docs/harness/<harness>/hooks-equivalent.md).

set -uo pipefail

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"

# The drain hook passes a space-separated list of absolute project-relative
# paths via $@. Filter to existing .cpp/.h files; pillar2-scan.sh does its
# own first-party + UI-reachable filtering so we forward everything.
declare -a candidates=()
for arg in "$@"; do
    rel="${arg#$PROJ_DIR/}"
    case "$rel" in
        *.cpp|*.h|*.hpp) candidates+=("$rel") ;;
    esac
done

if [[ ${#candidates[@]} -eq 0 ]]; then
    exit 0
fi

cd "$PROJ_DIR"
bash scripts/dev/pillar2-scan.sh "${candidates[@]}"
