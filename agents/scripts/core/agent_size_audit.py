#!/usr/bin/env python3
"""Agent-prompt / AGENTS.md size gate for the reduce-agent-prompt-bloat plan (Slice 0).

The first-party C++ tree is size-gated (file < 67 KB, function_size_audit.py); the agentic
docs that load every session (AGENTS.md) or on every delegation (agents/core, agents/project)
have had NO size guardrail, so prompt bloat regrows under editing pressure. This is the missing
keystone — a delta + grandfather line-budget over the agentic docs, the exact shape of
function_size_audit.py but with the FILE (not a parsed function) as the unit.

Three file classes, three budgets (maintainer decision; see AGENTS.md § Project rules):

  agent-prompt   agents/core/*.md, agents/project/*.md   hard 250 / soft 150   BLOCKS
  contract       AGENTS.md (the every-session contract)   hard 150 / soft 120   BLOCKS
  sink           docs/agent-rules/*.md + agents/_shared/skills/**/*.md   soft 400 only   WARN

The `sink` class is soft-warn-only because those files are where AGENTS.md detail and agent
procedure-bodies LAND (the extraction targets) — a hard cap there would fight the extraction it
is meant to receive. A monstrous rule-doc / skill gets split later, flagged by the warn, never
blocked. Skills get the SAME soft-warn sink treatment as docs/agent-rules (symmetric sinks);
the agent-prompt glob is deliberately the two prompt subdirs ONLY (agents/core, agents/project)
— NOT agents/**, which would wrongly cap agents/README.md and the skill sinks.

Budgets are sourced from project.config.json § agents (size_budget_lines / size_warn_lines /
contract_budget_lines / contract_warn_lines / sink_warn_lines); the constants below are the
fallback + the value --selftest asserts the config still matches (drift guard).

Delta semantics (grandfathering): a file is keyed by (rule, path). The current over-budget
whales (debug-detective, git-janitor, test-author, AGENTS.md) live in the base set, so they
never fire; a file fires only when it is brand-new over its cap or has just crossed it. A
grandfathered 733-line agent shrinking to 300 stays grandfathered (still over 250, but it was
over at base too) — so the whales can be slimmed without the gate fighting it; only REGROWTH
past the snapshot, or a NEW over-cap file, fails. An HTML-comment line anywhere in the file
reading `<!-- SMATCHET_DEVIATION(rule=agent-too-long; ...) -->` suppresses it (file-unit gate,
so the marker is file-scoped, not line-adjacent like the C++ rules). The `<!--` requirement is
load-bearing: everything this gate scopes is markdown, and AGENTS.md *documents* the escape
token in backtick prose — a bare-substring match let that prose exempt AGENTS.md from its own
cap (found 2026-07-13: the file crossed 150 lines in #1764 with no gate fire).

Modes mirror function_size_audit.py:

  agent_size_audit.py                  # human report of all current over-budget files
  agent_size_audit.py --list           # one `rule<TAB>path<TAB>NN lines (cap MM)` per violation
  agent_size_audit.py --baseline-md     # deterministic markdown grandfather snapshot
  agent_size_audit.py --diff <ref>      # DELTA gate: emit hard-cap violations NEW or just-crossed
                                        #   vs the merge-base of <ref>, PLUS advisory
                                        #   `[agent-size] WARN ...` lines (non-failing)
  agent_size_audit.py --scan-file <p>   # git-free single-file classify+measure (bats harness)
  agent_size_audit.py --selftest        # assert budgets match project.config.json + AGENTS.md

Exit contract (so test-lint-rules.sh / CI can fail CLOSED): 0 = clean, 1 = violations found
(printed to stdout), >=2 = infra error (git / IO).

See docs/plans/shipped/reduce-agent-prompt-bloat.md § Slice 0.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# --- budgets (fallback + --selftest drift anchor; authoritative values in project.config.json) -
PROMPT_HARD = 250        # agents/core, agents/project — hard cap (BLOCKS)
PROMPT_SOFT = 150        # agent-prompt soft warn
CONTRACT_HARD = 150      # AGENTS.md — hard cap (BLOCKS)
CONTRACT_SOFT = 120      # AGENTS.md soft warn
SINK_SOFT = 400          # docs/agent-rules + skills — soft warn only (NEVER blocks)

RULE = "agent-too-long"

# --- file-class scopes (POSIX paths; the agent-prompt class is the two prompt subdirs ONLY) ----
PROMPT_RE = re.compile(r"^agents/(core|project)/[^/]+\.md$")
RULE_DOC_RE = re.compile(r"^docs/agent-rules/[^/]+\.md$")
SKILL_RE = re.compile(r"^agents/_shared/skills/.+\.md$")

# git ls-files / ls-tree roots to enumerate (cheap superset; classify() is the real filter).
SCAN_ROOTS = ("AGENTS.md", "agents", "docs/agent-rules")


class Cls(object):
    __slots__ = ("name", "hard", "soft", "blocks")

    def __init__(self, name, hard, soft, blocks):
        self.name = name
        self.hard = hard          # None for soft-warn-only classes
        self.soft = soft
        self.blocks = blocks


def _budgets():
    """Return the five budgets, preferring project.config.json § agents, else the constants."""
    hard_p, soft_p = PROMPT_HARD, PROMPT_SOFT
    hard_c, soft_c = CONTRACT_HARD, CONTRACT_SOFT
    soft_s = SINK_SOFT
    repo = _repo_root()
    cfg_path = os.path.join(repo, "project.config.json")
    try:
        with open(cfg_path, "r", encoding="utf-8", errors="replace") as fh:
            ag = json.load(fh).get("agents", {})
        hard_p = int(ag.get("size_budget_lines", hard_p))
        soft_p = int(ag.get("size_warn_lines", soft_p))
        hard_c = int(ag.get("contract_budget_lines", hard_c))
        soft_c = int(ag.get("contract_warn_lines", soft_c))
        soft_s = int(ag.get("sink_warn_lines", soft_s))
    except (OSError, ValueError, KeyError, TypeError):
        pass  # config missing/malformed — fall back to constants (selftest catches real drift)
    return hard_p, soft_p, hard_c, soft_c, soft_s


def classify(path):
    """Return a Cls for `path`, or None if the file is out of scope. AGENTS.md (contract) and the
    two agent-prompt subdirs BLOCK; docs/agent-rules + skills are soft-warn-only sinks."""
    p = path.replace("\\", "/")
    hard_p, soft_p, hard_c, soft_c, soft_s = _budgets()
    if p == "AGENTS.md":
        return Cls("contract", hard_c, soft_c, True)
    if PROMPT_RE.match(p):
        return Cls("agent-prompt", hard_p, soft_p, True)
    if RULE_DOC_RE.match(p):
        return Cls("rule-doc", None, soft_s, False)
    if SKILL_RE.match(p):
        return Cls("skill", None, soft_s, False)
    return None


def count_lines(text):
    """Line count matching `wc -l` when the file ends in a newline, and counting a final
    unterminated line too (so a missing trailing newline doesn't undercount by one)."""
    if not text:
        return 0
    n = text.count("\n")
    return n if text.endswith("\n") else n + 1


# --- git plumbing (UTF-8 forced: agent prompts carry non-ASCII — em dashes, smart quotes) ------

def _git(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if p.returncode != 0:
        raise RuntimeError("git %s failed (%d): %s" % (" ".join(args), p.returncode, p.stderr.strip()))
    return p.stdout


def _git_ok(args):
    p = subprocess.run(["git"] + args, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.stdout if p.returncode == 0 else ""


def _repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))  # core -> scripts -> agents -> root


def list_head_files():
    out = _git(["ls-files"] + list(SCAN_ROOTS))
    return sorted({f for f in out.splitlines() if classify(f) is not None})


def list_ref_files(ref):
    out = _git(["ls-tree", "-r", "--name-only", ref])
    return sorted({f for f in out.splitlines() if classify(f) is not None})


def _read_head(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# --- scan: hard violations + soft warnings -----------------------------------

def scan_head():
    """{(rule, path): (lines, hard_cap, class_name)} for over-HARD-cap blocking files in the tree."""
    out = {}
    for f in list_head_files():
        cls = classify(f)
        if not (cls and cls.blocks and cls.hard is not None):
            continue
        lines = count_lines(_read_head(f))
        if lines > cls.hard:
            out[(RULE, f)] = (lines, cls.hard, cls.name)
    return out


def scan_ref(ref):
    """Set of (rule, path) keys over the HARD cap at <ref> (the grandfather base set)."""
    keys = set()
    for f in list_ref_files(ref):
        cls = classify(f)
        if not (cls and cls.blocks and cls.hard is not None):
            continue
        lines = count_lines(_git_ok(["show", "%s:%s" % (ref, f)]))
        if lines > cls.hard:
            keys.add((RULE, f))
    return keys


def scan_head_warnings():
    """{path: (lines, soft_cap, class_name)} of files over their SOFT cap but NOT a hard violation
    (advisory; never gates). Includes the soft-warn-only sink classes."""
    out = {}
    for f in list_head_files():
        cls = classify(f)
        if not cls:
            continue
        lines = count_lines(_read_head(f))
        if cls.blocks and cls.hard is not None and lines > cls.hard:
            continue  # already a hard violation — don't double-report
        if lines > cls.soft:
            out[f] = (lines, cls.soft, cls.name)
    return out


def scan_ref_warnings(ref):
    """Set of paths over their SOFT cap (and not a hard violation) at <ref>."""
    keys = set()
    for f in list_ref_files(ref):
        cls = classify(f)
        if not cls:
            continue
        lines = count_lines(_git_ok(["show", "%s:%s" % (ref, f)]))
        if cls.blocks and cls.hard is not None and lines > cls.hard:
            continue
        if lines > cls.soft:
            keys.add(f)
    return keys


# --- delta gate ---------------------------------------------------------------

def _merge_base_or_ref(ref):
    p = subprocess.run(["git", "merge-base", ref, "HEAD"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    mb = p.stdout.strip()
    if p.returncode == 0 and mb:
        return mb
    sys.stderr.write("agent_size_audit: WARN: `git merge-base %s HEAD` did not resolve "
                     "(shallow clone?) — falling back to %s tip; delta may false-flag.\n" % (ref, ref))
    return ref


def _deviation_suppresses(text):
    """True if `text` carries a real file-scoped marker: an HTML-comment line with
    `SMATCHET_DEVIATION(... rule=...agent-too-long...)`. Everything this gate scopes is markdown,
    so a real marker is `<!-- SMATCHET_DEVIATION(...) -->`; the token in prose or a backtick code
    span (AGENTS.md documents the escape hatch) must NOT suppress — a bare-substring match here
    let AGENTS.md's own prose exempt it from its own cap."""
    for line in text.split("\n"):
        if "SMATCHET_DEVIATION" not in line:
            continue
        line = re.sub(r"`[^`]*`", "", line)  # backtick code spans are documentation, not markers
        if "<!--" not in line.split("SMATCHET_DEVIATION", 1)[0]:
            continue  # prose mention, not a marker
        m = re.search(r"rule=([A-Za-z0-9_,-]+)", line)
        if m and RULE in [r.strip() for r in m.group(1).split(",")]:
            return True
    return False


def _suppressed(path):
    """File wrapper over `_deviation_suppresses` (file-unit gate, marker is file-scoped)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False
    return _deviation_suppresses(text)


def run_diff(ref):
    base = _merge_base_or_ref(ref)
    head = scan_head()
    base_keys = scan_ref(base)
    violations = []
    for key in sorted(head):
        if key in base_keys:
            continue
        _rule, path = key
        if _suppressed(path):
            continue
        lines, cap, cls = head[key]
        violations.append("%s\t%s\t%d lines (cap %d, %s)" % (RULE, path, lines, cap, cls))
    for v in violations:
        print(v)
    # Advisory soft-warning tier (NEVER affects exit code): new/changed files over the soft cap.
    warn_head = scan_head_warnings()
    warn_base = scan_ref_warnings(base)
    for path in sorted(warn_head):
        if path in warn_base:
            continue
        lines, cap, cls = warn_head[path]
        print("[agent-size] WARN %s — %d lines > %d (%s soft tier; not blocking, trim toward target)"
              % (path, lines, cap, cls), file=sys.stderr)
    return 1 if violations else 0


# --- reporting ----------------------------------------------------------------

def run_scan_file(path):
    cls = classify(path)
    if cls is None:
        print("agent_size_audit: %s is out of scope (not an agent prompt / AGENTS.md / sink)" % path,
              file=sys.stderr)
        return 0
    lines = count_lines(_read_head(path))
    if cls.blocks and cls.hard is not None and lines > cls.hard:
        print("%s\t%s\t%d lines (cap %d, %s)" % (RULE, path, lines, cls.hard, cls.name))
    elif lines > cls.soft:
        print("[agent-size] WARN %s — %d lines > %d (%s soft tier; not blocking)"
              % (path, lines, cls.soft, cls.name), file=sys.stderr)
    return 0


def run_list():
    for (rule, path), (lines, cap, cls) in sorted(scan_head().items()):
        print("%s\t%s\t%d lines (cap %d, %s)" % (rule, path, lines, cap, cls))
    return 0


def run_baseline_md():
    head = scan_head()
    hard_p, soft_p, hard_c, soft_c, soft_s = _budgets()
    rows = []
    for (rule, path), (lines, cap, cls) in sorted(head.items()):
        rows.append("- `%s` · %d lines (cap %d, %s)" % (path, lines, cap, cls))
    print("# Agent-prompt / AGENTS.md size — grandfathered baseline")
    print()
    print("_Auto-generated. Do not hand-edit; run "
          "`bash agents/scripts/project/test-lint-rules.sh --agentsize-baseline` and commit._")
    print("_The gate is a live merge-base delta vs `origin/develop` (agent_size_audit.py --diff); "
          "this file is an informational snapshot, not the gate input._")
    print()
    print("## %s (%d entries, cap %d lines agent-prompt / %d lines AGENTS.md)"
          % (RULE, len(rows), hard_p, hard_c))
    if rows:
        for r in rows:
            print(r)
    else:
        print("- (none)")
    print()
    print("## Totals")
    print("- oversized agent files grandfathered: %d" % len(rows))
    return 0


def run_report():
    head = scan_head()
    warns = scan_head_warnings()
    hard_p, soft_p, hard_c, soft_c, soft_s = _budgets()
    print("## Agent-size audit — agentic docs (current tree)\n")
    print("- agent-prompt: hard %d / soft %d lines (agents/core, agents/project)" % (hard_p, soft_p))
    print("- contract: hard %d / soft %d lines (AGENTS.md)" % (hard_c, soft_c))
    print("- sink (warn only): soft %d lines (docs/agent-rules, agents/_shared/skills)" % soft_s)
    print("- over hard cap (blocking): %d" % len(head))
    print("- over soft cap (advisory): %d" % len(warns))
    print("\n### Over hard cap\n")
    for (rule, path), (lines, cap, cls) in sorted(head.items(), key=lambda kv: kv[1][0], reverse=True):
        print("- `%s` — %d lines (cap %d, %s)" % (path, lines, cap, cls))
    if warns:
        print("\n### Over soft cap (advisory)\n")
        for path, (lines, cap, cls) in sorted(warns.items(), key=lambda kv: kv[1][0], reverse=True):
            print("- `%s` — %d lines > %d (%s)" % (path, lines, cap, cls))
    return 0


def run_selftest():
    """Assert (a) the constants match project.config.json § agents (config is authoritative — drift
    fails), (b) the budgets + rule-id appear in AGENTS.md (the discoverable one-liner), and (c)
    classify() behaves on canonical paths. Exit 0 = in sync, 1 = drift."""
    miss = 0
    repo = _repo_root()
    # (a) config vs constants
    try:
        with open(os.path.join(repo, "project.config.json"), "r", encoding="utf-8", errors="replace") as fh:
            ag = json.load(fh).get("agents", {})
    except (OSError, ValueError) as e:
        print("SELFTEST FAIL: cannot read project.config.json (%s)" % e, file=sys.stderr)
        return 1
    expect = {
        "size_budget_lines": PROMPT_HARD, "size_warn_lines": PROMPT_SOFT,
        "contract_budget_lines": CONTRACT_HARD, "contract_warn_lines": CONTRACT_SOFT,
        "sink_warn_lines": SINK_SOFT,
    }
    for key, val in sorted(expect.items()):
        if ag.get(key) != val:
            print("SELFTEST FAIL: project.config.json agents.%s = %r, script constant = %d"
                  % (key, ag.get(key), val), file=sys.stderr)
            miss = 1
    # (b) AGENTS.md discoverability
    try:
        with open(os.path.join(repo, "AGENTS.md"), "r", encoding="utf-8", errors="replace") as fh:
            doc = fh.read()
    except OSError as e:
        print("SELFTEST FAIL: cannot read AGENTS.md (%s)" % e, file=sys.stderr)
        return 1
    for needle in (str(PROMPT_HARD), str(CONTRACT_HARD), RULE, "agent_size_audit"):
        if needle not in doc:
            print("SELFTEST FAIL: '%s' missing from AGENTS.md (gate not discoverable)" % needle,
                  file=sys.stderr)
            miss = 1
    # (c) classify() behaviour: blocking prompts, blocking contract, non-blocking sinks, out-of-scope
    cases = [
        ("AGENTS.md", "contract", True),
        ("agents/core/debug-detective.md", "agent-prompt", True),
        ("agents/project/grid-engine.md", "agent-prompt", True),
        ("docs/agent-rules/process-rules.md", "rule-doc", False),
        ("agents/_shared/skills/perf-instrument/SKILL.md", "skill", False),
    ]
    for path, name, blocks in cases:
        c = classify(path)
        if c is None or c.name != name or c.blocks != blocks:
            got = "None" if c is None else "%s/blocks=%s" % (c.name, c.blocks)
            print("SELFTEST FAIL: classify(%r) = %s, expected %s/blocks=%s"
                  % (path, got, name, blocks), file=sys.stderr)
            miss = 1
    # selftest: asserts-failure — non-prompt / out-of-tree paths must classify as out-of-scope (None).
    for path in ("agents/README.md", "README.md", "agents/_shared/token-tracking/SKILL.md"):
        # token-tracking is NOT under _shared/skills/, README is not a prompt — all out of scope.
        if path == "agents/_shared/token-tracking/SKILL.md":
            if classify(path) is not None:
                print("SELFTEST FAIL: classify(%r) should be out of scope" % path, file=sys.stderr)
                miss = 1
        elif classify(path) is not None:
            print("SELFTEST FAIL: classify(%r) should be out of scope (not a prompt)" % path,
                  file=sys.stderr)
            miss = 1
    # (d) deviation-marker semantics: only a real HTML-comment marker suppresses; the token in
    # backtick prose (AGENTS.md documents the escape hatch) must not — the prose match silently
    # exempted AGENTS.md from its own cap.
    if _deviation_suppresses("`SMATCHET_DEVIATION(rule=agent-too-long; …)` anywhere escapes"):
        print("SELFTEST FAIL: backtick-prose deviation token must NOT suppress", file=sys.stderr)
        miss = 1
    if _deviation_suppresses("an HTML-comment marker `<!-- SMATCHET_DEVIATION(rule=agent-too-long;"
                             " …) -->` escapes"):
        print("SELFTEST FAIL: backticked full-form example must NOT suppress", file=sys.stderr)
        miss = 1
    if not _deviation_suppresses(
            "<!-- SMATCHET_DEVIATION(rule=agent-too-long; reason=t; owner=t; revisit=never) -->"):
        print("SELFTEST FAIL: HTML-comment deviation marker must suppress", file=sys.stderr)
        miss = 1
    if miss:
        return 1
    print("selftest: agent-size budgets in sync with project.config.json + AGENTS.md")
    return 0


def _utf8_stdio():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    _utf8_stdio()
    ap = argparse.ArgumentParser()
    ap.add_argument("--diff", metavar="REF")
    ap.add_argument("--scan-file", metavar="PATH")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--baseline-md", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    try:
        if args.selftest:
            sys.exit(run_selftest())
        if args.diff:
            sys.exit(run_diff(args.diff))
        if args.scan_file:
            sys.exit(run_scan_file(args.scan_file))
        if args.list:
            sys.exit(run_list())
        if args.baseline_md:
            sys.exit(run_baseline_md())
        sys.exit(run_report())
    except Exception as e:  # never crash-as-clean: surface as infra error (>=2)
        print("agent_size_audit: ERROR: %s" % e, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
