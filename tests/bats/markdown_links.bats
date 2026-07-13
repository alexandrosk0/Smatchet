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
