#!/usr/bin/env bash
# scripts/dev/agent-eval-run.sh — drive ONE agent eval case, write a result JSON.
#
# Companion to scripts/dev/agent-eval-score.py, one level up the stack from
# scripts/dev/perf-run.sh (whose CLI shape + output discipline this mirrors:
# stale-file wipe, last-line-is-the-path). Plan:
# docs/plans/shipped/subagent-eval-harness.md § Approach step 5.
#
# Runs ONE agent for ONE case across N trials and writes a result JSON per
# docs/agent-eval/result-schema.json. Two non-negotiable seams:
#
#   * BEFORE/AFTER is explicit via --prompt-root=<path>: the runner reads the
#     agent-prompt tree from that path. A prompt PR is scored by running the
#     case set once with --prompt-root=<base worktree> and once with
#     --prompt-root=<head worktree>, then diffing the two result JSONs with
#     agent-eval-score.py. No implicit "current checkout".
#
#   * HARNESS ADAPTER: `claude -p` print mode is ONE adapter behind a seam, not
#     the canonical interface. --fake-runner emits a canned result so the runner
#     is bats-testable with NO live tokens. The case / result / scoring formats
#     assume no specific harness.
#
# Harness-agnostic core — pure bash + python3 (stdlib) for JSON assembly.
#
# Usage:
#   bash scripts/dev/agent-eval-run.sh <case.json> --prompt-root=<path> \
#        [--trials=N] [--out=<path>] [--harness=<name>] [--exe=<path>] [--fake-runner]
#
# Examples:
#   bash scripts/dev/agent-eval-run.sh tests/agent-eval/code-review/cr-dpapi-secret-loss.json \
#        --prompt-root=. --fake-runner
#   bash scripts/dev/agent-eval-run.sh <case> --prompt-root=/path/to/head-worktree --trials=3
#
# Exit codes:
#   0 — ran cleanly, result JSON written to the reported path
#   1 — run failed (adapter error, JSON assembly failed, output missing)
#   2 — bad usage (missing case, missing --prompt-root, unknown flag)

set -euo pipefail

# -- dependency preflight ----------------------------------------------------
# Each candidate must RESOLVE and RUN. On Windows `python3` resolves to the
# Microsoft Store App Execution Alias stub -- on PATH, but it prints an "install
# from the Microsoft Store" banner and exits non-zero -- so a resolve-only chain
# picks the stub over the working `python` behind it and every eval run dies on
# the first interpreter call.
PY=""
for _c in python3 python py; do
    if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then PY="$_c"; break; fi
done
if [ -z "$PY" ]; then
    echo "agent-eval-run: a working python3 (or python) required" >&2
    exit 2
fi

usage() {
    cat >&2 <<'USAGE'
Usage: bash scripts/dev/agent-eval-run.sh <case.json> --prompt-root=<path> \
       [--trials=N] [--out=<path>] [--harness=<name>] [--exe=<path>] [--fake-runner]

  <case.json>        Golden case (docs/agent-eval/case-schema.json).
  --prompt-root=PATH Agent-prompt tree to run against (the before/after seam). Required.
  --trials=N         Trials to run. Default 1.
  --out=PATH         Write result JSON here. Default:
                     build/agent-eval-runs/<caseId>-<harness>-<timestamp>.json
  --harness=NAME     Adapter: 'claude-print' (default) or 'fake-runner'.
  --exe=PATH         Harness binary for claude-print. Default: claude
  --fake-runner      Shorthand for --harness=fake-runner (canned result, no tokens).
USAGE
    exit 2
}

# -- args --------------------------------------------------------------------
CASE=""
PROMPT_ROOT=""
TRIALS=1
OUT_PATH=""
HARNESS="claude-print"
EXE="${SMATCHET_AGENT_EVAL_EXE:-claude}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prompt-root=*) PROMPT_ROOT="${1#--prompt-root=}"; shift ;;
        --prompt-root)   [ "$#" -ge 2 ] || { echo "agent-eval-run: --prompt-root requires a value" >&2; usage; }
                         PROMPT_ROOT="$2"; shift 2 ;;
        --trials=*)      TRIALS="${1#--trials=}"; shift ;;
        --trials)        [ "$#" -ge 2 ] || { echo "agent-eval-run: --trials requires a value" >&2; usage; }
                         TRIALS="$2"; shift 2 ;;
        --out=*)         OUT_PATH="${1#--out=}"; shift ;;
        --out)           [ "$#" -ge 2 ] || { echo "agent-eval-run: --out requires a value" >&2; usage; }
                         OUT_PATH="$2"; shift 2 ;;
        --harness=*)     HARNESS="${1#--harness=}"; shift ;;
        --harness)       [ "$#" -ge 2 ] || { echo "agent-eval-run: --harness requires a value" >&2; usage; }
                         HARNESS="$2"; shift 2 ;;
        --exe=*)         EXE="${1#--exe=}"; shift ;;
        --exe)           [ "$#" -ge 2 ] || { echo "agent-eval-run: --exe requires a value" >&2; usage; }
                         EXE="$2"; shift 2 ;;
        --fake-runner)   HARNESS="fake-runner"; shift ;;
        -h|--help)       usage ;;
        --*)             echo "unknown flag: $1" >&2; usage ;;
        *)
            if [[ -z "$CASE" ]]; then
                CASE="$1"
            else
                echo "unexpected positional arg: $1" >&2; usage
            fi
            shift
            ;;
    esac
done

if [[ -z "$CASE" ]]; then
    echo "missing <case.json>" >&2
    usage
fi
if [[ ! -f "$CASE" ]]; then
    echo "FAIL: case file not found: $CASE" >&2
    exit 2
fi
if [[ -z "$PROMPT_ROOT" ]]; then
    echo "missing --prompt-root" >&2
    usage
fi
if [[ ! -d "$PROMPT_ROOT" ]]; then
    echo "FAIL: --prompt-root is not a directory: $PROMPT_ROOT" >&2
    exit 2
fi
case "$TRIALS" in
    ''|*[!0-9]*) echo "FAIL: --trials must be a positive integer: $TRIALS" >&2; exit 2 ;;
esac
if [[ "$TRIALS" -lt 1 ]]; then
    echo "FAIL: --trials must be >= 1" >&2
    exit 2
fi

# -- paths -------------------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

CASE_ID="$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1],encoding="utf-8"))["caseId"])' "$CASE")"

if [[ -z "$OUT_PATH" ]]; then
    TS="$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p "$REPO_ROOT/build/agent-eval-runs"
    OUT_PATH="$REPO_ROOT/build/agent-eval-runs/${CASE_ID}-${HARNESS}-${TS}.json"
fi

# Wipe any stale result at the destination — closes the WaitForFile-returns-
# stale-data footgun (same discipline as perf-run.sh).
rm -f "$OUT_PATH"

# Scratch dir for per-trial adapter outputs.
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# -- adapter -----------------------------------------------------------------
# Each adapter writes ONE trial's raw agent output to $1. The findings are
# parsed from that text by the python assembler below; the case / result format
# is harness-agnostic, so a new harness is just a new branch here.
run_trial() {
    local trial_index="$1" out_file="$2"
    case "$HARNESS" in
        fake-runner)
            # Canned, deterministic — no tokens. Findings are synthesised from
            # the case's referenceOutcome by the assembler in fake mode, so this
            # output is informational only.
            printf 'FAKE RUN for %s (trial %s)\n' "$CASE_ID" "$trial_index" > "$out_file"
            ;;
        claude-print)
            command -v "$EXE" >/dev/null 2>&1 || { echo "FAIL: harness exe not found: $EXE" >&2; return 1; }
            local packet
            packet="$("$PY" -c 'import json,sys; print(json.load(open(sys.argv[1],encoding="utf-8"))["delegationPacket"])' "$CASE")"
            # `claude -p` print mode: ONE adapter behind the seam. The prompt-root
            # is exported so a harness wrapper can point its agent-prompt loader
            # at the before/after tree.
            SMATCHET_AGENT_PROMPT_ROOT="$PROMPT_ROOT" "$EXE" -p "$packet" > "$out_file" 2>"$WORKDIR/err-$trial_index" || {
                echo "FAIL: harness '$EXE' exited non-zero on trial $trial_index" >&2
                cat "$WORKDIR/err-$trial_index" >&2 || true
                return 1
            }
            ;;
        *)
            echo "FAIL: unknown --harness: $HARNESS" >&2
            return 1
            ;;
    esac
}

OUTPUT_FILES=()
i=0
while [[ "$i" -lt "$TRIALS" ]]; do
    of="$WORKDIR/trial-$i.txt"
    run_trial "$i" "$of"
    OUTPUT_FILES+=("$of")
    i=$((i + 1))
done

# -- assemble result JSON ----------------------------------------------------
# Fake mode derives findings from the case's expectedFindings (deterministic).
# Real mode parses each trial's raw output: one finding per bullet under the
# code-review severity headings (## Critical / High / Medium / Low / Nits),
# falling back to a per-line file:line citation scan only when no severity
# heading is present. Neither parser counts citations in the contract's
# evidence sections (## Verified clean, ## Outcome, ## Self-improvement,
# ## Session context append). runMeta.parserByTrial records which parser ran.
MODE="real"
if [[ "$HARNESS" == "fake-runner" ]]; then MODE="fake"; fi

AE_CASE="$CASE" AE_OUT="$OUT_PATH" AE_PROMPT_ROOT="$PROMPT_ROOT" \
AE_HARNESS="$HARNESS" AE_TRIALS="$TRIALS" AE_MODE="$MODE" \
"$PY" - "${OUTPUT_FILES[@]}" <<'PYEOF'
import json, os, re, sys

case = json.load(open(os.environ["AE_CASE"], encoding="utf-8"))
mode = os.environ["AE_MODE"]
trials_n = int(os.environ["AE_TRIALS"])
ref = case.get("referenceOutcome", {}) or {}

def fake_findings():
    out = []
    for f in ref.get("expectedFindings", []) or []:
        out.append({
            "file": f.get("file"),
            "line": f.get("line") if isinstance(f.get("line"), int) else 1,
            "severity": f.get("severity", "info"),
            "summary": f.get("summary", ""),
        })
    return out

CITE = re.compile(r'([A-Za-z0-9_][A-Za-z0-9_./-]*\.[A-Za-z0-9]+):(\d+)')
SUMMARY_CAP = 200

# -- headings parser ---------------------------------------------------------
# The code-review agent's output contract (agent-layer/agents/core/code-review.md
# § Output format): one finding per bullet under `## Critical` / `## High` /
# `## Medium` / `## Low / Nits`. Every other section (`## Verified clean`,
# `## Outcome: ...`, `## Self-improvement`, ...) cites evidence, not findings,
# and is skipped. Keys are normalised titles (see _heading_key).
SEVERITY_HEADINGS = {
    "critical": "critical",
    "high": "high",
    "medium": "medium",
    "low / nits": "low",
    "low": "low",
    "nits": "low",
    "info": "info",
}
# The contract's evidence sections. Their citations are never findings, in
# either parser: a clean review is "no findings" + `## Verified clean` +
# `## Outcome:` with no severity heading, and its Verified-clean citations
# must not reach the citation scan.
CONTRACT_SECTION = re.compile(
    r'^(?:verified clean|outcome\b.*|session context append|self-improvement)$')
HEADING = re.compile(r'^ {0,3}#{1,6}[ \t]+(.*?)(?:[ \t]+#+)?[ \t]*$')
FENCE = re.compile(r'^ {0,3}(`{3,}|~{3,})')
BULLET = re.compile(r'^ *(?:[-*+]|\d{1,9}[.)])[ \t]+(\S.*)$')
THEMATIC_BREAK = re.compile(r'^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$')
# A bullet whose leading clause only says its section is empty is not a
# finding; counting it would break finding_count on a clean (0-finding) case.
# The marker is a closed phrase, matched on the bullet's first line whether or
# not it cites code, and must be followed by end of line or a separator:
#   head:  "no [qualifiers] findings|issues|bugs|problems|concerns" (whole
#          word, so "No issue-type check" is prose) or "none|nothing|n/a";
#   tail:  optional verb "identified|found|noted|observed|reported|flagged"
#          or "to report|flag|note", then optional scope "at|in|for this|the
#          severity|category|section|...|diff|change|patch|PR|hunk|code" or
#          "that meet(s) the bar".
# So "- No issues identified.", "- No critical or high findings.", "- None at
# this severity", "- No issues found in this diff.", "- None — a.h:410 is
# safe" and "- No findings (checked a.cpp:12)" are markers, while "- No issue
# key is validated before a.cpp:210", "- None of the callers holds mLock" and
# "- Nothing releases the handle" are findings. "issue" is a domain noun here
# (tracker issues), hence the closed tail.
_EMPTY_QUALIFIER = (r'(?:critical|high|medium|low|nits?|info|other|further|new|additional'
                    r'|genuine|correctness|real|actionable|blocking'
                    r'|(?:critical|high|medium|low)-(?:severity|priority))')
NONE_MARKER = re.compile(
    r'^[\s*_`(\[]*'
    r'(?:no(?:\s+' + _EMPTY_QUALIFIER +
    r'(?:\s*(?:,|/|\bor\b|\band\b)\s*' + _EMPTY_QUALIFIER + r')*)?'
    r'\s+(?:findings?|issues?|bugs?|problems?|concerns?)(?![\w-])'
    r'|none|nothing|n/a)'
    r'(?:\s+(?:identified|found|noted|observed|reported|flagged'
    r'|to\s+(?:report|flag|note)))?'
    r'(?:\s+(?:(?:at|in|for)\s+(?:this|the)\s+(?:severity|category|section|level|tier'
    r'|diff|change|changes|changeset|patch|pr|hunk|code|changed\s+code)'
    r'|that\s+meets?\s+the\s+bar))?'
    r'[\s*_`)\]]*(?:$|[.:;,!(—–-])', re.IGNORECASE)

def _heading_key(title):
    """Lowercased title without emphasis, a trailing count `(2)` or a dash
    suffix, with `/` spacing and runs of whitespace normalised."""
    t = re.sub(r'[*_`]', '', title.strip().lower())
    t = re.sub(r'\s*\([^)]*\)\s*$', '', t)
    t = re.sub(r'\s+[—–-]\s.*$', '', t)
    t = re.sub(r'\s*/\s*', ' / ', t)
    return re.sub(r'\s+', ' ', t).strip().rstrip(':').strip()

def _indent(line):
    return len(line) - len(line.lstrip(' '))

def _finding_from_bullet(severity, parts):
    if NONE_MARKER.match(parts[0]):
        return None
    text = re.sub(r'\s+', ' ', ' '.join(parts)).strip()
    f = {"severity": severity, "summary": text[:SUMMARY_CAP] or "parsed from output"}
    m = CITE.search(text)
    if m:
        f["file"] = m.group(1)
        f["line"] = int(m.group(2))
    return f

def parse_heading_findings(text):
    """Returns (saw_severity_heading, findings, non_evidence_lines).

    A bullet starts at an indent no deeper than the open bullet's; deeper
    lines (wrapped text, nested bullets, indented fences) continue it. A blank
    line keeps the bullet open; an unindented non-bullet line, a thematic
    break or a heading closes it. A `#` line indented to the open bullet's
    content column is wrapped bullet text, not a heading. Fenced code is
    opaque: a `## High` inside a fence is not a heading.

    non_evidence_lines are the lines outside the contract's evidence sections,
    for the citation fallback.
    """
    saw = False
    severity = None          # severity of the current section, None = not a severity section
    evidence = False         # inside a CONTRACT_SECTION
    cur = None               # (indent, content column, [text parts]) of the open bullet
    fence = None             # (char, length) of the open fence
    findings = []
    free = []

    def close():
        nonlocal cur
        if cur is not None and severity is not None:
            f = _finding_from_bullet(severity, cur[2])
            if f is not None:
                findings.append(f)
        cur = None

    for line in text.expandtabs(4).splitlines():
        stripped = line.strip()
        if fence is not None:
            m = FENCE.match(line)
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= fence[1] \
                    and stripped == m.group(1):
                fence = None
            elif cur is not None and stripped:
                cur[2].append(stripped)
            if not evidence:
                free.append(line)
            continue
        if not stripped:
            continue
        ind = _indent(line)
        hm = HEADING.match(line)
        if hm and (cur is None or ind < cur[1]):
            close()
            key = _heading_key(hm.group(1))
            severity = SEVERITY_HEADINGS.get(key)
            evidence = severity is None and CONTRACT_SECTION.match(key) is not None
            saw = saw or severity is not None
            if not evidence:
                free.append(line)    # "### 1. a.cpp:374 — ..." is a finding for the fallback
            continue
        if not evidence:
            free.append(line)
        continues = cur is not None and ind > cur[0]
        fm = FENCE.match(line)
        if fm:
            fence = (fm.group(1)[0], len(fm.group(1)))
            if not continues:
                close()
            continue
        if not continues and THEMATIC_BREAK.match(line):
            close()
            continue
        bm = BULLET.match(line)
        if bm and not continues:
            close()
            cur = (ind, bm.start(1), [bm.group(1)])
        elif continues:
            cur[2].append(stripped)
        else:
            close()          # unindented prose ends the bullet; not a finding
    close()
    return saw, findings, free

# -- citations parser (fallback) ---------------------------------------------
# For output with no severity heading: one info finding per line citing
# file:line, outside the contract's evidence sections. Coarse (it counts
# citing lines, not findings), so it runs only when the headings parser has
# nothing to key on.
def parse_citation_findings(lines):
    out = []
    for line in lines:
        m = CITE.search(line)
        if not m:
            continue
        out.append({
            "file": m.group(1),
            "line": int(m.group(2)),
            "severity": "info",
            "summary": line.strip()[:SUMMARY_CAP] or "parsed from output",
        })
    return out

def parse_findings(text):
    """Returns (parser_name, findings)."""
    saw, findings, free = parse_heading_findings(text)
    if saw:
        return "headings", findings
    return "citations", parse_citation_findings(free)

trials = []
parsers = []
if mode == "fake":
    ff = fake_findings()
    for i in range(trials_n):
        trials.append({
            "index": i,
            "output": "FAKE RUN for %s (trial %d)" % (case["caseId"], i),
            "findings": ff,
        })
        parsers.append("reference")
else:
    for i, path in enumerate(sys.argv[1:]):
        text = open(path, encoding="utf-8").read()
        parser, findings = parse_findings(text)
        trials.append({"index": i, "output": text, "findings": findings})
        parsers.append(parser)

result = {
    "schemaVersion": 1,
    "caseId": case["caseId"],
    "agent": case["agent"],
    "promptRoot": os.environ["AE_PROMPT_ROOT"],
    "harness": os.environ["AE_HARNESS"],
    # parserByTrial[i] names how trials[i].findings were produced: "headings"
    # (code-review severity sections), "citations" (fallback scan) or
    # "reference" (fake mode, from the case's expectedFindings).
    "runMeta": {"trialsRequested": trials_n, "parserByTrial": parsers},
    "case": case,
    "trials": trials,
}
with open(os.environ["AE_OUT"], "w", encoding="utf-8") as f:
    json.dump(result, f, indent=2)
PYEOF

if [[ ! -f "$OUT_PATH" ]]; then
    echo "FAIL: result assembly reported success but $OUT_PATH does not exist." >&2
    exit 1
fi

echo "[agent-eval-run] case=$CASE_ID harness=$HARNESS trials=$TRIALS prompt-root=$PROMPT_ROOT"
echo "[agent-eval-run] done. result: $OUT_PATH"
# Print ONLY the path on the last line so callers can RESULT=$(agent-eval-run.sh ...).
echo "$OUT_PATH"
