#!/usr/bin/env bash
# rewrite-plan-paths.sh — one-shot path rewriter for the docs/design -> docs/plans
# rename (Phase D of agentic-layer-project-independence).
#
# Rewrites, across all tracked files, the OLD plan paths to the NEW homes:
#   docs/design/archive/<slug>.md      -> docs/plans/shipped/<slug>.md
#   docs/design/<slug>.md (non-archive)-> docs/plans/active/<slug>.md
#   docs/backlog/BACKLOG_PLANS.md       -> docs/plans/INDEX.md
#
# ORDERING IS LOAD-BEARING: the `archive` rewrite runs BEFORE the bare `design`
# rewrite, else docs/plans/shipped/x would become docs/plans/active/archive/x.
#
# Skips generated metadata (.understand-anything/) — that tool regenerates it and
# rewriting it surfaces pre-existing mojibake. Emits a per-file change report.
#
# Idempotent-ish: a path already pointing at docs/plans/* is left alone (the
# regexes only match the OLD docs/design / docs/backlog prefixes).
set -euo pipefail

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""; for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && { echo "rewrite-plan-paths: python not found" >&2; exit 2; }

"$PY" - <<'PY'
import subprocess, re, sys

# (pattern, replacement) — ORDERED: archive before bare design.
subs = [
    (re.compile(r'docs/design/archive/'),            'docs/plans/shipped/'),
    (re.compile(r'docs/design/archive\b'),           'docs/plans/shipped'),
    (re.compile(r'docs/design/'),                    'docs/plans/active/'),
    (re.compile(r'docs/design\b'),                   'docs/plans/active'),
    (re.compile(r'docs/backlog/BACKLOG_PLANS\.md'),  'docs/plans/INDEX.md'),
    (re.compile(r'docs/backlog/BACKLOG_PLANS\b'),    'docs/plans/INDEX'),
]

files = subprocess.run(["git", "ls-files"], capture_output=True, text=True).stdout.split("\n")
report = []
total = 0
for f in files:
    if not f:
        continue
    if f.startswith(".understand-anything/"):   # generated metadata — regenerated, skip
        continue
    if not f.endswith((".md", ".sh", ".py", ".yml", ".yaml", ".json", ".cpp", ".h",
                       ".graphql")) and "git-hooks" not in f and ".gitattributes" not in f:
        continue
    try:
        txt = open(f, encoding="utf-8").read()
    except Exception:
        continue
    new = txt
    n = 0
    for pat, rep in subs:
        new, c = pat.subn(rep, new)
        n += c
    if new != txt:
        open(f, "w", encoding="utf-8", newline="\n").write(new)
        report.append((n, f))
        total += n

report.sort(reverse=True)
print("rewrite-plan-paths: %d occurrences across %d files" % (total, len(report)))
for n, f in report[:40]:
    print("  %4d  %s" % (n, f))
if len(report) > 40:
    print("  ... +%d more files" % (len(report) - 40))
PY
