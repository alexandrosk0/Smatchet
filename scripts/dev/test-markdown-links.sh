#!/usr/bin/env bash
# test-markdown-links.sh — flag dangling relative markdown links across docs.
#
# Why: sed-based path renames (e.g. docs/design/applied/ → archive/) routinely
# update body text but miss `[label](href)` link hrefs when only the label
# happens to look like the path. PR #496 + #497 each shipped with 3-4
# CR-caught broken-href findings of exactly this shape; this lint catches
# them mechanically before push.
#
# Rules:
#   1. Only relative links are checked (paths starting with `/`, `http://`,
#      `https://`, `mailto:`, or `#` are skipped).
#   2. The href is resolved relative to the source markdown file's directory.
#   3. Anchor fragments (`#section`) are stripped before existence check.
#   4. Image links (`![alt](path)`) are NOT checked — image assets vary by
#      build state.
#   5. Targets: docs/**/*.md, agents/**/*.md, AGENTS.md, BUILD.md, README.md
#      (if present).
#
# Bypass: SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 (logged when used).
#
# Modes:
#   default — diff-scope: only check markdown files modified vs origin/develop.
#             Grandfathers pre-existing broken refs; catches new regressions.
#   --all   — repo-wide scan of every actively-maintained markdown.
#
# Exit codes:
#   0 — every relative link in every scanned markdown resolves to an existing file
#   1 — at least one dangling link
#   2 — missing binary (python3)

set -euo pipefail
cd "$(dirname "$0")/../.."

if [ "${SMATCHET_SKIP_MARKDOWN_LINK_CHECK:-0}" = "1" ]; then
    echo "test-markdown-links: SMATCHET_SKIP_MARKDOWN_LINK_CHECK=1 — skipping" >&2
    echo "Passed: 0  Failed: 0"
    exit 0
fi

command -v python3 >/dev/null 2>&1 || { echo "python3 required" >&2; exit 2; }

# Diff-scope by default; --all overrides for whole-repo audit.
SCOPE="diff"
if [ "${1:-}" = "--all" ]; then
    SCOPE="all"
fi
export SCOPE

python3 - <<'PY'
import os
import re
import sys

import subprocess

REPO_ROOT = os.getcwd()
SCOPE = os.environ.get("SCOPE", "diff")
# Excluded paths under docs/ (active vs archived): docs/design/applied/ and
# docs/design/archive/ hold shipped/historical plans whose link-paths drifted
# over time without being maintained — they're documentation-history, not
# active. Scope to docs that ARE actively maintained.
EXCLUDED_PREFIXES = (
    os.path.join("docs", "design", "applied"),
    os.path.join("docs", "design", "archive"),
)


def is_active_md(rel_path):
    """True if rel_path is one of the actively-maintained markdown files
    (docs/, agents/, root-level repo docs) and NOT under an archived dir."""
    if not rel_path.endswith(".md"):
        return False
    parts = rel_path.split(os.sep)
    if parts[0] in ("AGENTS.md", "BUILD.md", "README.md") and len(parts) == 1:
        return True
    if parts[0] not in ("docs", "agents"):
        return False
    for p in EXCLUDED_PREFIXES:
        if rel_path == p or rel_path.startswith(p + os.sep):
            return False
    return True


TARGETS = []
if SCOPE == "diff":
    # Find every markdown changed vs origin/develop (ahead-range).
    try:
        out = subprocess.run(
            ["git", "diff", "--name-only", "origin/develop...HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout
        # Also include uncommitted working-tree changes.
        out += subprocess.run(
            ["git", "diff", "--name-only", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(f"WARN: git diff failed ({exc}); falling back to repo-wide\n")
        out = ""
    changed = sorted(set(
        line.strip() for line in out.splitlines() if line.strip()
    ))
    for rel_path in changed:
        if is_active_md(rel_path) and os.path.exists(rel_path):
            TARGETS.append(os.path.join(REPO_ROOT, rel_path))
else:
    # Repo-wide scan.
    for root, dirs, files in os.walk(REPO_ROOT):
        skip = {".git", "build", "node_modules", "_deps", "FetchContent",
                "external", "__pycache__", ".claude"}
        dirs[:] = [d for d in dirs if d not in skip]
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, REPO_ROOT)
            if is_active_md(rel):
                TARGETS.append(full)

# Match `[label](href)` but NOT image links `![alt](path)`. Negative
# look-behind for `!`. The href group is everything up to the closing `)`
# but stops at whitespace (defensive against malformed links).
LINK_RE = re.compile(r'(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)')

violations = []
checked = 0
for path in TARGETS:
    try:
        with open(path, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                for m in LINK_RE.finditer(line):
                    href = m.group(1)
                    # Skip absolute / external / anchor-only / mailto.
                    if href.startswith(("/", "http://", "https://", "mailto:",
                                        "#", "ftp://", "file://", "data:")):
                        continue
                    # Strip anchor fragment.
                    href_path = href.split("#", 1)[0]
                    if not href_path:
                        continue
                    # Strip `:NNN` line-anchor suffix — `[label](Foo.cpp:123)`
                    # is widely used in plan-docs as a file+line reference.
                    # Treat as if pointing at the file.
                    line_match = re.match(r"^(.*?):\d+(?::\d+)?$", href_path)
                    if line_match:
                        href_path = line_match.group(1)
                    # Resolve relative to the file's directory.
                    src_dir = os.path.dirname(path)
                    target = os.path.normpath(os.path.join(src_dir, href_path))
                    if not os.path.exists(target):
                        rel_src = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
                        violations.append(
                            f"{rel_src}:{lineno}: BROKEN_LINK: '{href}' "
                            f"does not resolve (looked at {os.path.relpath(target, REPO_ROOT).replace(os.sep, '/')})"
                        )
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"WARN: could not read {path}: {exc}\n")
    checked += 1

for v in violations:
    print(v, file=sys.stderr)

passed = checked - (1 if violations else 0)
failed = 1 if violations else 0
print(f"Passed: {passed}  Failed: {failed}  (scanned {checked} markdown file(s); "
      f"{len(violations)} dangling link(s))")
sys.exit(1 if violations else 0)
PY
