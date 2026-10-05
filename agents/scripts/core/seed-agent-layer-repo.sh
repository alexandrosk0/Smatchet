#!/usr/bin/env bash
# seed-agent-layer-repo.sh — one-shot seed of the public agent-layer repo
# (`the-unwilling-agentic-bunch`) from a fresh Smatchet clone, via git-filter-repo.
#
# Plan: docs/plans/agent-surface-extraction-repo.md — Phase B rows 8, 8a-8g.
# Companion detail: docs/plans/active/agent-surface-extraction-repo/phase-b-c.md.
#
# NOT named test-*.sh ON PURPOSE. scripts/dev/test-all.sh discovers tests by the
# test-*.sh glob and would auto-enrol this into CI — a destructive cross-repo
# history rewrite plus a public push, run on every lane. The name is load-bearing.
#
# The seed is an ALLOWLIST (seed-agent-layer-repo.d/seed-paths.txt), never a
# subtraction: nothing reaches the public repo that the manifest did not name and
# the publication audit did not clear. Publishing is one-way — a public history
# cannot be un-published — so phase 5 hard-refuses unless phase 4b is clean.
#
#   0  seeded (or --dry-run manifest review passed)
#   1  assertion failed
#   2  usage error, or required tooling missing
#   3  target repo not empty
#
# Human preconditions (pause exception 3 — cross-repo / external-service
# mutation; this script does NOT perform them):
#   a) gh repo create alexandrosk0/the-unwilling-agentic-bunch --public
#      with NO auto-init (omit --add-readme / --license / --gitignore) — a
#      non-empty target makes the seed push a non-fast-forward.
#   b) pip install git-filter-repo   (needs git >= 2.24, python >= 3.6)
#   c) CodeRabbit app installed on the new repo.
set -uo pipefail

_SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$_SCRIPT_PATH")" && pwd)"
# SCAFFOLD_DIR is a layout-faithful IMAGE of the seeded repo root: a file at
# <scaffold>/X is written to <layer-root>/X. That is why the manifest and the audit
# table live under `docs/` here too — it keeps README's `docs/...` links resolving
# host-side as well, instead of only after the seed has run.
# The one non-image file is `seed-scrub-paths.txt` (scaffold root): a seed-TIME
# input consumed by phase 3, never copied into the layer.
SCAFFOLD_DIR="$SCRIPT_DIR/seed-agent-layer-repo.d"
# The scaffold image rule — committed files under SCAFFOLD_DIR at a commit, minus
# the seed-time input — shared with agent-layer-sim.sh (ali_image_files,
# ali_extract_image).
# shellcheck source=agents/scripts/core/lib/agent-layer-image.sh
. "$SCRIPT_DIR/lib/agent-layer-image.sh"
MANIFEST_SRC="$SCAFFOLD_DIR/docs/seed-paths.txt"
AUDIT_SRC="$SCAFFOLD_DIR/docs/seed-audit.md"

# The layer's default branch stays `develop`, NOT `main`. ~60 of the moved gates
# compute a merge-base delta against the literal `origin/develop`
# (dup_audit.py --diff, agent_size_audit.py --diff, test-lint-rules.sh --diff
# origin/develop, test-markdown-links.sh, is-pure-docs-diff.sh's base_ref
# default). Renaming would turn every one of them into a no-op or a hard error.
LAYER_BRANCH="develop"
SOURCE_URL="https://github.com/alexandrosk0/Smatchet.git"

# Tripwire, not a measurement: the number of suites layer-side wrappers run (see
# layer_named_suites). A mismatch is NOT "bump the number" — a suite entered or
# left the layer. Decide whether it belongs there (its wrapper's location is the
# decision: agents/scripts/** travels, scripts/dev/ stays), re-audit it for
# publication, then set this deliberately. It started at 61 (2026-08-29, under the
# retired `grep -l 'agents/'` rule) and has drifted with the tree ever since.
EXPECTED_BATS_COUNT=67

# Wrapper roots that TRAVEL with the layer, and the host root that does not. A
# suite goes where its wrapper goes: scripts/dev/test-all.sh discovers tests by the
# test-*.sh glob and never runs a .bats directly, so a suite split from its wrapper
# reds a repo after the flip — an orphan suite on one side, a wrapper pointing at a
# deleted file on the other.
LAYER_WRAPPER_ROOTS=(agents/scripts/core agents/scripts/project)
HOST_WRAPPER_ROOTS=(scripts/dev)

# Paths that must NEVER appear in the manifest (grill decision 4 + plan scope).
FORBIDDEN_PREFIXES=(
    docs/self-improvement/categories/
    docs/self-improvement/postmortems.md
    docs/self-improvement/applied.md
    docs/plans/
    docs/work/
    Source/
    agents/project/
    CMakeLists.txt
)

# Phase 4 copies the WHOLE scaffold image of the validated commit into the seeded
# repo (lib/agent-layer-image.sh); agent-layer-sim.sh applies the same rule, so the
# simulated tree is the seeded tree. These lists are the files the image must
# contain — phase 4 refuses to publish a repo without its gates or its contract.
SCAFFOLD_ROW10=(README.md LICENSE project.config.json .gitignore)
SCAFFOLD_ROW9=(
    .coderabbit.yaml
    .github/workflows/agentic-selftests.yml
    .github/workflows/shell-lint.yml
    .github/workflows/doc-validation.yml
    docs/high-integrity/markdown-link-baseline.md
)

DRY_RUN=0
# --simulate: phases 1-2 report-only (like --dry-run), then run the layer CI lanes
# on an image of the validated commit (agent-layer-sim.sh) instead of stopping.
SIMULATE=0
TARGET=""
WORK_DIR=""
SCANNER=""
FAILED=0
# PASSED counts assertions that actually ran and held. FAILED alone only proves
# nothing failed — a phase whose loops iterate zero times would also leave it 0 —
# so each gating phase also demands a floor of PASSED assertions before it may
# report success (the shape-Z zero-run class test-fail-open-authoring.sh guards).
PASSED=0
PUBLISH_CLEARED=0
# Set by phase 4c once the layer's own CI lanes passed on the rewritten clone.
LANES_CLEARED=0
SIM_SCRIPT="$SCRIPT_DIR/agent-layer-sim.sh"
# The exact commit phase 2 validated. Phase 3 refuses a clone that resolves to
# anything else, so the tree that is rewritten and published is the tree that was
# checked — not whatever the remote branch happens to point at by then. Phase 4
# reads the scaffold image from this commit in SOURCE_ROOT, never the work tree.
SOURCE_SHA=""
SOURCE_ROOT=""

usage() {
    cat <<'USAGE'
seed-agent-layer-repo.sh — seed the public agent-layer repo from a Smatchet clone.

  seed-agent-layer-repo.sh --target <owner/repo> [--work-dir DIR] [--dry-run] [--help]
  seed-agent-layer-repo.sh --simulate [--target <owner/repo>]
  seed-agent-layer-repo.sh --print-bats-block

Options (both --key value and --key=value forms are accepted):
  --target <owner/repo>   Destination repo, e.g. alexandrosk0/the-unwilling-agentic-bunch.
                          Required. Must already exist and have ZERO commits.
  --work-dir DIR          Scratch dir for the fresh clone + rewrite. Must NOT exist:
                          git-filter-repo refuses any repo that is not a fresh clone,
                          and the fix is always a new directory, never --force (which
                          also suppresses filter-repo's own not-empty-target check).
                          Default: $TMPDIR/agent-layer-seed.$$
  --dry-run               Run phases 1-2 and stop after printing the manifest.
                          Preflight probes report PASS/WARN instead of exiting, so a
                          dry run is useful BEFORE the human preconditions are met —
                          which is the window it exists for. A real run keeps the
                          strict exit-2 / exit-3 semantics.
  --simulate              Phases 1-2 as --dry-run, then build an image of the
                          validated commit (manifest + scaffold) and run the layer's
                          three CI lanes in it via agent-layer-sim.sh (~25 min).
                          Proves the manifest is COMPLETE, which phase 2 cannot.
                          Manifest paths must be committed (a dirty tree is a hard
                          stop, unlike --dry-run). --target is optional here.
  --print-bats-block      Print the generated tests/bats block of seed-paths.txt
                          (the suites layer-side wrappers run) and exit.
  --help                  This text.

Exit: 0 seeded (or dry-run review passed) | 1 assertion failed
      2 usage / tooling missing           | 3 target not empty

Phases:
  1 preflight   tooling on PATH, gh auth, secret scanner, target exists and is
                empty, work-dir absent
  2 manifest    pin the validated commit (manifest paths must be clean); regenerate
                seed-paths.txt; assert the bats-count tripwire, the 8e
                suite/wrapper co-location invariant, and that no forbidden path
                slipped in; print the manifest.  --dry-run STOPS HERE.
  3 rewrite     fresh clone (--no-local --no-tags --single-branch --branch develop),
                refused unless it resolves to the commit phase 2 validated; then
                git filter-repo --paths-from-file, then the paired
                --invert-paths scrub pass if seed-scrub-paths.txt is non-empty
  4 scaffold    copy the scaffold image (row 9 CI files, row 10 root files,
                .coderabbit.yaml, .gitignore, docs/seed-paths.txt,
                docs/seed-audit.md, ...); commit
  4b audit      history-wide secret scan of the REWRITTEN history, plus assert the
                rewritten `git ls-files` is a SUBSET of the manifest and that every
                manifest path has exactly one CLEAR (or enforced SCRUB) verdict in
                docs/seed-audit.md. Any hit is a hard stop; nothing is pushed until
                this is clean.
  4c lanes      run the layer's own CI lanes on the rewritten clone
                (agent-layer-sim.sh --run-only); red, or any file the lanes
                change or create outside .gitignore, is a hard stop.
  5 publish     hard-refuses unless 4b and 4c cleared; remote add plus push develop;
                gh label create; setup-branch-protection.sh (failure = failed seed)
  6 report      the row-8 Accept checks with PASS/FAIL and the seed SHA

Human preconditions (pause exception 3 — this script does none of them):
  gh repo create <target> --public   with NO auto-init
  pip install git-filter-repo        (git >= 2.24, python >= 3.6)
  CodeRabbit app installed on the new repo
USAGE
}

die() {
    local code="$1"; shift
    printf 'seed-agent-layer-repo: %s\n' "$*" >&2
    exit "$code"
}

say()   { printf '%s\n' "$*"; }
head1()  { printf '\n=== %s ===\n' "$*"; }
pass()  { printf '  PASS  %s\n' "$*"; PASSED=$((PASSED + 1)); }

# Refuse to call a phase clean unless at least $2 assertions passed since $1.
require_floor() {
    local since="$1" min="$2" phase="$3"
    if [ "$PASSED" -lt "$((since + min))" ]; then
        die 1 "$phase ran only $((PASSED - since)) passing assertion(s), expected at least $min — refusing to report it clean"
    fi
}
warn()  { printf '  WARN  %s\n' "$*"; }
fail()  { printf '  FAIL  %s\n' "$*" >&2; FAILED=1; }

# Preflight verdict: hard-exit on a real run, report-only under --dry-run.
probe_fail() {
    local code="$1"; shift
    if [ "$DRY_RUN" -eq 1 ]; then
        warn "$* (ignored: --dry-run)"
        return 0
    fi
    die "$code" "$*"
}

# ---------------------------------------------------------------- flag parsing
parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --help|-h)      usage; exit 0 ;;
            --dry-run)      DRY_RUN=1; shift ;;
            --simulate)     SIMULATE=1; DRY_RUN=1; shift ;;
            --print-bats-block)
                            # The block describes THIS script's repo, wherever it is run from.
                            if ! cd "$SCRIPT_DIR/../../.." || [ ! -d agents/scripts/core ]; then
                                die 2 "cannot resolve the repo root from $SCRIPT_DIR"
                            fi
                            layer_named_suites
                            exit 0 ;;
            --target)       [ "$#" -ge 2 ] || die 2 "--target needs a value"
                            TARGET="$2"; shift 2 ;;
            --target=*)     TARGET="${1#--target=}"; shift ;;
            --work-dir)     [ "$#" -ge 2 ] || die 2 "--work-dir needs a value"
                            WORK_DIR="$2"; shift 2 ;;
            --work-dir=*)   WORK_DIR="${1#--work-dir=}"; shift ;;
            *)              usage >&2; die 2 "unknown argument: $1" ;;
        esac
    done
    if [ -z "$TARGET" ]; then
        [ "$SIMULATE" -eq 1 ] && { [ -n "$WORK_DIR" ] || WORK_DIR="${TMPDIR:-/tmp}/agent-layer-seed.$$"; return 0; }
        usage >&2; die 2 "--target <owner/repo> is required"
    fi
    case "$TARGET" in
        */*/*|/*|*/) die 2 "--target must be exactly owner/repo, got: $TARGET" ;;
        */*)         : ;;
        *)           die 2 "--target must be owner/repo, got: $TARGET" ;;
    esac
    [ -n "$WORK_DIR" ] || WORK_DIR="${TMPDIR:-/tmp}/agent-layer-seed.$$"
}

# ------------------------------------------------------------- phase 1 preflight

# gh_rest_ok — gh can authenticate against the REST API. Every gh call this script
# makes is REST (api, label create; setup-branch-protection.sh likewise), so that is
# the probe: `gh auth status` misreports behind a credential proxy, and `gh repo view`
# is GraphQL, which some environments (fine-grained tokens, sandboxed proxies) refuse.
gh_rest_ok() { gh api user --jq .login >/dev/null 2>&1; }
phase1_preflight() {
    head1 "phase 1 — preflight"

    command -v git >/dev/null 2>&1 || die 2 "git not on PATH"
    pass "git on PATH"

    # git-filter-repo is a git subcommand, not a standalone binary on PATH, so
    # the probe is the documented `git filter-repo --version` (precondition b).
    if git filter-repo --version >/dev/null 2>&1; then
        pass "git-filter-repo available"
    else
        probe_fail 2 "git-filter-repo missing — pip install git-filter-repo (git >= 2.24, python >= 3.6)"
    fi

    if command -v gh >/dev/null 2>&1; then
        pass "gh on PATH"
        if gh_rest_ok; then
            pass "gh authenticated (REST)"
        else
            probe_fail 2 "gh cannot reach the REST API — run: gh auth login"
        fi
    else
        probe_fail 2 "gh not on PATH"
    fi

    # Phase 4b is not optional, so its scanner is a phase-1 precondition.
    if command -v gitleaks >/dev/null 2>&1; then
        SCANNER="gitleaks"
    elif command -v trufflehog >/dev/null 2>&1; then
        SCANNER="trufflehog"
    fi
    if [ -n "$SCANNER" ]; then
        pass "secret scanner: $SCANNER"
    else
        probe_fail 2 "no secret scanner on PATH (need gitleaks or trufflehog) — phase 4b is not optional"
    fi

    # Target must EXIST (human precondition a) and be EMPTY. A non-empty target
    # makes the phase-5 push a non-fast-forward.
    if [ -z "$TARGET" ]; then
        warn "no --target given — target checks skipped (--simulate)"
    elif command -v gh >/dev/null 2>&1 && gh_rest_ok; then
        if gh api "repos/$TARGET" --jq .full_name >/dev/null 2>&1; then
            pass "target repo exists: $TARGET"
            # Emptiness from the refs, not the commits endpoint: an empty repo answers
            # that one with HTTP 409 AND prints the error body to stdout, which read as
            # "has commits". Zero refs is what the phase-5 push actually depends on.
            local refs
            if ! refs="$(git ls-remote "https://github.com/$TARGET.git" 2>/dev/null)"; then
                probe_fail 2 "cannot list the refs of $TARGET — is it reachable with these credentials?"
            elif [ -z "$refs" ]; then
                pass "target repo has zero commits (no refs)"
            elif [ "$DRY_RUN" -eq 1 ]; then
                warn "target repo is NOT empty (ignored: --dry-run)"
            else
                die 3 "target repo $TARGET already has commits — seed refuses a non-empty target"
            fi
        else
            probe_fail 2 "target repo $TARGET does not exist — create it first (see --help)"
        fi
    else
        warn "target emptiness not checked (gh unavailable or unauthenticated)"
    fi

    if [ -e "$WORK_DIR" ]; then
        probe_fail 2 "work-dir exists: $WORK_DIR — filter-repo demands a fresh clone dir; pick a new one (never --force)"
    else
        pass "work-dir is absent: $WORK_DIR"
    fi

    [ -f "$MANIFEST_SRC" ] || die 2 "manifest not found: $MANIFEST_SRC"
    pass "manifest present: $MANIFEST_SRC"
}

# -------------------------------------------------------------- phase 2 manifest
# Non-comment, non-blank lines of the manifest = the filter-repo pathspecs.
manifest_pathspecs() {
    grep -vE '^[[:space:]]*(#|$)' "$MANIFEST_SRC"
}

# Files of the scaffold image of the validated commit, image-relative, one per line.
scaffold_image_files() {
    ali_image_files "$SOURCE_ROOT" "$SOURCE_SHA"
}

# The publication audit is pinned to the develop commit it swept, recorded in
# seed-audit.md as **Audited through:** `<sha>`. Print that sha (empty if absent).
audit_pin() {
    # shellcheck disable=SC2016  # the backticks are literal Markdown, not a substitution
    sed -n 's/^\*\*Audited through:\*\* `\([0-9a-f]\{7,40\}\)`.*$/\1/p' "$AUDIT_SRC"
}

# Commits in <repo> after the audit pin that touch a manifest path, one per line.
# seed-audit.md itself is excluded: it is the audit record, and every re-audit
# edits it, so counting it would make the audit stale by construction.
audit_delta() {
    local repo="$1" pin="$2"
    local -a specs
    mapfile -t specs < <(manifest_pathspecs)
    git -C "$repo" log --oneline "$pin..HEAD" -- "${specs[@]}" \
        ':(exclude)agents/scripts/core/seed-agent-layer-repo.d/docs/seed-audit.md'
}

audit_stale_help() {
    fail "  Re-audit the delta, then move the pin:"
    fail "    python3 agents/scripts/core/seed-audit-sweep.py --since $1"
    fail "  Triage every new value, update the affected verdicts in seed-audit.md, and set"
    fail "  **Audited through:** to the develop commit you swept."
}

# Wrappers under $2.. that really RUN tests/bats/$1. Mirrors test-orphan-bats.sh's
# _is_wrapped: a bare-basename or comment-only mention must not count.
wrappers_for_suite() {
    local base="$1"; shift
    local root cand
    for root in "$@"; do
        [ -d "$root" ] || continue
        while IFS= read -r cand; do
            [ -n "$cand" ] || continue
            if grep -F "tests/bats/$base" "$cand" | grep -qvE '^[[:space:]]*#'; then
                printf '%s\n' "$cand"
            fi
        done < <(grep -rlF "tests/bats/$base" "$root" --include='test-*.sh' 2>/dev/null)
    done
}

# Every tests/bats/*.bats that a LAYER-side wrapper runs, sorted and unique. This is
# the generated bats block of seed-paths.txt. A wrapper names its suite by path on
# a non-comment line (BATS_FILE="tests/bats/<n>.bats", or a BATS_FILES=(...) array).
# The leading-context class [start "'=( space] keeps a scratch-dir path such as
# "$tmp/tests/bats/synth.bats" — a suite a selftest writes and deletes — from counting.
# Must run from the repo root.
layer_named_suites() {
    local root
    for root in "${LAYER_WRAPPER_ROOTS[@]}"; do
        [ -d "$root" ] || continue
        grep -rhE --include='test-*.sh' 'tests/bats/' "$root" 2>/dev/null \
            | grep -vE '^[[:space:]]*#' \
            | grep -oE "(^|[\"' =(])tests/bats/[A-Za-z0-9_.-]+\\.bats" \
            | sed -E "s#^[\"' =(]##"
    done | sort -u
}

phase2_manifest() {
    head1 "phase 2 — manifest"
    local p0="$PASSED"

    local repo_root
    repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
        || die 2 "not inside a git repo — run from the Smatchet working tree"
    cd "$repo_root" || die 2 "cannot cd to repo root: $repo_root"
    pass "source tree: $repo_root"

    # --- pin the validated revision (phase 3 binds the clone to it) ------------
    # Everything below reads the WORKING TREE, so uncommitted edits under a
    # manifest path (or tests/bats/, which feeds the regenerated block) would
    # validate a tree that no commit — and therefore no clone — contains.
    SOURCE_SHA="$(git rev-parse HEAD)" || die 2 "cannot resolve HEAD in $repo_root"
    SOURCE_ROOT="$repo_root"
    local -a pin_specs
    mapfile -t pin_specs < <(manifest_pathspecs)
    local dirty
    dirty="$(git status --porcelain --untracked-files=all -- "${pin_specs[@]}" tests/bats/)"
    if [ -z "$dirty" ]; then
        pass "validating committed revision $SOURCE_SHA (manifest paths clean)"
    elif [ "$DRY_RUN" -eq 1 ] && [ "$SIMULATE" -eq 0 ]; then
        warn "uncommitted changes under manifest paths — this dry run validates the working tree, not $SOURCE_SHA"
    else
        # --simulate reads this working tree's manifest here but images $SOURCE_SHA,
        # so a dirty tree would let a green run vouch for a tree it never tested.
        printf '%s\n' "$dirty" | sed 's/^/          /' >&2
        die 1 "uncommitted changes under manifest paths — commit or discard them; the seed publishes, and --simulate tests, a commit, not a working tree"
    fi

    # --- regenerate the bats block and diff it against the committed manifest ---
    local regen committed
    regen="$(mktemp)" || die 2 "mktemp failed"
    committed="$(mktemp)" || die 2 "mktemp failed"
    # shellcheck disable=SC2064
    trap "rm -f '$regen' '$committed'" RETURN

    layer_named_suites > "$regen"
    grep '^tests/bats/' "$MANIFEST_SRC" | sort > "$committed"

    local regen_count
    regen_count="$(wc -l < "$regen")"
    regen_count="${regen_count//[[:space:]]/}"

    if [ "$regen_count" -eq "$EXPECTED_BATS_COUNT" ]; then
        pass "layer-run bats count = $regen_count (tripwire $EXPECTED_BATS_COUNT)"
    else
        fail "layer-run bats count drifted: $regen_count vs tripwire $EXPECTED_BATS_COUNT."
        fail "  Do NOT just bump the constant. A suite entered or left the layer: decide"
        fail "  whether its wrapper belongs in agents/scripts/** (travels) or scripts/dev/"
        fail "  (stays), re-audit it for publication, refresh seed-paths.txt, then set"
        fail "  EXPECTED_BATS_COUNT deliberately."
    fi

    if diff -u "$committed" "$regen" >/dev/null 2>&1; then
        pass "committed manifest bats block matches the regenerated list"
    else
        fail "seed-paths.txt bats block is stale — regenerate it:"
        fail "  bash agents/scripts/core/seed-agent-layer-repo.sh --print-bats-block"
        diff -u "$committed" "$regen" >&2 || true
    fi

    # --- 8e, both directions: a suite travels with the wrapper that runs it -----
    # Layer -> suite: every suite a layer wrapper names must exist, or that wrapper
    # is an orphan that reds layer CI. Suite -> host: no host wrapper may also run a
    # layer suite, or it points at a deleted file once the flip removes the suite.
    local orphaned=0 split=0 f base host_hits
    while IFS= read -r f; do
        if [ ! -f "$f" ]; then
            fail "8e: a layer wrapper runs $f, which does not exist in the source tree"
            orphaned=$((orphaned + 1))
            continue
        fi
        base="$(basename "$f")"
        host_hits="$(wrappers_for_suite "$base" "${HOST_WRAPPER_ROOTS[@]}" | sort -u)"
        if [ -n "$host_hits" ]; then
            fail "8e: $f moves to the layer but a host wrapper also runs it:"
            printf '%s\n' "$host_hits" | sed 's/^/          /' >&2
            fail "  One side must own the suite. Keep exactly one wrapper, in agents/scripts/**"
            fail "  if the suite tests layer content, in scripts/dev/ if it tests the product."
            split=$((split + 1))
        fi
    done < "$regen"
    if [ "$orphaned" -eq 0 ] && [ "$split" -eq 0 ]; then
        pass "8e suite/wrapper co-location holds both ways for all $regen_count layer suites"
    fi

    # Advisory: a host-run suite that names layer paths still works after the flip
    # only if it reaches them through $AGENT_LAYER_ROOT (Phase C row 15/16 input).
    local host_coupled
    host_coupled="$(grep -l 'agents/' tests/bats/*.bats 2>/dev/null | sort | comm -23 - "$regen" | wc -l)"
    host_coupled="${host_coupled//[[:space:]]/}"
    if [ "$host_coupled" -gt 0 ]; then
        warn "$host_coupled host-run suite(s) name agents/ paths — they must resolve them via \$AGENT_LAYER_ROOT after the flip"
    fi

    # --- nothing host-only may ride along -------------------------------------
    local spec p bad=0
    while IFS= read -r spec; do
        for p in "${FORBIDDEN_PREFIXES[@]}"; do
            case "$spec" in
                "$p"*) fail "forbidden path in manifest: $spec (matches $p)"; bad=1 ;;
            esac
        done
    done < <(manifest_pathspecs)
    [ "$bad" -eq 0 ] && pass "no forbidden (host-only) path in the manifest"

    # --- every pathspec must actually exist in the source tree -----------------
    # filter-repo only WARNS on a pathspec that matches nothing, so a typo would
    # silently drop a whole subtree from the layer.
    local missing=0
    while IFS= read -r spec; do
        [ -e "$spec" ] || { fail "manifest path does not exist in the source tree: $spec"; missing=1; }
    done < <(manifest_pathspecs)
    [ "$missing" -eq 0 ] && pass "every manifest pathspec exists in the source tree"

    # --- the AGENTS.md prefix claim, asserted rather than assumed --------------
    # filter-repo matches a PATH PREFIX from the repo root, so `AGENTS.md` should
    # take the root rulebook only. Prove the host tree really has leaf AGENTS.md
    # files that the prefix leaves behind (phase 4b re-asserts it post-rewrite).
    local leaves
    leaves="$(git ls-files '*AGENTS.md' | grep -cv '^AGENTS.md$')"
    if [ "$leaves" -gt 0 ]; then
        pass "prefix claim is testable: $leaves non-root AGENTS.md leaf doc(s) must stay host-side"
    else
        warn "no non-root AGENTS.md in the tree — the prefix claim cannot be proven by this seed"
    fi

    # --- audit freshness, advisory here (phase 3 enforces it on the real clone) ---
    local pin delta_n
    pin="$(audit_pin)"
    if [ -z "$pin" ]; then
        warn "seed-audit.md has no **Audited through:** pin — phase 3 will refuse"
    elif ! git merge-base --is-ancestor "$pin" HEAD 2>/dev/null; then
        warn "audit pin $pin is not an ancestor of this tree's HEAD — freshness not checked here"
    else
        delta_n="$(audit_delta . "$pin" | wc -l)"
        delta_n="${delta_n//[[:space:]]/}"
        if [ "$delta_n" -eq 0 ]; then
            pass "publication audit is current (pinned at $pin)"
        else
            warn "$delta_n commit(s) touched manifest paths after the audit pin $pin — phase 3 will refuse until re-audited"
        fi
    fi

    local nspec
    nspec="$(manifest_pathspecs | wc -l)"
    nspec="${nspec//[[:space:]]/}"
    head1 "manifest — $nspec pathspecs"
    manifest_pathspecs

    if [ "$FAILED" -ne 0 ]; then
        die 1 "manifest assertions failed — nothing was cloned, rewritten or pushed"
    fi
    # Source tree, bats tripwire, block match, 8e, forbidden paths, pathspecs exist.
    require_floor "$p0" 6 "phase 2"

    if [ "$SIMULATE" -eq 1 ]; then
        head1 "--simulate: layer CI lanes on an image of $SOURCE_SHA"
        [ -f "$SIM_SCRIPT" ] || die 2 "simulator missing: $SIM_SCRIPT"
        bash "$SIM_SCRIPT" --rev "$SOURCE_SHA"
        exit $?
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        head1 "--dry-run: stopping after phase 2"
        say "Manifest reviewed clean. Re-run without --dry-run once the human"
        say "preconditions in --help are met."
        exit 0
    fi
}

# --------------------------------------------------------------- phase 3 rewrite
phase3_rewrite() {
    head1 "phase 3 — clone + rewrite"

    say "clone -> $WORK_DIR"
    git clone --no-local --no-tags --single-branch --branch "$LAYER_BRANCH" \
        "$SOURCE_URL" "$WORK_DIR" \
        || die 1 "clone failed"
    pass "fresh clone of $LAYER_BRANCH (--no-local --no-tags --single-branch)"

    local clone_sha
    clone_sha="$(git -C "$WORK_DIR" rev-parse HEAD)" || die 1 "cannot resolve the clone's HEAD"
    if [ "$clone_sha" != "$SOURCE_SHA" ]; then
        fail "remote $LAYER_BRANCH is at $clone_sha, but phase 2 validated $SOURCE_SHA."
        fail "  Run the seed from a checkout of the exact origin/$LAYER_BRANCH tip (git pull --ff-only),"
        fail "  so the tree that gets published is the tree that was checked."
        die 1 "clone does not match the validated revision — nothing rewritten or pushed"
    fi
    pass "clone matches the validated revision $SOURCE_SHA"

    # Audit freshness, enforced on exactly what will be rewritten and published.
    # Read-only git in the clone, so filter-repo still sees a fresh clone.
    local pin delta
    pin="$(audit_pin)"
    [ -n "$pin" ] || die 1 "seed-audit.md has no **Audited through:** \`<sha>\` pin — the audit cannot be proven current"
    git -C "$WORK_DIR" merge-base --is-ancestor "$pin" HEAD \
        || die 1 "audit pin $pin is not an ancestor of the cloned $LAYER_BRANCH — re-audit against a develop commit"
    delta="$(audit_delta "$WORK_DIR" "$pin")"
    if [ -n "$delta" ]; then
        fail "commits touched manifest paths after the publication audit (pin $pin):"
        printf '%s\n' "$delta" | sed 's/^/          /' >&2
        audit_stale_help "$pin"
        die 1 "publication audit is stale — nothing rewritten or pushed"
    fi
    pass "publication audit is current for the cloned $LAYER_BRANCH (pin $pin)"

    cp "$MANIFEST_SRC" "$WORK_DIR/seed-paths.txt" || die 1 "cannot stage the manifest"

    cd "$WORK_DIR" || die 1 "cannot cd to $WORK_DIR"

    # --paths-from-file, never ~75 --path argv entries: the same argv-length
    # discipline that forced the GraphQL-document-by-file fix on Windows
    # (52500a9bd), and the file is the artefact reviewers read.
    # --replace-refs delete-no-add keeps refs/replace/* out of the seeded repo;
    # --prune-empty auto is the default, stated so a later edit cannot flip it.
    git filter-repo --paths-from-file seed-paths.txt \
        --replace-refs delete-no-add --prune-empty auto \
        || die 1 "git filter-repo failed"
    pass "history rewritten to the allowlist"

    # Paired scrub pass for paths INSIDE an allowed subtree that failed the
    # publication audit. Empty today: the one known offender,
    # historical-review-sweep.js, lives under agents/project/workflows/ which the
    # allowlist already excludes wholesale. Kept wired so a future audit verdict
    # needs a manifest line, not a code change.
    local scrub="$SCAFFOLD_DIR/seed-scrub-paths.txt"
    if [ -f "$scrub" ] && grep -qvE '^[[:space:]]*(#|$)' "$scrub"; then
        cp "$scrub" "$WORK_DIR/seed-scrub-paths.txt" || die 1 "cannot stage the scrub list"
        git filter-repo --paths-from-file seed-scrub-paths.txt --invert-paths \
            --replace-refs delete-no-add --prune-empty auto \
            || die 1 "scrub pass failed"
        rm -f "$WORK_DIR/seed-scrub-paths.txt"
        pass "scrub pass applied (audit-failed paths removed from history)"
    else
        pass "no scrub pass needed (seed-scrub-paths.txt empty or absent)"
    fi

    rm -f "$WORK_DIR/seed-paths.txt"

    local branch
    branch="$(git rev-parse --abbrev-ref HEAD)"
    [ "$branch" = "$LAYER_BRANCH" ] \
        || die 1 "branch is '$branch', expected '$LAYER_BRANCH' — ~60 moved gates diff against the literal origin/develop"
    pass "default branch is still '$LAYER_BRANCH'"
}

# -------------------------------------------------------------- phase 4 scaffold
phase4_scaffold() {
    head1 "phase 4 — scaffold"

    # The image comes from the validated commit, never the work tree: an untracked
    # or ignored file under the scaffold dir (a stray log, an editor backup) is not
    # in it, so it can neither be published nor exempted by phase 4b.
    local image
    image="$(scaffold_image_files)" || die 1 "no scaffold image at $SOURCE_SHA"
    local missing=0 asset
    for asset in "${SCAFFOLD_ROW10[@]}" "${SCAFFOLD_ROW9[@]}" docs/seed-paths.txt docs/seed-audit.md; do
        printf '%s\n' "$image" | grep -qxF -- "$asset" \
            || { fail "required scaffold file missing at $SOURCE_SHA: $asset"; missing=1; }
    done
    if [ "$missing" -ne 0 ]; then
        fail "The scaffold image must carry the layer's CI (row 9), its root files and"
        fail "consumption contract (row 10), the manifest and the audit table. Pushing a"
        fail "public repo with no gates is worse than not pushing it."
        die 1 "scaffold incomplete"
    fi

    # <image>/X lands at <layer>/X — one copy rule, shared with agent-layer-sim.sh.
    ali_extract_image "$SOURCE_ROOT" "$SOURCE_SHA" "$WORK_DIR" \
        || die 1 "could not write the scaffold image of $SOURCE_SHA into $WORK_DIR"
    local f
    while IFS= read -r f; do
        [ -f "$WORK_DIR/$f" ] || die 1 "scaffold file not written: $f"
        pass "wrote $f"
    done <<< "$image"

    cd "$WORK_DIR" || die 1 "cannot cd to $WORK_DIR"
    git add -A || die 1 "git add failed"
    git commit -m "chore(seed): layer CI + consumption contract" \
        || die 1 "seed scaffold commit failed"
    pass "committed: chore(seed): layer CI + consumption contract"
}

# ---------------------------------------------------------------- phase 4b audit
phase4b_audit() {
    head1 "phase 4b — publication audit (hard gate on the phase 5 push)"
    local p0="$PASSED"

    cd "$WORK_DIR" || die 1 "cannot cd to $WORK_DIR"

    # 1. history-wide secret scan of the REWRITTEN history. A scrubbed head over
    #    a leaky history still publishes the leak.
    local th_out th_rc
    case "$SCANNER" in
        gitleaks)
            if gitleaks detect --redact --log-opts=--all --exit-code 1; then
                pass "gitleaks: no secrets in the rewritten history"
            else
                fail "gitleaks found secrets in the rewritten history"
                fail "  Scrub via filter-repo --replace-text / --invert-paths and re-run from"
                fail "  phase 3, or revoke the credential and record the revocation."
                die 1 "secret scan failed — nothing pushed"
            fi
            ;;
        trufflehog)
            th_out="$(trufflehog git "file://$WORK_DIR" --results=verified --fail 2>&1)"
            th_rc=$?
            if [ "$th_rc" -eq 0 ]; then
                pass "trufflehog: no verified secrets in the rewritten history"
            else
                printf '%s\n' "$th_out" >&2
                die 1 "trufflehog found verified secrets — nothing pushed"
            fi
            ;;
        *)  die 1 "no secret scanner resolved — phase 4b cannot be skipped" ;;
    esac

    # 2. the rewritten file set must be a SUBSET of the manifest. Anything the
    #    allowlist did not name aborts before the push.
    local tracked image outside=0 f ok spec
    tracked="$(mktemp)" || die 1 "mktemp failed"
    image="$(mktemp)" || die 1 "mktemp failed"
    # shellcheck disable=SC2064
    trap "rm -f '$tracked' '$image'" RETURN
    git ls-files > "$tracked"
    scaffold_image_files > "$image"
    while IFS= read -r f; do
        # Files phase 4 copied from the scaffold image are seeded by construction,
        # not manifest rows — and the image is itself under a manifest path, so it
        # was audited with the rest.
        grep -qxF -- "$f" "$image" && continue
        ok=0
        while IFS= read -r spec; do
            case "$f" in "$spec"*) ok=1; break ;; esac
        done < <(manifest_pathspecs)
        [ "$ok" -eq 1 ] || { fail "seeded path outside the manifest: $f"; outside=1; }
    done < "$tracked"
    [ "$outside" -eq 0 ] || die 1 "manifest subset assertion failed — nothing pushed"
    pass "every seeded path is covered by the manifest"

    # 3. the AGENTS.md prefix claim, now asserted on the real rewrite.
    local agents_md
    agents_md="$(git ls-files '*AGENTS.md' | tr '\n' ' ')"
    agents_md="${agents_md% }"
    if [ "$agents_md" = "AGENTS.md" ]; then
        pass "git ls-files '*AGENTS.md' == AGENTS.md (leaf docs stayed host-side)"
    else
        fail "AGENTS.md prefix match took more than the root rulebook: $agents_md"
        die 1 "prefix assertion failed — nothing pushed"
    fi

    # 4. every manifest path has EXACTLY ONE manifest-row verdict, and it is one
    #    this script can honour. Rejecting only PENDING was too weak: a SCRUB row
    #    nobody applied, an EXCLUDE row for a path still being seeded, a typo, or
    #    duplicate rows would all have unlocked the push.
    #      CLEAR   publishes as-is
    #      SCRUB   must be listed in seed-scrub-paths.txt AND gone from the rewrite
    #      EXCLUDE contradicts the path being in the manifest -> refuse
    #      PENDING not audited -> refuse; anything else is malformed -> refuse
    local bad_verdict=0 cells nrows verdict
    local scrub_list="$SCAFFOLD_DIR/seed-scrub-paths.txt"
    while IFS= read -r spec; do
        # Manifest rows only: first cell numeric, second cell the backticked path.
        cells="$(awk -F'|' -v want="\`$spec\`" '
            {
                n = $2; p = $3; v = $4
                gsub(/^[ \t]+|[ \t]+$/, "", n); gsub(/^[ \t]+|[ \t]+$/, "", p)
                gsub(/^[ \t]+|[ \t]+$/, "", v)
                if (n ~ /^[0-9]+$/ && p == want) print v
            }' docs/seed-audit.md)"
        nrows="$(printf '%s' "$cells" | grep -c .)"
        if [ "$nrows" -ne 1 ]; then
            fail "docs/seed-audit.md has $nrows manifest row(s) for $spec — need exactly 1"
            bad_verdict=1
            continue
        fi
        verdict="${cells//\`/}"
        case "$verdict" in
            CLEAR) ;;
            SCRUB)
                if [ ! -f "$scrub_list" ] || ! grep -qxF "$spec" "$scrub_list"; then
                    fail "$spec is SCRUB in the audit but not listed in seed-scrub-paths.txt"
                    bad_verdict=1
                elif [ -n "$(git log --all --oneline -- "$spec")" ]; then
                    fail "$spec is SCRUB in the audit but still has history in the rewrite"
                    bad_verdict=1
                fi
                ;;
            EXCLUDE)
                fail "$spec is EXCLUDE in the audit but still in seed-paths.txt — remove it from the manifest"
                bad_verdict=1
                ;;
            PENDING)
                fail "publication verdict still PENDING for: $spec"
                bad_verdict=1
                ;;
            *)
                fail "malformed verdict '$verdict' for $spec (expected CLEAR, SCRUB, EXCLUDE or PENDING)"
                bad_verdict=1
                ;;
        esac
    done < <(manifest_pathspecs)
    if [ "$bad_verdict" -ne 0 ]; then
        fail "Every manifest path needs exactly one CLEAR or enforced SCRUB verdict in"
        fail "docs/seed-audit.md, from a read of the path AND its history. Publishing is one-way."
        die 1 "publication audit incomplete — nothing pushed"
    fi
    pass "every manifest path has exactly one CLEAR or enforced SCRUB verdict"

    # Secret scan, manifest subset, AGENTS.md prefix, verdicts. The push is
    # irreversible, so a vacuous pass here must not unlock it.
    require_floor "$p0" 4 "phase 4b"
    PUBLISH_CLEARED=1
    pass "publication audit CLEAR — phase 5 unlocked"
}

# ----------------------------------------------------------------- phase 4c lanes
# The layer's own CI lanes, on the exact tree phase 5 is about to push. Phase 2
# proves the manifest's shape; only running the gates proves it is complete — an
# unseeded fixture or a host-only subject passes every shape check and reds the
# first layer PR.
phase4c_lanes() {
    head1 "phase 4c — layer CI lanes on the rewritten clone (hard gate on the phase 5 push)"

    [ -f "$SIM_SCRIPT" ] || die 1 "simulator missing: $SIM_SCRIPT"
    if bash "$SIM_SCRIPT" --run-only "$WORK_DIR"; then
        LANES_CLEARED=1
        pass "the layer's CI lanes are green on the rewritten clone — phase 5 unlocked"
    else
        die 1 "the layer's CI lanes are red on the rewritten clone — nothing pushed"
    fi
}

# --------------------------------------------------------------- phase 5 publish
phase5_publish() {
    head1 "phase 5 — publish (IRREVERSIBLE)"

    [ "$PUBLISH_CLEARED" -eq 1 ] \
        || die 1 "refusing to push: phase 4b did not clear. A public history cannot be un-published."
    [ "$LANES_CLEARED" -eq 1 ] \
        || die 1 "refusing to push: phase 4c did not clear. A public repo that reds its own gates is not a seed."

    cd "$WORK_DIR" || die 1 "cannot cd to $WORK_DIR"

    # filter-repo removes `origin` by design; re-point it.
    git remote remove origin >/dev/null 2>&1 || true
    git remote add origin "https://github.com/$TARGET.git" || die 1 "git remote add failed"
    pass "origin -> https://github.com/$TARGET.git"

    git push -u origin "$LAYER_BRANCH" || die 1 "push failed"
    pass "pushed $LAYER_BRANCH to $TARGET"

    # Labels plus branch protection come from the LAYER's own project.config.json.
    local layer_cfg="$WORK_DIR/project.config.json"
    local label
    if [ -f "$layer_cfg" ] && [ -f "$WORK_DIR/scripts/dev/project-config.sh" ]; then
        # shellcheck disable=SC1091
        PC_CONFIG_FILE="$layer_cfg" . "$WORK_DIR/scripts/dev/project-config.sh" || true
        # shellcheck disable=SC2086
        for label in ${PC_OVERRIDE_LABELS:-}; do
            if gh label create "$label" --repo "$TARGET" --force >/dev/null 2>&1; then
                pass "label: $label"
            else
                warn "label create failed (may already exist): $label"
            fi
        done
    else
        warn "layer project.config.json not resolvable — labels not created"
    fi

    # Branch protection is part of the seed contract, so failing to apply it is a
    # failed seed, not a warning: the push has already happened, and "Seed complete"
    # over an unprotected branch would be a lie. setup-branch-protection.sh takes
    # its target from $REPO and its config from $SMATCHET_BP_CONFIG — it has no
    # --repo flag and does not read PC_CONFIG_FILE, so both are passed explicitly.
    local bp="$WORK_DIR/agents/scripts/core/setup-branch-protection.sh"
    if [ ! -f "$bp" ]; then
        fail "setup-branch-protection.sh is missing from the seed — $TARGET/$LAYER_BRANCH is pushed but UNPROTECTED"
    elif REPO="$TARGET" SMATCHET_BP_CONFIG="$layer_cfg" bash "$bp"; then
        pass "branch protection applied to $TARGET/$LAYER_BRANCH from the layer's required_contexts"
    else
        fail "branch protection NOT applied — $TARGET/$LAYER_BRANCH is pushed but UNPROTECTED."
        fail "  Fix the cause, then from the seeded clone:"
        fail "    REPO=$TARGET bash agents/scripts/core/setup-branch-protection.sh"
    fi
}

# ---------------------------------------------------------------- phase 6 report
phase6_report() {
    head1 "phase 6 — row-8 Accept checks"
    local p0="$PASSED"

    cd "$WORK_DIR" || die 1 "cannot cd to $WORK_DIR"

    local n sha
    n="$(git ls-files | grep -cE '^(Source/|docs/plans/|docs/self-improvement/categories/|CMakeLists.txt)')"
    if [ "$n" -eq 0 ]; then
        pass "no host-only tree in the seed (count 0)"
    else
        fail "host-only paths leaked into the seed: $n"
    fi

    n="$(git ls-files 'tests/bats/*' | wc -l)"
    n="${n//[[:space:]]/}"
    if [ "$n" -eq "$EXPECTED_BATS_COUNT" ]; then
        pass "tests/bats count = $n"
    else
        fail "tests/bats count = $n, expected $EXPECTED_BATS_COUNT"
    fi

    n="$(git log --follow --oneline -- agents/core/code-review.md | wc -l)"
    n="${n//[[:space:]]/}"
    if [ "$n" -gt 1 ]; then
        pass "history survived (agents/core/code-review.md has $n commits)"
    else
        fail "history did NOT survive — agents/core/code-review.md has $n commit(s)"
    fi

    git count-objects -v | sed 's/^/  /'

    sha="$(git rev-parse HEAD)"
    head1 "seed SHA: $sha"

    if [ "$FAILED" -ne 0 ]; then
        die 1 "seed NOT complete — one or more checks FAILED above (publish or row-8 Accept)"
    fi
    require_floor "$p0" 3 "phase 6"
    say "Seed complete: https://github.com/$TARGET"
}

main() {
    parse_args "$@"
    phase1_preflight
    phase2_manifest
    phase3_rewrite
    phase4_scaffold
    phase4b_audit
    phase4c_lanes
    phase5_publish
    phase6_report
}

main "$@"
