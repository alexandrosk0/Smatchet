#!/bin/bash
# is-pure-docs-diff: exit 0 if the ahead-range diff against the given base
# branch is strictly within the pure-docs allow-list:
#   - docs/**
#   - backlog/**
#   - AGENTS.md
#   - root-level *.md (e.g. README.md, CONTEXT.md, CLAUDE.md)
#
# Exit 1 if any file is outside the allow-list (deny-list catches scripts/,
# agents/, tests/, .gitignore, .github/, CMake files, C++/Lua/Python/shell).
#
# Used by agents/core/git-janitor.md § FF-clean docs-batch exception §
# Pure-docs sub-exception to skip the test-all.sh gate on pure-doc diffs.
#
# Plan: docs/plans/shipped/process-backlog-tighten-1-2-3-9-11-12.md § Slice 2
#
# Usage:
#   bash agents/scripts/core/is-pure-docs-diff.sh [base-branch]   # default: develop

set -euo pipefail

base="${1:-develop}"

# Resolve base — accept either "develop" (assume origin/develop) or a fully
# qualified ref (e.g. "origin/main"). If the caller gave a bare name and an
# origin/<name> exists, prefer that.
if git rev-parse --verify "origin/$base" >/dev/null 2>&1; then
    base_ref="origin/$base"
elif git rev-parse --verify "$base" >/dev/null 2>&1; then
    base_ref="$base"
else
    echo "is-pure-docs-diff: cannot resolve base ref '$base' or 'origin/$base'" >&2
    exit 2
fi

# Empty diff = pure-docs by definition (nothing to validate).
files=$(git diff --name-only "$base_ref...HEAD")
if [ -z "$files" ]; then
    exit 0
fi

# Allow-list as ERE — line must match one of:
#   docs/...        backlog/...        AGENTS.md (exact)        ROOTCAPS.md (exact)
# Root *.md restricted to uppercase-letter-and-underscore-only filenames so
# the discriminator stays tight (README.md, CONTEXT.md, CLAUDE.md, BUILD.md
# all match; lowercase foo.md does not — those don't exist at repo root today).
allow='^(docs/|backlog/|agents/scripts/|AGENTS\.md$|[A-Z][A-Z_]*\.md$)'

if printf '%s\n' "$files" | grep -qvE -- "$allow"; then
    exit 1
fi

exit 0
