#!/usr/bin/env bash
# issue-sweep.sh — triage open GitHub Issues per the issue-triage protocol
# (docs/agent-rules/issue-triage.md). Classifies each open Issue by author +
# label state + dedup, emits a verdict, and surfaces the top-priority open bug as
# an [issue-propose] line. Runs in the ship-loop closeout + the issue-janitor.
#
# Verdicts (decision tree): keep / relabel / mirror-then-close / flag-stale /
# flag-relabel. AUTONOMY: --apply may auto-act (relabel / close) ONLY on
# bot-authored Issues; human-authored Issues are report-only (never auto-closed).
#
# Usage:
#   bash agents/scripts/core/issue-sweep.sh             # dry-run (default): verdicts + propose
#   bash agents/scripts/core/issue-sweep.sh --apply     # act on BOT-authored strays only
#
# Env: REPO; BOT_LOGINS (space-sep, default "coderabbitai coderabbitai[bot] app/coderabbitai");
#      ISSUES_JSON (a file of `gh issue list --json ...` output — bats injects a fixture, no gh).
#
# Exit codes: 0 ok · 2 gh missing / bad args.

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" 2>/dev/null || true

MODE="dry-run"
case "${1:-}" in
    --apply) MODE="apply" ;;
    "")      MODE="dry-run" ;;
    *) echo "usage: $0 [--apply]" >&2; exit 2 ;;
esac

BOT_LOGINS="${BOT_LOGINS:-coderabbitai coderabbitai[bot] app/coderabbitai}"
PY=""; for c in python3 python py; do command -v "$c" >/dev/null 2>&1 && { PY="$c"; break; }; done
[ -n "$PY" ] || { echo "issue-sweep: no python" >&2; exit 2; }

# Source of open Issues: a fixture file (bats) or a live `gh issue list`.
if [ -n "${ISSUES_JSON:-}" ]; then
    [ -f "$ISSUES_JSON" ] || { echo "issue-sweep: ISSUES_JSON '$ISSUES_JSON' not found" >&2; exit 2; }
    issues="$(cat "$ISSUES_JSON")"
    REPO="${REPO:-fixture/repo}"
else
    command -v gh >/dev/null 2>&1 || { echo "issue-sweep: gh required (or set ISSUES_JSON)" >&2; exit 2; }
    REPO="${REPO:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || echo alexandrosk0/Smatchet)}"
    issues="$(gh issue list --repo "$REPO" --state open --limit 200 \
        --json number,title,author,labels 2>/dev/null || echo '[]')"
fi

# Python computes the verdict per Issue + the propose line. Emits:
#   ACTION lines:  verdict<TAB>number<TAB>botflag<TAB>title   (botflag = bot|human)
#   one PROPOSE line: PROPOSE<TAB>number<TAB>prio<TAB>title   (or none)
# Issues go via a temp file (argv[2]) — `python -` already consumes stdin for the program,
# so a pipe would be overridden by the heredoc (SC2259).
issues_file="$(mktemp)"; printf '%s' "$issues" > "$issues_file"
records="$("$PY" - "$BOT_LOGINS" "$issues_file" <<'PY'
import json, sys
for s in (sys.stdout, sys.stderr):
    try: s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass
bots = set(sys.argv[1].split())
try:
    issues = json.load(open(sys.argv[2], encoding="utf-8", errors="replace"))
except Exception:
    issues = []
PRIO = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}

def prio_of(labels):
    for l in labels:
        if l in PRIO: return l
    return ""

def verdict(it, bot):
    labels = [l.get("name", "") for l in it.get("labels", [])]
    has_bug = "bug" in labels
    has_prio = any(l in PRIO for l in labels)
    if bot:
        # Bot stray: unlabeled / missing bug+priority → relabel; labelled already → keep.
        if not has_bug or not has_prio:
            return "relabel"
        return "keep"
    # Human-authored: report-only.
    return "keep"

rows = []
best = None  # (prio_rank, number, prio, title)
for it in issues:
    login = (it.get("author") or {}).get("login", "")
    bot = login in bots or ("[bot]" in login) or login.startswith("app/")
    v = verdict(it, bot)
    title = (it.get("title") or "").replace("\t", " ")
    rows.append("%s\t%s\t%s\t%s" % (v, it.get("number", "?"), "bot" if bot else "human", title))
    labels = [l.get("name", "") for l in it.get("labels", [])]
    p = prio_of(labels)
    # propose only among bug-ish open Issues with a P0/P1 (or any if unprioritised bot stray)
    rank = PRIO.get(p, 9)
    if rank <= 1 and (best is None or rank < best[0]):
        best = (rank, it.get("number", "?"), p, title)

out = []
for r in rows: out.append(r)
if best is not None:
    out.append("PROPOSE\t%s\t%s\t%s" % (best[1], best[2], best[3]))
sys.stdout.write("\n".join(out) + ("\n" if out else ""))
PY
)"
rm -f "$issues_file"

acted=0; kept=0; relabeled=0
propose_line=""
while IFS=$'\t' read -r verdict num botflag title; do
    [ -n "$verdict" ] || continue
    if [ "$verdict" = "PROPOSE" ]; then
        # num=number prio=botflag title=title (reused columns)
        propose_line="[issue-propose] #$num $title ($botflag) — elevate? gh issue develop $num"
        continue
    fi
    case "$verdict" in
        relabel)
            relabeled=$((relabeled+1))
            echo "  RELABEL #$num [$botflag] $title"
            if [ "$MODE" = "apply" ] && [ "$botflag" = "bot" ]; then
                gh issue edit "$num" --repo "$REPO" --add-label "bug" >/dev/null 2>&1 \
                    && acted=$((acted+1)) || echo "issue-sweep: WARN relabel #$num failed" >&2
            fi ;;
        keep) kept=$((kept+1)); echo "  KEEP    #$num [$botflag] $title" ;;
        *)    echo "  $verdict #$num [$botflag] $title" ;;
    esac
done <<< "$records"

echo "issue-sweep: $relabeled relabel · $kept keep (repo $REPO) — $([ "$MODE" = apply ] && echo "applied $acted bot-only action(s)" || echo "dry-run; --apply acts on bot-authored only")"
[ -n "$propose_line" ] && echo "$propose_line"
exit 0
