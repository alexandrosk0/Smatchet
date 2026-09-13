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
MANIFEST_SRC="$SCAFFOLD_DIR/docs/seed-paths.txt"
AUDIT_SRC="$SCAFFOLD_DIR/docs/seed-audit.md"

# The layer's default branch stays `develop`, NOT `main`. ~60 of the moved gates
# compute a merge-base delta against the literal `origin/develop`
# (dup_audit.py --diff, agent_size_audit.py --diff, test-lint-rules.sh --diff
# origin/develop, test-markdown-links.sh, is-pure-docs-diff.sh's base_ref
# default). Renaming would turn every one of them into a no-op or a hard error.
LAYER_BRANCH="develop"
SOURCE_URL="https://github.com/alexandrosk0/Smatchet.git"

# Tripwire, not a measurement. `grep -l 'agents/' tests/bats/*.bats` was 61 when
# the plan was written (2026-08-29) and is 62 today; the tree keeps drifting. A
# mismatch is NOT "bump the number" — it means a suite entered or left the
# layer-coupled set and its wrapper co-location (8e) needs re-auditing before the
# seed, which is exactly the hole this constant exists to keep shut.
EXPECTED_BATS_COUNT=62

# Wrapper roots that TRAVEL with the layer. A layer-coupled suite whose only
# wrapper lives outside these (i.e. in scripts/dev/) reds BOTH repos after the
# flip: orphan suite layer-side, wrapper pointing at a deleted file host-side.
LAYER_WRAPPER_ROOTS=(agents/scripts/core agents/scripts/project)
ALL_WRAPPER_ROOTS=(scripts/dev agents/scripts/core agents/scripts/project)

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

# Root files phase 4 writes into the seeded repo, sourced from SCAFFOLD_DIR.
# `row10` assets are authored; `row9` assets are NOT yet — row 9 (layer CI) is
# blocked on the repo existing, so phase 4 names them and stops rather than
# pushing a repo with no gates.
SCAFFOLD_ROW10=(README.md LICENSE project.config.json)
SCAFFOLD_ROW9=(
    .coderabbit.yaml
    .github/workflows/agentic-selftests.yml
    .github/workflows/shell-lint.yml
    .github/workflows/doc-validation.yml
)

DRY_RUN=0
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

usage() {
    cat <<'USAGE'
seed-agent-layer-repo.sh — seed the public agent-layer repo from a Smatchet clone.

  seed-agent-layer-repo.sh --target <owner/repo> [--work-dir DIR] [--dry-run] [--help]

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
  --help                  This text.

Exit: 0 seeded (or dry-run review passed) | 1 assertion failed
      2 usage / tooling missing           | 3 target not empty

Phases:
  1 preflight   tooling on PATH, gh auth, secret scanner, target exists and is
                empty, work-dir absent
  2 manifest    regenerate seed-paths.txt; assert the bats-count tripwire, the 8e
                suite/wrapper co-location invariant, and that no forbidden path
                slipped in; print the manifest.  --dry-run STOPS HERE.
  3 rewrite     fresh clone (--no-local --no-tags --single-branch --branch develop)
                plus git filter-repo --paths-from-file, then the paired
                --invert-paths scrub pass if seed-scrub-paths.txt is non-empty
  4 scaffold    write the row 9 CI files, row 10 root files, .coderabbit.yaml,
                docs/seed-paths.txt and docs/seed-audit.md; commit
  4b audit      history-wide secret scan of the REWRITTEN history, plus assert the
                rewritten `git ls-files` is a SUBSET of the manifest and that every
                seeded path carries a verdict in docs/seed-audit.md. Any hit is a
                hard stop; nothing is pushed until this is clean.
  5 publish     hard-refuses unless 4b cleared; remote add plus push develop;
                gh label create; setup-branch-protection.sh
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
            --target)       [ "$#" -ge 2 ] || die 2 "--target needs a value"
                            TARGET="$2"; shift 2 ;;
            --target=*)     TARGET="${1#--target=}"; shift ;;
            --work-dir)     [ "$#" -ge 2 ] || die 2 "--work-dir needs a value"
                            WORK_DIR="$2"; shift 2 ;;
            --work-dir=*)   WORK_DIR="${1#--work-dir=}"; shift ;;
            *)              usage >&2; die 2 "unknown argument: $1" ;;
        esac
    done
    [ -n "$TARGET" ] || { usage >&2; die 2 "--target <owner/repo> is required"; }
    case "$TARGET" in
        */*/*|/*|*/) die 2 "--target must be exactly owner/repo, got: $TARGET" ;;
        */*)         : ;;
        *)           die 2 "--target must be owner/repo, got: $TARGET" ;;
    esac
    [ -n "$WORK_DIR" ] || WORK_DIR="${TMPDIR:-/tmp}/agent-layer-seed.$$"
}

# ------------------------------------------------------------- phase 1 preflight
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
        if gh auth status >/dev/null 2>&1; then
            pass "gh authenticated"
        else
            probe_fail 2 "gh not authenticated — run: gh auth login"
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
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        if gh repo view "$TARGET" >/dev/null 2>&1; then
            pass "target repo exists: $TARGET"
            local commits
            commits="$(gh api "repos/$TARGET/commits?per_page=1" --jq 'length' 2>/dev/null)"
            # An empty repo answers 409 "Git Repository is empty" -> empty $commits.
            if [ -z "$commits" ] || [ "$commits" = "0" ]; then
                pass "target repo has zero commits"
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

phase2_manifest() {
    head1 "phase 2 — manifest"
    local p0="$PASSED"

    local repo_root
    repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
        || die 2 "not inside a git repo — run from the Smatchet working tree"
    cd "$repo_root" || die 2 "cannot cd to repo root: $repo_root"
    pass "source tree: $repo_root"

    # --- regenerate the bats block and diff it against the committed manifest ---
    local regen committed
    regen="$(mktemp)" || die 2 "mktemp failed"
    committed="$(mktemp)" || die 2 "mktemp failed"
    # shellcheck disable=SC2064
    trap "rm -f '$regen' '$committed'" RETURN

    grep -l 'agents/' tests/bats/*.bats 2>/dev/null | sort > "$regen"
    grep '^tests/bats/' "$MANIFEST_SRC" | sort > "$committed"

    local regen_count
    regen_count="$(wc -l < "$regen")"
    regen_count="${regen_count//[[:space:]]/}"

    if [ "$regen_count" -eq "$EXPECTED_BATS_COUNT" ]; then
        pass "layer-coupled bats count = $regen_count (tripwire $EXPECTED_BATS_COUNT)"
    else
        fail "layer-coupled bats count drifted: $regen_count vs tripwire $EXPECTED_BATS_COUNT."
        fail "  Do NOT just bump the constant. A suite entered or left the layer-coupled"
        fail "  set: re-audit its wrapper co-location (row 8e), refresh seed-paths.txt,"
        fail "  then set EXPECTED_BATS_COUNT deliberately."
    fi

    if diff -u "$committed" "$regen" >/dev/null 2>&1; then
        pass "committed manifest bats block matches the regenerated list"
    else
        fail "seed-paths.txt bats block is stale — regenerate it:"
        fail "  grep -l 'agents/' tests/bats/*.bats | sort"
        diff -u "$committed" "$regen" >&2 || true
    fi

    # --- 8e: every moved suite's wrapper must travel with it -------------------
    local off_side=0 unwrapped=0 f base layer_hits all_hits
    while IFS= read -r f; do
        base="$(basename "$f")"
        all_hits="$(wrappers_for_suite "$base" "${ALL_WRAPPER_ROOTS[@]}" | sort -u)"
        layer_hits="$(wrappers_for_suite "$base" "${LAYER_WRAPPER_ROOTS[@]}" | sort -u)"
        if [ -z "$all_hits" ]; then
            fail "8e: $f has NO test-*.sh wrapper (test-orphan-bats.sh would red too)"
            unwrapped=$((unwrapped + 1))
            continue
        fi
        if [ -z "$layer_hits" ]; then
            fail "8e: $f moves to the layer but every wrapper naming it stays host-side:"
            printf '%s\n' "$all_hits" | sed 's/^/          /' >&2
            fail "  git mv the wrapper into agents/scripts/core/ (that is what A1w did for"
            fail "  android_openssl_failfast.bats and safe_merge.bats), or drop the suite"
            fail "  from the manifest. Left as-is this reds BOTH repos after the flip."
            off_side=$((off_side + 1))
        fi
    done < "$regen"
    if [ "$off_side" -eq 0 ] && [ "$unwrapped" -eq 0 ]; then
        pass "8e suite/wrapper co-location holds for all $regen_count moved suites"
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

    local missing=0 asset
    for asset in "${SCAFFOLD_ROW10[@]}"; do
        [ -f "$SCAFFOLD_DIR/$asset" ] \
            || { fail "row 10 scaffold asset missing: $SCAFFOLD_DIR/$asset"; missing=1; }
    done
    for asset in "${SCAFFOLD_ROW9[@]}"; do
        [ -f "$SCAFFOLD_DIR/$asset" ] \
            || { fail "row 9 scaffold asset missing: $SCAFFOLD_DIR/$asset"; missing=1; }
    done
    if [ "$missing" -ne 0 ]; then
        fail "Row 9 (layer CI: the three workflow lanes plus .coderabbit.yaml) is not authored yet."
        fail "It is blocked on the repo existing — branch protection needs the exact job names"
        fail "the lanes emit. Land row 9 into $SCAFFOLD_DIR before a real seed:"
        fail "pushing a public repo with no gates is worse than not pushing it."
        die 1 "scaffold incomplete"
    fi

    for asset in "${SCAFFOLD_ROW10[@]}" "${SCAFFOLD_ROW9[@]}"; do
        mkdir -p "$WORK_DIR/$(dirname "$asset")" || die 1 "mkdir failed for $asset"
        cp "$SCAFFOLD_DIR/$asset" "$WORK_DIR/$asset" || die 1 "cannot write $asset"
        pass "wrote $asset"
    done

    # The manifest is the audit trail; it ships in the seeded repo at the
    # location row 8c names. Both already sit under docs/ in the scaffold image.
    mkdir -p "$WORK_DIR/docs" || die 1 "mkdir docs failed"
    cp "$MANIFEST_SRC" "$WORK_DIR/docs/seed-paths.txt" || die 1 "cannot write docs/seed-paths.txt"
    pass "wrote docs/seed-paths.txt"

    [ -f "$AUDIT_SRC" ] \
        || die 1 "audit table missing: $AUDIT_SRC (the per-path publication verdict is a phase-4b precondition)"
    cp "$AUDIT_SRC" "$WORK_DIR/docs/seed-audit.md" || die 1 "cannot write docs/seed-audit.md"
    pass "wrote docs/seed-audit.md"

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
    local tracked outside=0 f ok spec
    tracked="$(mktemp)" || die 1 "mktemp failed"
    # shellcheck disable=SC2064
    trap "rm -f '$tracked'" RETURN
    git ls-files > "$tracked"
    while IFS= read -r f; do
        # Files phase 4 just added are seeded-by-construction, not manifest rows.
        case "$f" in
            README.md|LICENSE|project.config.json|.coderabbit.yaml) continue ;;
            .github/workflows/*|docs/seed-paths.txt|docs/seed-audit.md) continue ;;
        esac
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

    # 4. every seeded path carries a CLEARED verdict in docs/seed-audit.md.
    #    Presence alone is too weak: the generated table starts every row PENDING,
    #    so a presence-only check would wave through a completely un-audited seed.
    local unverdicted=0 row
    while IFS= read -r spec; do
        row="$(grep -F "| \`$spec\` |" docs/seed-audit.md)"
        if [ -z "$row" ]; then
            fail "no row in docs/seed-audit.md for: $spec"
            unverdicted=1
        elif printf '%s' "$row" | grep -q 'PENDING'; then
            fail "publication verdict still PENDING for: $spec"
            unverdicted=1
        fi
    done < <(manifest_pathspecs)
    if [ "$unverdicted" -ne 0 ]; then
        fail "Read each path AND its history (git log -p --follow <path>) and record"
        fail "CLEAR / SCRUB / EXCLUDE in docs/seed-audit.md. Publishing is one-way."
        die 1 "publication audit incomplete — nothing pushed"
    fi
    pass "every manifest path carries a non-PENDING verdict in docs/seed-audit.md"

    # Secret scan, manifest subset, AGENTS.md prefix, verdicts. The push is
    # irreversible, so a vacuous pass here must not unlock it.
    require_floor "$p0" 4 "phase 4b"
    PUBLISH_CLEARED=1
    pass "publication audit CLEAR — phase 5 unlocked"
}

# --------------------------------------------------------------- phase 5 publish
phase5_publish() {
    head1 "phase 5 — publish (IRREVERSIBLE)"

    [ "$PUBLISH_CLEARED" -eq 1 ] \
        || die 1 "refusing to push: phase 4b did not clear. A public history cannot be un-published."

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

    if [ -f "$WORK_DIR/agents/scripts/core/setup-branch-protection.sh" ]; then
        PC_CONFIG_FILE="$layer_cfg" \
            bash "$WORK_DIR/agents/scripts/core/setup-branch-protection.sh" --repo "$TARGET" \
            || warn "setup-branch-protection.sh returned non-zero — apply protection by hand"
        pass "branch protection attempted from the layer's required_contexts"
    else
        warn "setup-branch-protection.sh not present in the seed — apply protection by hand"
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
        die 1 "one or more row-8 Accept checks FAILED"
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
    phase5_publish
    phase6_report
}

main "$@"
