# shellcheck shell=bash
# lib/agent-layer-image.sh — the ONE definition of the agent layer's scaffold image.
#
# The image is every file COMMITTED under seed-agent-layer-repo.d/ at a given
# commit, minus the seed-time input seed-scrub-paths.txt; <image>/X lands at
# <layer-root>/X. Sourced by seed-agent-layer-repo.sh (phase 4 copy, phase 4b
# exemption) and agent-layer-sim.sh (image build), so the simulated tree and the
# seeded tree are the same tree by construction.
#
# Committed files only, never the working tree: a file git does not track — an
# ignored stray log, an editor backup — would otherwise be copied into a public
# repo, and phase 4b would wave it through as "scaffold". Phase 2's dirty check
# cannot see ignored files, so reading git objects is the only closed rule.
#
# Plan: docs/plans/agent-surface-extraction-repo.md — Phase B rows 8 and 9.

ALI_SCAFFOLD_REL="agents/scripts/core/seed-agent-layer-repo.d"
ALI_NON_IMAGE="seed-scrub-paths.txt"

# ali_image_files <repo> <commit> — image-relative paths, sorted, one per line.
# Non-zero if the commit carries no scaffold at all.
ali_image_files() {
    local repo="$1" sha="$2" list
    list="$(git -C "$repo" ls-tree -r --name-only "$sha" -- "$ALI_SCAFFOLD_REL/")" || return 2
    [ -n "$list" ] || return 2
    printf '%s\n' "$list" | cut -c"$(( ${#ALI_SCAFFOLD_REL} + 2 ))"- \
        | { grep -vxF "$ALI_NON_IMAGE" || true; } | sort
}

# ali_extract_image <repo> <commit> <dest> — write the image into the existing
# directory <dest>, file modes preserved. Non-zero on any failure.
ali_extract_image() {
    local repo="$1" sha="$2" dest="$3" stage rc=0
    stage="$(mktemp -d "${TMPDIR:-/tmp}/agent-layer-image.XXXXXX")" || return 2
    if git -C "$repo" archive --format=tar "$sha" -- "$ALI_SCAFFOLD_REL" | tar -x -C "$stage"; then
        rm -f "$stage/$ALI_SCAFFOLD_REL/$ALI_NON_IMAGE"
        ( cd "$stage/$ALI_SCAFFOLD_REL" && tar -cf - . ) | tar -x -C "$dest" || rc=2
    else
        rc=2
    fi
    rm -rf "$stage"
    return "$rc"
}
