#!/usr/bin/env bash
# scripts/ci/detect-changes.sh — classify a diff for build-and-test.yml's `changes` job.
#
# Usage:
#   bash scripts/ci/detect-changes.sh <range>
#
# <range> is a git diff range, normally `<base>...HEAD` (three-dot: diff against the
# merge-base) or `<before>...HEAD` on a push. Run from inside the repository whose
# history the range names.
#
# Prints exactly three `key=value` lines on stdout, in this order, ready to append to
# "$GITHUB_OUTPUT":
#   code=<true|false>             any non-docs file changed
#   source_core_cpp=<true|false>  run the per-PR sanitizer lanes
#   android_only=<true|false>     every code file is android/mobile-only
# Everything else (the file list, the reasoning) goes to stderr, so the caller can
# redirect stdout straight into $GITHUB_OUTPUT.
#
# Always exits 0 after printing all three lines; exits 2 only on a usage error. A
# failure to compute the diff is not an error: it yields an empty file list, and each
# output takes its fail-safe value (code=true, source_core_cpp=true, android_only=false).
#
# The merge_group / golden-bootstrap forcing stays in the workflow: it depends on the
# GitHub event, which this script does not see.
#
# Tests: tests/bats/detect_changes.bats (scripts/dev/test-detect-changes-bats.sh).
set -uo pipefail

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: detect-changes.sh <range>" >&2
    exit 2
fi
range="$1"

files=$(git diff --name-only "$range" 2>/dev/null || true)
echo "Changed files in ${range}:" >&2
if [ -n "$files" ]; then
    printf '%s\n' "$files" | sed 's/^/  /' >&2
fi

# workflow_sanitizer_touched <path> — rc 0 when the diff of workflow <path> over $range
# touches the sanitizer lanes, rc 1 when it provably does not.
#
# "Touches" means either of:
#   * a changed (added or removed) line mentions "sanitizer", case-insensitively — an
#     `if:` gate, a `needs:` edge, a comment about the lanes; or
#   * a changed line lies inside a job whose id or `name:` mentions "sanitizer", so a
#     changed step in the lane (a ctest flag, a cache key) counts even when the line
#     itself does not say "sanitizer". Removed lines are located in the merge-base
#     copy of the file, added lines in the head copy.
# Fail-safe: any git failure, or an empty diff for a file the name list reported,
# returns 0 (run the lanes).
workflow_sanitizer_touched() {
    local path="$1" diff lhs rhs base_rev head_rev
    diff=$(git diff -U0 "$range" -- "$path" 2>/dev/null) || return 0
    [ -n "$diff" ] || return 0

    if printf '%s\n' "$diff" | grep -E '^[+-]' | grep -vE '^(\+\+\+|---) ' | grep -qi 'sanitizer'; then
        return 0
    fi

    case "$range" in
        *...*)
            lhs="${range%%...*}"
            rhs="${range#*...}"
            [ -n "$rhs" ] || rhs=HEAD
            base_rev=$(git merge-base "$lhs" "$rhs" 2>/dev/null) || return 0
            head_rev="$rhs"
            ;;
        *..*)
            base_rev="${range%%..*}"
            head_rev="${range#*..}"
            [ -n "$head_rev" ] || head_rev=HEAD
            ;;
        *)
            # A single revision diffs against the working tree; there is no head blob.
            return 0
            ;;
    esac

    # job_ranges — read a workflow on stdin, print "<first> <last>" (1-based, inclusive)
    # for every job under the top-level `jobs:` map whose id or `name:` says sanitizer.
    # shellcheck disable=SC2016  # an awk program; nothing here is meant to expand
    local job_ranges_awk='
        function flush(end_line) {
            if (start > 0 && san) print start, end_line
            start = 0; san = 0
        }
        /^jobs:[[:space:]]*(#.*)?$/ { in_jobs = 1; next }
        in_jobs && /^[^[:space:]#]/ { flush(NR - 1); in_jobs = 0; next }
        in_jobs && /^  [A-Za-z0-9_-]+:[[:space:]]*(#.*)?$/ {
            flush(NR - 1)
            start = NR
            san = (tolower($1) ~ /sanitizer/)
            next
        }
        in_jobs && start > 0 && /^    name:/ && tolower($0) ~ /sanitizer/ { san = 1 }
        END { flush(NR) }
    '
    # Ranges travel to the second awk as "a b;c d" — no newline inside an -v value.
    local old_ranges new_ranges
    old_ranges=$(git show "${base_rev}:${path}" 2>/dev/null | awk "$job_ranges_awk" | tr '\n' ';')
    new_ranges=$(git show "${head_rev}:${path}" 2>/dev/null | awk "$job_ranges_awk" | tr '\n' ';')

    # Walk the hunk headers (`@@ -a[,b] +c[,d] @@`) and test each changed line number
    # against the ranges of its own side. A count of 0 means that side has no changed
    # line in this hunk, so it is skipped.
    printf '%s\n' "$diff" | awk -v old_ranges="$old_ranges" -v new_ranges="$new_ranges" '
        function load(spec, lo, hi,    n, i, rows, f) {
            n = split(spec, rows, ";")
            for (i = 1; i <= n; i++) {
                if (split(rows[i], f, " ") == 2) { lo[++lo[0]] = f[1] + 0; hi[lo[0]] = f[2] + 0 }
            }
        }
        function inside(line, lo, hi,    i) {
            for (i = 1; i <= lo[0]; i++) if (line >= lo[i] && line <= hi[i]) return 1
            return 0
        }
        function side(spec, lo, hi,    start, count, parts, l) {
            split(spec, parts, ",")
            start = parts[1] + 0
            count = (2 in parts) ? parts[2] + 0 : 1
            for (l = start; l < start + count; l++) if (inside(l, lo, hi)) return 1
            return 0
        }
        BEGIN { olo[0] = 0; nlo[0] = 0; load(old_ranges, olo, ohi); load(new_ranges, nlo, nhi) }
        /^@@ / {
            o = $2; sub(/^-/, "", o)
            n = $3; sub(/^\+/, "", n)
            if (side(o, olo, ohi) || side(n, nlo, nhi)) { hit = 1; exit }
        }
        END { exit hit ? 0 : 1 }
    '
}

# Default code=true (fail-safe). Only flip to false when we have a non-empty file list
# AND every entry is a docs-class path.
code=true
if [ -n "$files" ]; then
    code=false
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        case "$f" in
            # Build-invoked exception: test-lint-rules.sh is the strict-zone delta gate
            # run by comment-noise-gate's code-gated "--diff origin/develop" step, plus
            # the develop-push "--catalog --refresh" job. It lives in the agent layer, so
            # it reaches this repo only through the agent-layer gitlink, and a change to
            # it MUST trigger the build so the gate is re-validated — otherwise a bump
            # that moves only the gate skips the build and the gate never runs. So a
            # pointer bump is code. Keep before the agents/* docs-class arm (case matches
            # the first pattern).
            agent-layer|.gitmodules) code=true; break ;;
            # Docs-class scripts: test-docs.sh is the local mirror of
            # .github/workflows/doc-validation.yml. It runs no compiler and touches no
            # product code, so a change to it alone must NOT trigger the expensive
            # Windows builds — doc-validation.yml re-validates it on its own. (Other
            # scripts/dev/*.sh DO affect the build and stay non-docs via the *) arm.)
            scripts/dev/test-docs.sh) ;;
            *.md|docs/*|agents/*|scripts/dev/local/*|.gitignore|.gitattributes|.coderabbit.yaml|.github/CODEOWNERS) ;;
            *) code=true; break ;;
        esac
    done <<< "$files"
fi
echo "non-docs changes present: ${code}" >&2

# Sanitizer-lane gate (docs/plans/ci-build-time-reduction.md Q5). The per-PR sanitizer
# lanes build Debug / RelWithDebInfo under instrumentation (uncacheable by sccache), so
# they run pre-merge only when the diff can change what they catch:
#   * first-party shared C++ that can introduce UB — Source/Core or Source/Plugins;
#   * the lanes' own configuration — cmake/Sanitizers.cmake, the pinned clang-cl action,
#     CMakePresets.json (the asan presets), sanitizer-nightly.yml, and the sanitizer
#     jobs of build-and-test.yml (workflow_sanitizer_touched: only a diff that touches
#     sanitizer lines, so an unrelated workflow edit does not pay for two 25-30 minute
#     Windows jobs);
#   * the doctest rig the lanes run — tests/**, except tests/bats/** (bash suites) and
#     tests/agent-eval/** (LLM-eval fixtures), which no compiled target reads.
# A lane that skips on the PR that reconfigures it first executes on someone else's PR:
# #2400 changed the UBSan lane's flags and rig with no Core C++, the lane skipped (a
# skipped required check is a pass), and its LLVM-20 crash surfaced on three later PRs.
# Fail-safe = TRUE: an empty/failed file list means the diff could not be classified, so
# the lanes run. In `case`, * spans '/', so nested paths match.
if [ -z "$files" ]; then
    source_core_cpp=true
else
    source_core_cpp=false
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        case "$f" in
            Source/Core/*.cpp|Source/Core/*.h|Source/Core/*.hpp|Source/Core/*.cc|Source/Core/*.cxx|Source/Core/*.hh|Source/Plugins/*.cpp|Source/Plugins/*.h|Source/Plugins/*.hpp|Source/Plugins/*.cc|Source/Plugins/*.cxx|Source/Plugins/*.hh)
                source_core_cpp=true; break ;;
            cmake/Sanitizers.cmake|.github/actions/setup-pinned-llvm/*|CMakePresets.json|.github/workflows/sanitizer-nightly.yml)
                source_core_cpp=true; break ;;
            tests/bats/*|tests/agent-eval/*) ;;
            tests/*)
                source_core_cpp=true; break ;;
            .github/workflows/build-and-test.yml)
                if workflow_sanitizer_touched "$f"; then
                    echo "build-and-test.yml diff touches the sanitizer lanes" >&2
                    source_core_cpp=true; break
                fi
                ;;
        esac
    done <<< "$files"
fi
echo "Core/Plugins C++, sanitizer config or the doctest rig changed (sanitizer gate): ${source_core_cpp}" >&2

# Android-only diff gate: skip the desktop standalone builds + their downstream tests when
# the diff touches ONLY android/mobile paths — Source/Mobile/** (compiled by NO desktop
# job) and cmake/toolchains/** (consumed only by the android-ndk preset). The
# mobile-android-ndk job gates on `code`, not this, so it still runs.
# Set true ONLY when code==true AND every changed file is an android-only path OR a
# docs-class path the `code` gate above treats as non-compiling. The two build-invoked
# arms MIRROR the `code` arms above so a gate-script change can't be mis-skipped.
# Fail-safe = FALSE (run the full desktop suite) on ANY uncertainty.
android_only=false
if [ "$code" = "true" ] && [ -n "$files" ]; then
    android_only=true
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        case "$f" in
            agent-layer|.gitmodules) android_only=false; break ;;
            Source/Mobile/*|cmake/toolchains/*) ;;
            scripts/dev/test-docs.sh) ;;
            *.md|docs/*|agents/*|scripts/dev/local/*|.gitignore|.gitattributes|.coderabbit.yaml|.github/CODEOWNERS) ;;
            *) android_only=false; break ;;
        esac
    done <<< "$files"
fi
echo "android-only diff (skip desktop builds): ${android_only}" >&2

printf 'code=%s\nsource_core_cpp=%s\nandroid_only=%s\n' "$code" "$source_core_cpp" "$android_only"
exit 0
