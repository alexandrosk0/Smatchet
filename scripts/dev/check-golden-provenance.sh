#!/usr/bin/env bash
# check-golden-provenance.sh — is every committed bucket-C golden CI-native?
#
# Bucket-C diffs llvmpipe (software-GL) captures against the checked-in goldens.
# For that diff to mean anything, both sides must come from the SAME renderer.
# Goldens bootstrapped on a developer GPU produce whole-region deltas (L_inf
# 214-245 against a tolerance of 4, measured on #2096) that no tolerance value
# can separate from a real screenshot regression — which is why the lane is
# advisory and cannot block (#2099).
#
# This script answers the question that state depends on: was each golden
# produced by the CI bootstrap job, or by somebody's GPU? It recomputes each
# golden's SHA-256 and looks it up in the provenance manifest the bootstrap job
# emits. A golden regenerated locally changes its bytes without gaining a
# manifest row, so it reports as non-CI-native instead of silently reintroducing
# the very drift this exists to prevent.
#
# WARN-first BY DESIGN. Today no golden is CI-native, so a blocking gate here
# would block every PR. It reports and exits 0 unless --strict is passed. Flip
# the CI call site to --strict only once `--list-missing` is empty — the same
# WARN-first calibration the `duplication` gate graduated through, and the
# sequencing `postmortems.md` records (PR #1180 landed a blocking gate while red
# and red-walled develop).
#
# Usage:
#   bash scripts/dev/check-golden-provenance.sh              # report, always exit 0
#   bash scripts/dev/check-golden-provenance.sh --strict     # exit 1 if any golden is not CI-native
#   bash scripts/dev/check-golden-provenance.sh --list-missing  # print only the non-CI-native basenames
#   bash scripts/dev/check-golden-provenance.sh --emit <run-id> <mesa> <geometry>  # write the manifest
#
# --emit is what the CI bootstrap job runs after `--bootstrap` has written fresh
# goldens: it stamps each one with its hash and the environment that produced it,
# so the manifest travels with the PNGs and a reviewer can see WHICH run and
# WHICH renderer version blessed them. Lives here rather than in a second script
# so the hashing rule can never drift between writer and checker.
#
# Env overrides:
#   SMATCHET_GOLDEN_DIR   golden PNG dir (default tests/golden)
#   SMATCHET_GOLDEN_PROVENANCE  manifest path (default <golden dir>/PROVENANCE.tsv)
#
# Exit codes:
#   0 — reported successfully (default), or --strict with every golden CI-native
#   1 — --strict and at least one golden is not CI-native
#   2 — the golden dir does not exist (misconfiguration, not a verdict)

set -uo pipefail

GOLDEN_DIR="${SMATCHET_GOLDEN_DIR:-tests/golden}"
MANIFEST="${SMATCHET_GOLDEN_PROVENANCE:-$GOLDEN_DIR/PROVENANCE.tsv}"

STRICT=0
LIST_ONLY=0
EMIT=0
EMIT_RUN=""
EMIT_MESA=""
EMIT_GEOM=""
case "${1:-}" in
    --strict) STRICT=1 ;;
    --list-missing) LIST_ONLY=1 ;;
    --emit)
        EMIT=1
        EMIT_RUN="${2:-unknown}"
        EMIT_MESA="${3:-unknown}"
        EMIT_GEOM="${4:-unknown}"
        ;;
    "") ;;
    *)
        echo "check-golden-provenance: unknown argument '$1'" >&2
        exit 2
        ;;
esac

if [ ! -d "$GOLDEN_DIR" ]; then
    echo "check-golden-provenance: golden dir '$GOLDEN_DIR' does not exist" >&2
    exit 2
fi

# sha256sum is coreutils (present in Git-for-Windows bash, which is what the CI
# lane uses); shasum is the BSD/macOS fallback. Resolve once.
sha_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Manifest rows are `<sha256>\t<basename>\t<run-id>\t<mesa>\t<geometry>`; comment
# and blank lines are ignored. Only the sha+basename PAIR authorises a golden —
# matching on basename alone would let edited bytes keep a stale row's blessing,
# and matching on sha alone would let a golden be renamed into another scenario.
manifest_has() {
    local want_sha="$1" want_name="$2"
    [ -f "$MANIFEST" ] || return 1
    while IFS=$'\t' read -r sha name _rest; do
        case "$sha" in ''|'#'*) continue ;; esac
        if [ "$sha" = "$want_sha" ] && [ "$name" = "$want_name" ]; then
            return 0
        fi
    done < "$MANIFEST"
    return 1
}

if [ "$EMIT" -eq 1 ]; then
    {
        echo "# tests/golden/PROVENANCE.tsv — which goldens are CI-native, and from where."
        echo "#"
        echo "# Written by the bucket-C golden bootstrap job; verified by"
        echo "# scripts/dev/check-golden-provenance.sh. A golden whose bytes are not listed"
        echo "# here was produced off-CI (a developer GPU), and diffing llvmpipe captures"
        echo "# against it is not authoritative — see #2099."
        echo "#"
        echo "# Columns: sha256<TAB>basename<TAB>run-id<TAB>mesa<TAB>capture-geometry"
        for png in "$GOLDEN_DIR"/*.png; do
            [ -e "$png" ] || continue
            printf '%s\t%s\t%s\t%s\t%s\n' \
                "$(sha_of "$png")" "$(basename "$png")" "$EMIT_RUN" "$EMIT_MESA" "$EMIT_GEOM"
        done
    } > "$MANIFEST"
    echo "check-golden-provenance: wrote $MANIFEST"
    exit 0
fi

total=0
native=0
missing=()

# Nullglob-free: guard the literal-glob case so an empty dir reports 0 goldens
# rather than trying to hash a path called '*.png'.
for png in "$GOLDEN_DIR"/*.png; do
    [ -e "$png" ] || continue
    total=$((total + 1))
    base="$(basename "$png")"
    if manifest_has "$(sha_of "$png")" "$base"; then
        native=$((native + 1))
    else
        missing+=("$base")
    fi
done

if [ "$LIST_ONLY" -eq 1 ]; then
    for m in ${missing+"${missing[@]}"}; do
        echo "$m"
    done
    exit 0
fi

echo "golden provenance: $native/$total CI-native (manifest: $MANIFEST)"
if [ "${#missing[@]}" -gt 0 ]; then
    echo "  NOT CI-native (bootstrapped off-CI, or edited since):"
    for m in ${missing+"${missing[@]}"}; do
        echo "    $m"
    done
    echo "  Regenerate via the bucket-C golden bootstrap job (workflow_dispatch on"
    echo "  build-and-test.yml with bootstrap_goldens=true), review the uploaded PNGs,"
    echo "  then commit them WITH the PROVENANCE.tsv the job emits."
    echo "  The human approval step is not optional — docs/agent-rules/golden-image-approval.md."
    if [ "$STRICT" -eq 1 ]; then
        exit 1
    fi
fi
exit 0
