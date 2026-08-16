#!/usr/bin/env bats
# tests/bats/markdown_links.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/test-markdown-links.sh.
#
# Why this lint exists: sed-based path renames routinely update body text but
# miss `[label](href)` link hrefs when the label happens to match the path.
# PRs #496 + #497 each shipped with 3-4 CR-caught broken-href findings of
# exactly this shape; this lint catches them mechanically before push.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export LINT="$REPO_ROOT/agents/scripts/core/test-markdown-links.sh"
    FIXTURE_DIR="$(mktemp -d)"
    export FIXTURE_DIR
    # Resolve a WORKING python interpreter (bats-coverage-01): a bare `python3`
    # matches the Windows WinStore alias which passes `command -v` but exits 49
    # on exec, red-walling 5/7 of these tests. Exec-validate each candidate and
    # skip cleanly when none runs (mirrors agent_size.bats).
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
}

teardown() {
    rm -rf "${FIXTURE_DIR:-}"
}

# ---------- env-gate ----------

@test "SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 bypasses and exits 0" {
    SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 run bash "$LINT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1"* ]]
    [[ "$output" == *"Passed: 0  Failed: 0"* ]]
}

# ---------- broken-link detection (inline Python with isolated REPO_ROOT) ----------

@test "broken relative link is flagged" {
    # Set up a fixture under the bats temp dir. Use cygpath on Windows to give
    # Python a path it can chdir to (MSYS /tmp/... isn't visible to native python3).
    mkdir -p "$FIXTURE_DIR/docs"
    cat > "$FIXTURE_DIR/docs/broken.md" <<'MD'
# Broken

See [missing target](./does-not-exist.md) for context.
MD
    PY_FIXTURE_DIR="$FIXTURE_DIR"
    if command -v cygpath >/dev/null 2>&1; then
        PY_FIXTURE_DIR="$(cygpath -w "$FIXTURE_DIR")"
    fi
    # Assert the regex + path-resolve mechanics directly.
    run "$PY" -c "
import os, re
os.chdir(r'$PY_FIXTURE_DIR')
text = open('docs/broken.md').read()
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)\)')
for m in LINK_RE.finditer(text):
    href = m.group(1)
    if href.startswith(('/', 'http://', 'https://', '#')): continue
    href_path = href.split('#', 1)[0]
    target = os.path.normpath(os.path.join('docs', href_path))
    if not os.path.exists(target):
        print(f'BROKEN: {href} -> {target}')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"BROKEN: ./does-not-exist.md"* ]]
}

@test "absolute / external / anchor links are NOT flagged" {
    run "$PY" -c "
import re
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)\)')
hrefs = ['/abs/path.md', 'https://example.com', 'http://a.b', 'mailto:x@y',
         '#section', 'ftp://server/file']
flagged = 0
for h in hrefs:
    if not h.startswith(('/', 'http://', 'https://', 'mailto:', '#', 'ftp://', 'file://', 'data:')):
        flagged += 1
        print(f'BUG: {h} not skipped')
print(f'flagged={flagged}')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"flagged=0"* ]]
}

@test "image links (![alt](path)) are NOT flagged" {
    run "$PY" -c "
import re
text = '![image](missing.png)\n[regular](missing.md)'
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)\)')
hits = [m.group(1) for m in LINK_RE.finditer(text)]
print('hits:', hits)
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hits: ['missing.md']"* ]]
    [[ "$output" != *"missing.png"* ]]
}

@test "line-anchor suffix (path:NNN) is stripped before existence check" {
    run "$PY" -c "
import re, os
hrefs = ['Source/Core/include/Foo.h:58', 'Source/Core/src/Bar.cpp:123:7', 'Plain.md']
for h in hrefs:
    m = re.match(r'^(.*?):\d+(?::\d+)?$', h)
    stripped = m.group(1) if m else h
    print(f'{h} -> {stripped}')
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Source/Core/include/Foo.h:58 -> Source/Core/include/Foo.h"* ]]
    [[ "$output" == *"Source/Core/src/Bar.cpp:123:7 -> Source/Core/src/Bar.cpp"* ]]
    [[ "$output" == *"Plain.md -> Plain.md"* ]]
}

# ---------- exclude archived plan dirs ----------

@test "docs/plans/shipped/ is excluded from default scan (was docs/design/archive/)" {
    run "$PY" -c "
import os
EXCLUDED = ('docs/plans/shipped',)
def is_active_md(rel_in):
    if not rel_in.endswith('.md'): return False
    rel = rel_in.replace(os.sep, '/')
    parts = rel.split('/')
    if parts[0] in ('AGENTS.md', 'BUILD.md', 'README.md') and len(parts) == 1: return True
    if parts[0] not in ('docs', 'agents'): return False
    for p in EXCLUDED:
        if rel == p or rel.startswith(p + '/'): return False
    return True
print('active:', is_active_md('docs/plans/active/foo.md'))

print('shipped excluded:', is_active_md('docs/plans/shipped/baz.md'))
print('agents:', is_active_md('agents/whatever.md'))
print('root agents.md:', is_active_md('AGENTS.md'))
print('src:', is_active_md('Source/Core/foo.md'))
"
    [ "$status" -eq 0 ]
    [[ "$output" == *"active: True"* ]]
    
    [[ "$output" == *"shipped excluded: False"* ]]
    [[ "$output" == *"agents: True"* ]]
    [[ "$output" == *"root agents.md: True"* ]]
    [[ "$output" == *"src: False"* ]]
}

# ---------- widened target set + baseline (gate-blind-spot-sweep Slice 3) ----------

@test "the remaining root-level docs are IN the target set" {
    # Rule 5's target list and is_active_md()'s root-file tuple must agree — the
    # header is the documented contract. cli.md carried two dangling links for the
    # entire time it sat outside the scan, which is why this parity check exists.
    for f in AGENTS.md BUILD.md README.md AI_POLICY.md CONTEXT-MAP.md; do
        grep -q "\"$f\"" "$LINT" || {
            echo "is_active_md() does not list $f" >&2
            return 1
        }
        grep -q "$f" <(sed -n '1,45p' "$LINT") || {
            echo "rule 5 header does not document $f" >&2
            return 1
        }
    done
}

@test "the relocated guides and audit reports are covered via the docs/ branch" {
    # The root-declutter move put the user-facing guides under docs/guides/ and the
    # dated audit reports under docs/audits/, so they no longer need a root-file
    # tuple entry — is_active_md() matches them on parts[0] == 'docs'. This asserts
    # they really are under docs/ (a move back to the root without a matching tuple
    # entry would silently drop them out of the scan, the exact Slice 3 regression).
    for f in docs/guides/cli.md docs/guides/lua.md docs/guides/mcp.md \
             docs/audits/SECURITY_AUDIT.md docs/audits/CPP_CODE_AUDIT.md \
             docs/audits/AGENTIC_INFRA_AUDIT.md docs/audits/UX_DESIGN_CRITIQUE.md \
             docs/audits/TEST_COVERAGE_GAP_MAP.md docs/audits/MUTATION_PILOT.md; do
        [ -f "$REPO_ROOT/$f" ] || {
            echo "$f is not where the docs/ branch of is_active_md() expects it" >&2
            return 1
        }
    done
}

@test "the scanner actually walks docs/guides/ and docs/audits/, not just the file-existence check above" {
    # The file-existence test above passes even if is_active_md() stops scanning
    # docs/** entirely — it only proves the files are on disk, not that the scanner
    # sees them. Exercise $LINT --all for real, on an isolated fixture (same
    # copied-script idiom as the --all tests below), with a dangling link planted
    # in each relocated directory, and assert both are actually flagged.
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" "$FIXTURE_DIR/docs/guides" "$FIXTURE_DIR/docs/audits"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    printf '# Guide\n\nSee [gone](./nope-guide.md).\n' > "$FIXTURE_DIR/docs/guides/cli.md"
    printf '# Audit\n\nSee [gone](./nope-audit.md).\n' > "$FIXTURE_DIR/docs/audits/SECURITY_AUDIT.md"

    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 1 ]
    [[ "$output" == *"docs/guides/cli.md"*"nope-guide.md"* ]]
    [[ "$output" == *"docs/audits/SECURITY_AUDIT.md"*"nope-audit.md"* ]]
}

@test "--all passes on the real repo using the committed baseline" {
    # --all must be usable as a gate: without the baseline, widening the target set
    # would have meant burning down the pre-existing links first. The --baseline
    # WRITE path is exercised by the round-trip test below, not here.
    run bash "$LINT" --all
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed:"* ]]
    [ -f "$REPO_ROOT/docs/high-integrity/markdown-link-baseline.md" ]
}

@test "--all grandfathers a baselined link but still FAILS on a new one" {
    # End-to-end on the REAL script. It re-anchors with
    # `cd "$(dirname "$0")/../../.."`, so the copy has to sit at the same depth
    # inside the fixture for that to land on the fixture root instead of the repo.
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" "$FIXTURE_DIR/docs/high-integrity"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    printf '# a\n\n[old](missing-old.md)\n[new](missing-new.md)\n' > "$FIXTURE_DIR/docs/a.md"
    {
        printf '# Dangling markdown links — grandfathered baseline\n\n'
        printf '## dangling links (1)\n'
        printf -- '- `docs/a.md` — `missing-old.md` (resolves to `docs/missing-old.md`)\n'
    } > "$FIXTURE_DIR/docs/high-integrity/markdown-link-baseline.md"

    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 1 ]
    [[ "$output" == *"missing-new.md"* ]]
    [[ "$output" != *"BROKEN_LINK: 'missing-old.md'"* ]]
    [[ "$output" == *"1 grandfathered"* ]]
}

@test "the rendered baseline rows parse back (round-trip)" {
    # Regression guard: the row regex was once anchored at end-of-line while the
    # renderer appends "(resolves to ...)", so EVERY row failed to parse and --all
    # grandfathered nothing — while the baseline file itself looked perfectly fine.
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" "$FIXTURE_DIR/docs"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    printf '# a\n\n[gone](missing-one.md)\n' > "$FIXTURE_DIR/docs/a.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --baseline
    [ "$status" -eq 0 ]
    [ -f "$FIXTURE_DIR/docs/high-integrity/markdown-link-baseline.md" ]
    # Written by --baseline, read back by --all: the whole point of the round trip.
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 grandfathered"* ]]
}

@test "an unknown argument is an infra error (exit 2), not a silent full scan" {
    run bash "$LINT" --definitely-not-a-mode
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown argument"* ]]
}

# ---------- diff scope default (smoke) ----------

@test "default mode runs without crash on the real repo (no diff = 0/0)" {
    # The repo's working tree state is whatever bats sees. The lint should
    # always return cleanly when SCOPE=diff and no markdown is in the diff
    # OR every diff-touched markdown's links resolve.
    run bash "$LINT"
    # Acceptable exits: 0 (clean) or 1 (real broken refs in current diff).
    # Either way it should NOT exit 2 (binary missing) or crash unexpectedly.
    [ "$status" -eq 0 ] || [ "$status" -eq 1 ]
    [[ "$output" == *"Passed:"* ]]
    [[ "$output" == *"Failed:"* ]]
}

# ---------- inline code spans (tooling 2026-08-07) ----------
#
# Two halves of one blindness: a repo path written as a bare code span was never
# checked (so a backlog entry could cite a file that does not exist), and prose
# quoting LINK SYNTAX inside a code span was parsed as a real link and reported
# dangling. Teaching the tokenizer about code spans fixes both.

# Build a fixture repo with a real `origin/develop` ref — the code-span rule
# falls back to that tree, and stays SILENT when it cannot be resolved, so a
# git-less fixture would vacuously pass every warning assertion below.
_mk_span_repo() {
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" \
             "$FIXTURE_DIR/docs/self-improvement/categories/tooling"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    git -C "$FIXTURE_DIR" init -q
    git -C "$FIXTURE_DIR" config user.email t@t
    git -C "$FIXTURE_DIR" config user.name t
    git -C "$FIXTURE_DIR" add -A
    git -C "$FIXTURE_DIR" commit -qm base
    git -C "$FIXTURE_DIR" update-ref refs/remotes/origin/develop HEAD
}

@test "a backlog code span citing a nonexistent repo path WARNs without failing" {
    _mk_span_repo
    printf -- '- entry\n\n  Proposal anchored on `scripts/dev/does-not-exist-xyz.sh` here.\n' \
        > "$FIXTURE_DIR/docs/self-improvement/categories/tooling/e.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]   # WARN-first: never changes the exit code
    [[ "$output" == *"WARN: code-span path 'scripts/dev/does-not-exist-xyz.sh'"* ]]
}

@test "a backlog code span citing a path that EXISTS draws no warning" {
    _mk_span_repo
    printf -- '- entry\n\n  Anchored on `agents/scripts/core/test-markdown-links.sh` which exists.\n' \
        > "$FIXTURE_DIR/docs/self-improvement/categories/tooling/e.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN: code-span path"* ]]
}

# applied.md is the ARCHIVE of already-actioned entries: a path that has since
# moved is expected there and nobody will act on it. Without this exemption the
# real repo emits ~100 rows of noise that drown the live signal.
@test "applied.md is exempt from the code-span path rule" {
    _mk_span_repo
    printf -- '- old entry citing `scripts/dev/does-not-exist-xyz.sh`\n' \
        > "$FIXTURE_DIR/docs/self-improvement/categories/applied.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN: code-span path"* ]]
}

# `Source/Core/.../Commands/Foo.h` is prose shorthand for "somewhere under",
# never a literal file — it cannot resolve and was never meant to.
@test "an ELIDED code-span path (...) draws no warning" {
    _mk_span_repo
    printf -- '- entry citing `Source/Core/.../Commands/PathConfinement.h` loosely\n' \
        > "$FIXTURE_DIR/docs/self-improvement/categories/tooling/e.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN: code-span path"* ]]
}

# The false-positive half: a doc that needs to DISCUSS link syntax had to
# describe it in words, because a link-shaped code span was checked as a link.
@test "link-shaped text inside a code span is not parsed as a link" {
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" "$FIXTURE_DIR/docs"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    printf '# a\n\nThe checker matches `[label](some-missing-target.md)` shaped text.\n' \
        > "$FIXTURE_DIR/docs/a.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" != *"BROKEN_LINK"* ]]
}

# A REAL link whose label happens to be a code span must still be checked —
# blanking spans must not blind the checker to the href half.
@test "a real link with a code-span label is still checked" {
    mkdir -p "$FIXTURE_DIR/agents/scripts/core" "$FIXTURE_DIR/docs"
    cp "$LINT" "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh"
    printf '# a\n\nSee [`docs/gone.md`](gone-missing-target.md) for detail.\n' \
        > "$FIXTURE_DIR/docs/a.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 1 ]
    [[ "$output" == *"BROKEN_LINK: 'gone-missing-target.md'"* ]]
}

# The common case the entry calls out: an entry and the file it cites land in
# the SAME PR. That path is in the worktree but not yet at origin/develop, so a
# develop-only check would false-warn on nearly every new entry. HEAD is
# therefore consulted FIRST, with develop only as the fallback.
@test "a code-span path added by this change (worktree-only, not on develop) draws no warning" {
    _mk_span_repo
    # Created AFTER the base commit, so it exists in the worktree and is absent
    # from the origin/develop tree.
    mkdir -p "$FIXTURE_DIR/scripts/dev"
    printf '#!/usr/bin/env bash\n' > "$FIXTURE_DIR/scripts/dev/brand-new-helper.sh"
    printf -- '- entry\n\n  Anchored on `scripts/dev/brand-new-helper.sh` added by this same change.\n' \
        > "$FIXTURE_DIR/docs/self-improvement/categories/tooling/e.md"
    run bash "$FIXTURE_DIR/agents/scripts/core/test-markdown-links.sh" --all
    [ "$status" -eq 0 ]
    [[ "$output" != *"WARN: code-span path"* ]]
}
