#!/usr/bin/env python3
"""Repo-health dashboard generator — renders tools/repo-health/dashboard.html.

The dashboard is the human-on-the-loop **visibility layer** described in
AI_POLICY.md: one page that replaces reconstructing project state from five
markdown files plus the Actions tab. This script is the "living" half — it
recomputes every metric that is *derivable from the working tree* on each run,
so the numbers never silently go stale as the repo moves.

Two data sources, by design:

  1. COMPUTED (this script, offline, deterministic) — coverage %, audit finding
     totals, fuzz-target count, test-TU count, plan lifecycle counts,
     self-improvement backlog counts, and the governance grant flags. All read
     straight from the tree (docs, config, `git`, and the canonical
     test-backlog-counts.sh), so a plan shipping or a fuzz target landing moves
     the dashboard automatically.

  2. FACTS (facts.json, session-maintained) — the judgment calls a standalone
     script cannot make and the live state only the GitHub MCP tools can fetch:
     the campaign scorecard verdicts, the nightly advisory-lane run statuses,
     and the open-PR gate states. A session (or the nightly triage routine)
     refreshes these via MCP, then reruns this script.

Render is template.html with `{{TOKEN}}` substitution — scalars inline, repeating
blocks (scorecard cards, CI lanes, PR rows, backlog bars) built here as HTML
fragments. The output dashboard.html is a build artifact (git-ignored); publish
it to the fixed Artifact URL recorded in facts.json — see README.md § Republish.

Usage:
    python3 tools/repo-health/generate.py            # -> tools/repo-health/dashboard.html
    python3 tools/repo-health/generate.py --print    # also dump the computed metrics
    python3 tools/repo-health/generate.py --out PATH # write elsewhere

Degrades gracefully: any metric it cannot parse falls back to the value carried
in facts.json (or an em dash), and the run still succeeds — a partial dashboard
beats a crash in an unattended routine.
"""
from __future__ import annotations

import argparse
import html
import json
import re
import subprocess
import sys
from datetime import date, timezone, datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent  # tools/repo-health -> repo root


# --------------------------------------------------------------------------- #
# small helpers
# --------------------------------------------------------------------------- #
def read(rel: str) -> str:
    """Read a repo-relative text file, '' if missing."""
    p = REPO / rel
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def esc(s) -> str:
    return html.escape(str(s), quote=True)


def git(*args: str) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO), *args],
            capture_output=True, text=True, timeout=15,
        )
        return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def pct(n: int, d: int) -> int:
    return round(100 * n / d) if d else 0


# --------------------------------------------------------------------------- #
# computed collectors  (tree -> numbers)
# --------------------------------------------------------------------------- #
def collect_coverage() -> dict:
    """Parse the canonical headline out of TEST_COVERAGE_GAP_MAP.md."""
    doc = read("TEST_COVERAGE_GAP_MAP.md")
    out = {}
    m = re.search(r"Core TUs compiled.*?\*\*\s*([\d,]+)\s*/\s*([\d,]+)\s*\((\d+)%\)", doc)
    if m:
        out["tu_have"], out["tu_total"], out["tu_pct"] = m.group(1), m.group(2), int(m.group(3))
    m = re.search(r"Core src LOC compiled.*?\*\*\s*([\d.]+K?)\s*/\s*([\d.]+K?)\s*\((\d+)%\)", doc)
    if m:
        out["loc_have"], out["loc_total"], out["loc_pct"] = m.group(1), m.group(2), int(m.group(3))
    return out


def collect_audit() -> dict:
    """Parse the finding totals out of CPP_CODE_AUDIT.md's summary line."""
    doc = read("CPP_CODE_AUDIT.md")
    out = {}
    m = re.search(r"\*\*\s*(\d+)\s+findings\s*\*\*.*?High:\s*(\d+).*?Medium:\s*(\d+).*?Low:\s*(\d+)",
                  doc, re.IGNORECASE | re.DOTALL)
    if not m:
        m = re.search(r"(\d+)\s+findings\D+High:\s*(\d+),\s*Medium:\s*(\d+),\s*Low:\s*(\d+)",
                      doc, re.IGNORECASE)
    if m:
        out["total"] = int(m.group(1))
        out["high"] = int(m.group(2))
        out["medium"] = int(m.group(3))
        out["low"] = int(m.group(4))
    return out


def collect_fuzz() -> int:
    return len(list((REPO / "tests" / "fuzz").glob("fuzz_*.cpp")))


def collect_test_tus() -> int:
    return sum(1 for _ in (REPO / "tests").rglob("*.cpp"))


def collect_plans() -> dict:
    """Count real plan files per lifecycle bucket (skip _-prefixed scaffolding)."""
    def n(bucket: str) -> int:
        d = REPO / "docs" / "plans" / bucket
        if not d.is_dir():
            return 0
        return sum(1 for f in d.glob("*.md") if not f.name.startswith("_"))
    active, deferred, shipped = n("active"), n("deferred"), n("shipped")
    total = active + deferred + shipped
    return {
        "active": active, "deferred": deferred, "shipped": shipped, "total": total,
        "ship_rate": pct(shipped, total),
    }


def collect_backlog() -> dict:
    """Per-category self-improvement counts via the canonical --list script."""
    counts: dict[str, int] = {}
    try:
        out = subprocess.run(
            ["bash", str(REPO / "agents/scripts/core/test-backlog-counts.sh"), "--list"],
            capture_output=True, text=True, timeout=30, cwd=str(REPO),
        )
        for line in out.stdout.splitlines():
            m = re.match(r"\s*([a-z]+)\s+(\d+)\s*$", line)
            if m:
                counts[m.group(1)] = int(m.group(2))
    except (OSError, subprocess.SubprocessError):
        pass
    return counts


def collect_governance() -> dict:
    try:
        cfg = json.loads(read("project.config.json"))
        gov = cfg.get("governance", {})
        return {"loop_mode": gov.get("loop_mode"), "auto_merge": gov.get("auto_merge")}
    except (json.JSONDecodeError, ValueError):
        return {}


def collect() -> dict:
    return {
        "coverage": collect_coverage(),
        "audit": collect_audit(),
        "fuzz": collect_fuzz(),
        "test_tus": collect_test_tus(),
        "plans": collect_plans(),
        "backlog": collect_backlog(),
        "governance": collect_governance(),
        "develop_sha": git("rev-parse", "--short", "origin/develop") or git("rev-parse", "--short", "HEAD"),
    }


# --------------------------------------------------------------------------- #
# fragment builders  (data -> HTML)
# --------------------------------------------------------------------------- #
STATUS = {  # facts verdict -> (pill class, stripe class, dot label)
    "landed":      ("p-ok", "st-ok", "Landed"),
    "partial":     ("p-warn", "st-warn", "Half shipped"),
    "not-started": ("p-idle", "st-idle", "Not started"),
}


def build_scorecard(items: list) -> tuple[str, str]:
    cards, tally = [], {"landed": 0, "partial": 0, "not-started": 0}
    for i, it in enumerate(items, 1):
        st = it.get("status", "not-started")
        tally[st] = tally.get(st, 0) + 1
        pill, stripe, label = STATUS.get(st, STATUS["not-started"])
        delay = f"d{min(3, (i + 1) // 2)}"
        cards.append(
            f'''      <article class="card item {stripe} reveal {delay}">
        <span class="num mono">ITEM {i:02d}</span>
        <h3>{esc(it.get("title",""))}</h3>
        <p>{it.get("evidence","")}</p>
        <span class="pill {pill}"><span class="dot"></span>{esc(it.get("label", label))}</span>
      </article>''')
    summary = f'{tally["landed"]} landed · {tally["partial"]} partial · {tally["not-started"]} open'
    return "\n".join(cards), summary


def build_gov_chips(gov: dict, facts: dict) -> str:
    chips = []
    lm = gov.get("loop_mode") or facts.get("governance", {}).get("loop_mode", "?")
    am = gov.get("auto_merge") or facts.get("governance", {}).get("auto_merge", "?")
    for text in (f"loop_mode: {lm}", f"auto_merge: {am}", "block‑on‑any‑red"):
        chips.append(f'<span class="chip on"><span class="dot"></span>{esc(text)}</span>')
    return "\n        ".join(chips)


def build_lanes(lanes: list) -> str:
    rows = []
    for ln in lanes:
        ok = ln.get("status") == "pass"
        dot = "ok" if ok else ("idle" if ln.get("status") in (None, "weekly", "pending") else "crit")
        stat = ln.get("status", "—")
        stat_cls = "ok" if ok else "idle"
        rows.append(
            f'''            <div class="lane">
              <span class="sdot {dot}"></span>
              <span class="l-name">{esc(ln.get("name",""))} <small>{esc(ln.get("detail",""))}</small></span>
              <span class="l-stat {stat_cls}">{esc(stat)}</span>
              <span class="l-when">{esc(ln.get("when",""))}</span>
            </div>''')
    return "\n".join(rows)


def build_prs(prs: list) -> str:
    rows = []
    for pr in prs:
        pill = STATUS.get({"ready": "landed", "green": "landed", "draft": "not-started"}
                          .get(pr.get("state"), "partial"), STATUS["partial"])[0]
        rows.append(
            f'''            <div class="pr">
              <span class="id">#{esc(pr.get("id",""))}</span>
              <span class="desc">
                <span class="t">{esc(pr.get("title",""))}</span>
                <span class="m">{esc(pr.get("meta",""))}</span>
              </span>
              <span class="pill {pill}"><span class="dot"></span>{esc(pr.get("label",""))}</span>
            </div>''')
    return "\n".join(rows)


def build_gov_list(items: list) -> str:
    rows = []
    for g in items:
        rows.append(
            f'''            <div class="gv">
              <span class="ic">{esc(g.get("icon",""))}</span>
              <span class="g-body">
                <span class="g-t">{esc(g.get("title",""))}</span>
                <span class="g-d">{g.get("detail","")}</span>
              </span>
            </div>''')
    return "\n".join(rows)


CAT_ACCENT = {"tooling", "process", "infra", "test", "debt"}


def build_backlog(counts: dict) -> str:
    cats = [(k, counts[k]) for k in
            ("tooling", "process", "infra", "test", "debt", "bug", "security", "external")
            if k in counts]
    if not cats:
        return '<div class="cat"><span class="cl">—</span></div>'
    top = max(v for _, v in cats) or 1
    rows = []
    for name, v in cats:
        w = max(2, round(100 * v / top))
        color = "var(--accent)" if name in CAT_ACCENT and v >= 10 else "var(--idle)"
        rows.append(
            f'''          <div class="cat"><span class="cl">{esc(name)}</span>'''
            f'''<div class="track"><div class="fill" style="width:{w}%;background:{color}"></div></div>'''
            f'''<span class="amt">{v}</span></div>''')
    return "\n".join(rows)


def build_plan_stack(p: dict) -> str:
    t = p["total"] or 1
    a, d, s = (100 * p["active"] / t, 100 * p["deferred"] / t, 100 * p["shipped"] / t)
    return (
        f'          <span style="width:{a:.1f}%;background:var(--accent)" title="active"></span>\n'
        f'          <span style="width:{d:.1f}%;background:var(--idle)" title="deferred"></span>\n'
        f'          <span style="width:{s:.1f}%;background:var(--ok)">{p["shipped"]}</span>')


# --------------------------------------------------------------------------- #
# render
# --------------------------------------------------------------------------- #
def render(data: dict, facts: dict) -> str:
    tpl = (HERE / "template.html").read_text(encoding="utf-8")

    cov, aud, plans, bl = data["coverage"], data["audit"], data["plans"], data["backlog"]
    resolved = facts.get("audit_resolved", {})
    fixed = resolved.get("fixed", "—")
    accepted = resolved.get("accepted", 0)
    low = aud.get("low", 0)
    low_fixed = low - accepted if isinstance(low, int) else "—"
    low_pct = pct(low_fixed, low) if isinstance(low_fixed, int) and low else 100

    stamp = f'snapshot · {date.today().isoformat().replace("-", "‑")} · develop @ {data["develop_sha"] or "—"}'

    scorecard, summary = build_scorecard(facts.get("campaign", []))

    tok = {
        "STAMP": esc(stamp),
        "GOV_CHIPS": build_gov_chips(data["governance"], facts),
        "CAMPAIGN_SUMMARY": esc(summary),
        "SCORECARD": scorecard,
        "AUDIT_TOTAL": aud.get("total", "—"),
        "AUDIT_TOTAL2": aud.get("total", "—"),
        "AUDIT_FIXED": fixed,
        "AUDIT_ACCEPTED": accepted,
        "AUDIT_HIGH": aud.get("high", "—"),
        "AUDIT_MED": aud.get("medium", "—"),
        "AUDIT_LOW": low,
        "AUDIT_LOW_FIXED": low_fixed,
        "AUDIT_LOW_ACCEPTED": accepted,
        "AUDIT_LOW_PCT": low_pct,
        "COV_TU_PCT": cov.get("tu_pct", "—"),
        "COV_TU_FRAC": f'{cov.get("tu_have","—")} / {cov.get("tu_total","—")}',
        "COV_LOC_PCT": cov.get("loc_pct", "—"),
        "COV_LOC_FRAC": f'{cov.get("loc_have","—")} / {cov.get("loc_total","—")}',
        "FUZZ_N": data["fuzz"],
        "TEST_TU_N": data["test_tus"],
        "PLANS_ACTIVE": plans["active"],
        "PLANS_DEFERRED": plans["deferred"],
        "PLANS_SHIPPED": plans["shipped"],
        "PLANS_TOTAL": plans["total"],
        "SHIP_RATE": plans["ship_rate"],
        "APPLIED_N": bl.get("applied", "—"),
        "LANES": build_lanes(facts.get("ci_lanes", [])),
        "TRIAGE_NOTE": facts.get("triage_note", ""),
        "PRS": build_prs(facts.get("open_prs", [])),
        "PR_COUNT": len(facts.get("open_prs", [])),
        "GOV_LIST": build_gov_list(facts.get("governance_list", [])),
        "BACKLOG_CATS": build_backlog(bl),
        "PLANS_STACK": build_plan_stack(plans),
        "GEN_AT": esc(datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%MZ")),
    }
    for k, v in tok.items():
        tpl = tpl.replace("{{" + k + "}}", str(v))
    # flag any placeholder left unfilled so it surfaces in review, not the page
    leftover = re.findall(r"\{\{([A-Z0-9_]+)\}\}", tpl)
    if leftover:
        print(f"warning: unfilled template tokens: {sorted(set(leftover))}", file=sys.stderr)
    return tpl


# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the Smatchet repo-health dashboard.")
    ap.add_argument("--out", default=str(HERE / "dashboard.html"), help="output HTML path")
    ap.add_argument("--print", action="store_true", dest="dump", help="print computed metrics to stderr")
    args = ap.parse_args()

    data = collect()
    facts = json.loads((HERE / "facts.json").read_text(encoding="utf-8"))

    if args.dump:
        json.dump(data, sys.stderr, indent=2)
        print(file=sys.stderr)

    Path(args.out).write_text(render(data, facts), encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
