#!/usr/bin/env bash
# migrate-bugs-to-issues.sh — one-time migration of the deprecated bug.md backlog
# to GitHub Issues (genuine product bugs) + debt.md (tech-debt), per ADR-0014.
#
# Classifies each bug.md entry bug-vs-debt by the deterministic rule
# (docs/agent-rules/issue-triage.md): user-observable defect / correctness-safety
# violation → Issue; internal maintainability with no observable defect → debt;
# unclear → AMBIGUOUS (left in bug.md, flagged for a human — never auto-filed).
#
# Usage:
#   bash agents/scripts/project/migrate-bugs-to-issues.sh             # dry-run (default): print the split
#   bash agents/scripts/project/migrate-bugs-to-issues.sh --apply     # create Issues + move debt + prune bug.md
#
# Env: REPO (owner/repo, default from `gh repo view`); BUG_MD / DEBT_MD path overrides (bats).
#
# Exit codes (test-author convention): 0 ok · 2 bad args / missing file / gh missing.

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

BUG_MD="${BUG_MD:-docs/self-improvement/categories/bug.md}"
DEBT_MD="${DEBT_MD:-docs/self-improvement/categories/debt.md}"

MODE="dry-run"
case "${1:-}" in
    --apply) MODE="apply" ;;
    "")      MODE="dry-run" ;;
    *) echo "usage: $0 [--apply]" >&2; exit 2 ;;
esac
[ -f "$BUG_MD" ] || { echo "migrate-bugs: missing $BUG_MD" >&2; exit 2; }

# Probe-EXECUTE each candidate, don't just `command -v` it: on Windows `python3`
# resolves to the Microsoft Store App Execution Alias stub, which is on PATH but
# exits non-zero on run — a resolve-only probe picks it and the classifier yields
# 0 records.
PY=""
for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "migrate-bugs: no working python interpreter" >&2; exit 2; }

# The classifier emits one record per entry: verdict<TAB>prio<TAB>area<TAB>title<TAB>bodyfile
# (bodyfile = a temp file holding the Issue body). Parsing + the bug-vs-debt heuristic live
# in Python (multi-line entries + keyword rules are awkward in pure bash).
TMPDIR_M="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_M"' EXIT

"$PY" - "$BUG_MD" "$TMPDIR_M" <<'PY' > "$TMPDIR_M/records.tsv"
import re, sys, os
# Windows default stdio is cp1252 — bug.md carries em-dashes / ↔ / smart quotes, so force utf-8
# (the exact cp1252 crash class the harness has hit before).
for s in (sys.stdout, sys.stderr):
    try: s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass
bug_md, tmp = sys.argv[1], sys.argv[2]
text = open(bug_md, encoding="utf-8", errors="replace").read().split("\n")

# SAFETY/correctness violations ALWAYS win (a race / UB / PII / freeze is a bug regardless of any
# "refactor" mention in the entry's explanation). Checked over the whole entry.
SAFETY = ("crash", " ub ", " ub.", "race", "unlocked while", "block the ui", "blocks the ui",
          "freeze", "pii", "email at info", "memory leak", "leaks memory", "data corrupt",
          "deadlock", "use-after-free", "overflow", "thread doesn't terminate", "doesn't terminate")
# Explicit tech-DEBT markers (internal maintainability, no observable defect). Deliberately strong
# — NOT "refactor" alone, which appears in bug explanations ("behaviour-preserving refactor").
DEBT = ("code-health", "god-object", "duplicat", "coupling", "lives inline in a hot header",
        "architecture/coupling", "code health")
# Behavioural-bug markers — judged on the TITLE only (the crisp claim), not the body's context.
BEHAVIOR = ("wrong", "ignores", "can't be cancelled", "cannot be cancelled", "returns success",
            "unescaped", "zero tickets", "doesn't tokenize", "stale client")
# Light area heuristic (optional; human/sweep refines). Maps a title keyword → area:<x>.
AREA = [("lua", "lua-binder"), ("tracker", "tracker-backend"), ("field-catalog", "tracker-backend"),
        ("grid", "grid-engine"), ("mcp", "mcp-toolsmith"), ("p4 ", "p4-annotate"),
        ("syntax highlight", "p4-annotate")]

# Split into entries: a header line `- <date> · ... · [bug] · P<k> — <title>` + indented continuation.
entries = []
cur = None
hdr = re.compile(r"^- (\d{4}-\d{2}-\d{2}) · ([^·]+)· \[bug\] · (P\d) — (.+)$")
for line in text:
    m = hdr.match(line)
    if m:
        if cur: entries.append(cur)
        cur = {"date": m.group(1), "prio": m.group(3), "title": m.group(4).strip(), "body": []}
    elif cur is not None:
        cur["body"].append(line)
if cur: entries.append(cur)

def classify(title, body):
    blob = (title + " " + body).lower()
    tlow = title.lower()
    for k in SAFETY:            # correctness/safety violation → always a bug
        if k in blob: return "bug"
    for k in DEBT:              # explicit, strong tech-debt markers
        if k in blob: return "debt"
    for k in BEHAVIOR:          # behavioural bug, judged on the title's crisp claim
        if k in tlow: return "bug"
    return "ambiguous"         # vague → leave in bug.md, flag a human (never silently file)

def area_for(title):
    t = title.lower()
    for kw, a in AREA:
        if kw in t: return a
    return ""

for i, e in enumerate(entries):
    body = "\n".join(e["body"]).strip()
    verdict = classify(e["title"], body)
    area = area_for(e["title"])
    bf = os.path.join(tmp, "body_%d.txt" % i)
    with open(bf, "w", encoding="utf-8") as fh:
        fh.write("%s\n\n%s\n\n_Migrated from `docs/self-improvement/categories/bug.md` "
                 "(%s, originally %s) per ADR-0014._\n" % (e["title"], body, e["prio"], e["date"]))
    # TSV: verdict, prio, area, title, bodyfile. Empty area → "-" (a bash `read` with IFS=$'\t'
    # collapses consecutive tabs — tab is IFS-whitespace — so an empty field would shift columns).
    sys.stdout.write("\t".join([verdict, e["prio"], area or "-", e["title"].replace("\t", " "), bf]) + "\n")
PY

nbug=0; ndebt=0; namb=0
while IFS=$'\t' read -r verdict prio area title bodyfile; do
    [ -n "$verdict" ] || continue
    [ "$area" = "-" ] && area=""
    case "$verdict" in
        bug)  nbug=$((nbug+1));  labels="bug,$prio,src:code-review"; [ -n "$area" ] && labels="$labels,area:$area"
              echo "  ISSUE   [$prio${area:+, area:$area}] $title" ;;
        debt) ndebt=$((ndebt+1)); echo "  DEBT    [$prio] $title" ;;
        *)    namb=$((namb+1));  echo "  AMBIG   [$prio] $title  (left in bug.md — flag a human)" ;;
    esac
done < "$TMPDIR_M/records.tsv"

echo "migrate-bugs: $nbug → GitHub Issues · $ndebt → debt.md · $namb ambiguous (left in bug.md)"

if [ "$MODE" != "apply" ]; then
    echo "migrate-bugs: DRY-RUN — no Issues created, no files changed. --apply dedups each ISSUE"
    echo "             candidate against open Issues first (e.g. bulkImportFutures → existing #734)."
    exit 0
fi

command -v gh >/dev/null 2>&1 || { echo "migrate-bugs: gh required for --apply" >&2; exit 2; }
REPO="${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || echo alexandrosk0/Smatchet)}"
created=0; deduped=0
while IFS=$'\t' read -r verdict prio area title bodyfile; do
    [ "$verdict" = "bug" ] || continue
    [ "$area" = "-" ] && area=""
    # Dedup-FIRST (the #734 lesson — never re-create a bug an open Issue already covers). Derive a
    # distinctive search term: the first `backtick`'d symbol in the title, else its first 4 words.
    # shellcheck disable=SC2016  # the single-quoted backtick regex is literal, not a subshell
    term="$(printf '%s' "$title" | grep -oE '`[^`]+`' | head -1 | tr -d '`')"
    [ -n "$term" ] || term="$(printf '%s' "$title" | tr -cs 'A-Za-z0-9 ' ' ' | awk '{print $1, $2, $3, $4}')"
    if [ -n "$term" ] && [ -n "$(gh issue list --repo "$REPO" --state open --search "$term" --json number --jq '.[0].number' 2>/dev/null)" ]; then
        existing="$(gh issue list --repo "$REPO" --state open --search "$term" --json number --jq '.[0].number')"
        echo "migrate-bugs: DEDUP — '$title' already covered by #$existing (search: $term); skipping create"
        deduped=$((deduped+1))
        continue
    fi
    labels="bug,$prio,src:code-review"; [ -n "$area" ] && labels="$labels,area:$area"
    if gh issue create --repo "$REPO" --title "bug: $title" --body-file "$bodyfile" --label "$labels" >/dev/null 2>&1; then
        created=$((created+1))
    else
        echo "migrate-bugs: WARN create failed for: $title" >&2
    fi
done < "$TMPDIR_M/records.tsv"
echo "migrate-bugs: APPLIED — created $created Issue(s), $deduped deduped (already tracked) on $REPO. Debt entries + bug.md pruning: review the dry-run list and move/delete by hand (non-destructive by design)."
exit 0
