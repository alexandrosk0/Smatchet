#!/usr/bin/env bash
# test-portable-purity.sh — keep the PORTABLE agentic layer free of NEW project
# literals, so it stays reusable in another project (agentic-layer-project-
# independence § "portable extraction done" bar).
#
# Portable dirs (must not gain new project-specific literals):
#     agents/core/  agents/_shared/  docs/agent-rules/  docs/harness/
# Denylist = generated from project.config.json (project.name, env_prefix,
# project.literals, build.presets, vcs.p4_streams) — NOT a hardcoded pair, so a
# reused project rewrites the config and the guard re-targets automatically.
#
# Reality: the prompts still embed project specifics today (the structure is
# portable; full de-Smatchet-ification of the prompts is tracked follow-up).
# So this is a BASELINE guard (same pattern as test-lint-rules --catalog):
#   default / --check   fail only on leakage NOT in the committed baseline
#   --refresh           regenerate the baseline from the current tree
# Baseline: docs/high-integrity/portable-purity-baseline.txt (sorted file:literal).
set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 1

BASELINE="docs/high-integrity/portable-purity-baseline.txt"
PORTABLE_DIRS=(agents/core agents/_shared docs/agent-rules docs/harness)
MODE="check"; [ "${1:-}" = "--refresh" ] && MODE="refresh"

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""; for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && { echo "test-portable-purity: python not found" >&2; exit 2; }

current="$("$PY" - "$BASELINE" <<'PY'
import json, os, re, sys

cfg = json.load(open("project.config.json", encoding="utf-8"))
p = cfg.get("project", {})
terms = set(filter(None, [p.get("name"), p.get("env_prefix")]))
terms |= set(p.get("literals", []))
terms |= set(cfg.get("build", {}).get("presets", []))
terms |= set(cfg.get("vcs", {}).get("p4_streams", []))
terms = {t for t in terms if t}   # drop empty/None terms (would match everywhere)
if not terms:
    sys.exit(0)
# longest first; whole-token-ish match
pat = re.compile("|".join(re.escape(t) for t in sorted(terms, key=len, reverse=True)))

PORTABLE = ("agents/core/", "agents/_shared/", "docs/agent-rules/", "docs/harness/")
import subprocess
# Only GIT-TRACKED files — never generated artifacts (__pycache__/*.pyc, etc.)
# that exist on a CI runner but aren't part of the canonical tree.
tracked = subprocess.run(["git", "ls-files", "--", *PORTABLE],
                         capture_output=True, text=True).stdout.split("\n")
hits = []
for fp in tracked:
    if not fp or fp.endswith(".tmpl"):  # *.tmpl = project-specific templates
        continue
    try:
        txt = open(fp, encoding="utf-8", errors="ignore").read()
    except Exception:
        continue
    for m in set(pat.findall(txt)):
        hits.append("%s\t%s" % (fp, m))
for h in sorted(set(hits)):
    print(h)
PY
)"

if [ "$MODE" = "refresh" ]; then
  mkdir -p "$(dirname "$BASELINE")"
  printf '%s\n' "$current" > "$BASELINE"
  echo "test-portable-purity: baseline refreshed ($(printf '%s\n' "$current" | grep -c . ) entries)"
  echo "Passed: 1  Failed: 0"
  exit 0
fi

# Strip CR so a CRLF-checked-out baseline (Windows autocrlf) still matches the
# LF scan output.
base="$( [ -f "$BASELINE" ] && tr -d '\r' < "$BASELINE" || true )"
current="$(printf '%s' "$current" | tr -d '\r')"
# NEW leakage = current entries not in the baseline.
new="$(comm -13 <(printf '%s\n' "$base" | sort -u) <(printf '%s\n' "$current" | sort -u) | grep -c . || true)"
if [ "${new:-0}" -eq 0 ]; then
  echo "test-portable-purity: no NEW project literals in portable dirs (baseline holds)."
  echo "Passed: 1  Failed: 0"
  exit 0
fi
echo "test-portable-purity: $new NEW project-literal leak(s) into portable dirs:" >&2
comm -13 <(printf '%s\n' "$base" | sort -u) <(printf '%s\n' "$current" | sort -u) | sed 's/^/  /' >&2
echo "  Use config-key references instead, or (if intentional) refresh: bash agents/scripts/core/test-portable-purity.sh --refresh" >&2
echo "Passed: 0  Failed: 1"
exit 1
