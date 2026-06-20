#!/usr/bin/env bash
# test-plan-index.sh — keep the shipped-plan index in sync with the archive dir.
#
# Regenerates the table of shipped/archived plans from the actual *.md files in
# the archive directory, into the index file between the markers
#   <!-- BEGIN auto-plan-index --> ... <!-- END auto-plan-index -->
# Surrounding prose (the human §Notes etc.) is never touched.
#
# Modes:
#   (default) / --check   regenerate in memory, diff vs committed; exit 1 on drift
#   --fix                 rewrite the block in place from the live archive
#
# Per row: slug (filename), link (relative href), approx date (first-commit date
# via `git log --follow`), one-line summary (an in-file
# `<!-- index-summary: ... -->` override if present, else the plan's H1 title).
# Output is byte-stable (no timestamps) so --check is deterministic.
#
# Paths are config-driven below — the Phase-D rename (design/archive -> plans/
# shipped, BACKLOG_PLANS.md -> plans/INDEX.md) is a two-line edit here.
set -uo pipefail

# Resolve paths against the git root, not $PWD. When --fix is invoked from a
# subdir or a sibling worktree, a relative ARCHIVE_DIR/INDEX_FILE would either
# silently NO-OP (archive dir not found here) or index the WRONG tree. cd to the
# toplevel so the live archive + INDEX are always THIS repo's
# (plan-index-fix-wrong-cwd-silent-noop). Honour an explicit absolute override.
#
# Capture our OWN dir as an ABSOLUTE path BEFORE the cd: the later
# `. "$_SCRIPT_DIR/lib/..."` source and the self-re-invocation (`"$_SCRIPT_PATH"
# --check`) both rely on $0, which is relative for a relative invocation from a
# subdir — after `cd "$_GIT_ROOT"` a relative $0 no longer resolves. Resolve
# once here while $PWD is still the caller's dir.
_SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
_SCRIPT_PATH="$_SCRIPT_DIR/$(basename "$0")"
_GIT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || _GIT_ROOT=""
if [ -n "$_GIT_ROOT" ]; then
  cd "$_GIT_ROOT" || { echo "test-plan-index: cannot cd to git root '$_GIT_ROOT'" >&2; exit 2; }
fi

ARCHIVE_DIR="${PLAN_INDEX_ARCHIVE_DIR:-docs/plans/shipped}"
INDEX_FILE="${PLAN_INDEX_FILE:-docs/plans/INDEX.md}"
BEGIN_MARK="<!-- BEGIN auto-plan-index -->"
END_MARK="<!-- END auto-plan-index -->"

# Shared shallow-clone guard: on a shallow clone, git-log --follow dates drift
# from CI (which runs on full history), so a local --fix would commit an INDEX
# the required check rejects. Sourced lib auto-unshallows under --fix (matching
# CI) or refuses; WARNs under --check. See lib/plan-history-guard.sh header.
# shellcheck source=lib/plan-history-guard.sh
. "$_SCRIPT_DIR/lib/plan-history-guard.sh"

MODE="check"
for a in "$@"; do
  case "$a" in
    --fix) MODE="fix" ;;
    --check) MODE="check" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: $0 [--check|--fix|--selftest]" >&2; exit 2 ;;
  esac
done

# --selftest — prove (1) the script resolves paths against the git root (so a
# subdir invocation can't silent-NO-OP), and (2) the shallow-clone guard's
# check-mode WARN/refuse contract holds. No network; uses the live repo's own
# non-shallow state for the guard smoke. Asserts-behaviour, not snapshots.
if [ "$MODE" = "selftest" ]; then
  fail=0
  # (1) git-root resolution: ARCHIVE_DIR must resolve to an existing dir AFTER
  # the cd above, regardless of where the selftest was launched from.
  if [ ! -d "$ARCHIVE_DIR" ]; then
    echo "test-plan-index --selftest: FAIL — ARCHIVE_DIR '$ARCHIVE_DIR' not found after git-root cd" >&2
    fail=1
  fi
  # (2) guard contract: check-mode must NEVER exit non-zero on this (non-shallow)
  # repo, and the function must be defined by the sourced lib.
  if ! type is_shallow_or_refuse >/dev/null 2>&1; then
    echo "test-plan-index --selftest: FAIL — is_shallow_or_refuse not sourced" >&2
    fail=1
  else
    ( is_shallow_or_refuse check ) || {
      echo "test-plan-index --selftest: FAIL — guard check-mode returned non-zero on non-shallow repo" >&2
      fail=1
    }
  fi
  # (3) negative case.
  # selftest: asserts-failure — feed known-bad input (a non-existent archive
  # dir) and require a NON-ZERO exit. A regression that makes the script silently
  # NO-OP on a missing/wrong archive dir (the wrong-cwd class) would wrongly
  # succeed here. Re-invoke ourselves with the override so the real arg-parse +
  # archive-dir check runs end-to-end.
  if PLAN_INDEX_ARCHIVE_DIR="docs/plans/__no_such_archive_dir__$$" \
       "$_SCRIPT_PATH" --check >/dev/null 2>&1; then
    echo "test-plan-index --selftest: FAIL — a missing archive dir did NOT fail (silent NO-OP)" >&2
    fail=1
  fi
  if [ "$fail" = "0" ]; then
    echo "test-plan-index --selftest: PASS (git-root path resolution + shallow guard + missing-archive refusal)"
    echo "Passed: 1  Failed: 0"
    exit 0
  fi
  echo "Passed: 0  Failed: 1"
  exit 1
fi

# Guard the real run: shallow clones drift git-log dates from CI. Under --fix
# this auto-unshallows (or hard-refuses, exit 2); under --check it WARNs.
is_shallow_or_refuse "$MODE"

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""; for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && { echo "test-plan-index: python not found" >&2; exit 2; }

"$PY" - "$ARCHIVE_DIR" "$INDEX_FILE" "$BEGIN_MARK" "$END_MARK" "$MODE" <<'PY'
import os, re, subprocess, sys

archive_dir, index_file, begin, end, mode = sys.argv[1:6]

def _git_first_date_at(git_path):
    try:
        out = subprocess.run(["git", "log", "--follow", "--format=%ad", "--date=short", "--", git_path],
                             capture_output=True, text=True, check=False).stdout.strip().splitlines()
        return out[-1] if out else ""
    except Exception:
        return ""

def git_first_date(path):
    git_path = path.replace(os.sep, "/")
    d = _git_first_date_at(git_path)
    if not d:
        # A freshly `git mv`'d plan (active/ -> shipped/) has NO committed history at the
        # NEW path while the rename is only STAGED, so `git log --follow <new-path>` is
        # empty and the row gets a blank "—" date — which CI then re-fixes to the real
        # first-add date, drifting the committed index (the #1061 / #1092 archive
        # date-drift, twice). Fall back to the SIBLING tier: the rename source still
        # carries the history. Covers active<->shipped in both directions.
        if "/plans/shipped/" in git_path:
            d = _git_first_date_at(git_path.replace("/plans/shipped/", "/plans/active/"))
        elif "/plans/active/" in git_path:
            d = _git_first_date_at(git_path.replace("/plans/active/", "/plans/shipped/"))
    return d or "—"

def summary_for(path):
    h1 = ""
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'^\s*<!--\s*index-summary:\s*(.+?)\s*-->\s*$', line)
            if m:
                return m.group(1).strip()
            if not h1 and line.startswith("# "):
                h1 = line[2:].strip()
    return h1 or "_(summary TODO)_"

if not os.path.isdir(archive_dir):
    print("test-plan-index: archive dir %s not found" % archive_dir, file=sys.stderr); sys.exit(2)

PLACEHOLDER = "—"  # em-dash: emitted when no first-commit date resolves.

rows = []
placeholder_slugs = []
for fn in sorted(os.listdir(archive_dir)):
    if not fn.endswith(".md") or fn.startswith("_"):
        continue
    slug = fn[:-3]
    path = os.path.join(archive_dir, fn)
    date = git_first_date(path)
    if date == PLACEHOLDER:
        placeholder_slugs.append(slug)
    summ = summary_for(path).replace("|", "\\|")
    # link relative to the index file's directory
    href = os.path.relpath(path, os.path.dirname(index_file)).replace(os.sep, "/")
    rows.append((date, slug, "| [`%s`](%s) | %s | %s |" % (slug, href, date, summ)))

rows.sort(key=lambda r: (r[0], r[1]))
block = [begin,
         "<!-- Generated by agents/scripts/core/test-plan-index.sh — do not hand-edit between the markers.",
         "     Override a row's summary with an `<!-- index-summary: ... -->` comment in the plan file. -->",
         "",
         "| Plan (slug) | Approx. date | One-line summary |",
         "|---|---|---|"]
block += [r[2] for r in rows]
block.append(end)
new_block = "\n".join(block)

with open(index_file, encoding="utf-8") as f:
    content = f.read()

if begin not in content or end not in content:
    print("test-plan-index: markers not found in %s" % index_file, file=sys.stderr)
    print("  add a '%s' / '%s' pair where the shipped-plan table should live." % (begin, end), file=sys.stderr)
    sys.exit(2)

pre = content[:content.index(begin)]
post = content[content.index(end) + len(end):]
rebuilt = pre + new_block + post

if rebuilt == content:
    print("test-plan-index: index up to date (%d plans)" % len(rows))
    print("Passed: 1  Failed: 0")
    sys.exit(0)

if mode == "fix":
    # Deterministic-date guard (test-plan-index-fix-shipped-date-placeholder):
    # CI's autosync runs this same script on FULL history, so every shipped plan
    # there resolves a real git-log first-commit date. If a local --fix would
    # write the "—" placeholder for any row, the resulting INDEX is NOT
    # byte-identical to what CI regenerates — committing it drifts the required
    # check on a date the author can't reproduce. REFUSE rather than emit the
    # placeholder, with the same remedy the shallow guard names.
    if placeholder_slugs:
        print("test-plan-index: REFUSING --fix — no git-log first-commit date resolves for:",
              file=sys.stderr)
        for s in placeholder_slugs:
            print("    - %s" % s, file=sys.stderr)
        print("  CI derives these from FULL history; emitting the '—' placeholder here would",
              file=sys.stderr)
        print("  drift the committed INDEX from CI's regeneration. Likely a shallow clone or an",
              file=sys.stderr)
        print("  uncommitted plan file.", file=sys.stderr)
        print("  Remedy: git fetch --unshallow  (or commit the plan), then re-run --fix.",
              file=sys.stderr)
        print("Passed: 0  Failed: 1")
        sys.exit(2)
    with open(index_file, "w", encoding="utf-8", newline="\n") as f:
        f.write(rebuilt)
    print("test-plan-index: rewrote index (%d plans)" % len(rows))
    print("Passed: 1  Failed: 0")
    sys.exit(0)

# check mode: report drift
print("test-plan-index: DRIFT — shipped-plan index out of sync (%d plans in archive)." % len(rows), file=sys.stderr)
print("  run: bash agents/scripts/core/test-plan-index.sh --fix", file=sys.stderr)
print("Passed: 0  Failed: 1")
sys.exit(1)
PY
