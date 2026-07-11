#!/usr/bin/env python3
# scripts/dev/osv-scan.py — query OSV.dev for known CVEs in the pinned deps.
#
# Closes the CVE-scanning half of the Tier-6 gap (DEEPER_AUDIT_PLAYBOOK.md
# target 31 / tooling.md item 4). Consumes the CycloneDX SBOM from
# scripts/dev/gen-sbom.py.
#
# Why query OSV.dev directly by git commit (not the osv-scanner binary)?
#   Every dependency here is pinned to an immutable git commit, and OSV's
#   commit-range matching (`{"commit": "<sha>"}`) is the most precise key for
#   unversioned C/C++ source deps — it resolves a vulnerability's affected GIT
#   ranges against the exact SHA we build, with none of the ecosystem-PURL
#   guesswork an SBOM-only scan needs for github-hosted C++ libraries. It also
#   needs no third-party action/binary (nothing extra to SHA-pin), just Python
#   stdlib + the public OSV API. Same OSV vulnerability database; higher signal.
#
# Advisory-on-introduction: with no --fail-on threshold the script REPORTS and
# exits 0 even when it finds CVEs (findings surface in the job summary), so a new
# gate cannot red-wall every open PR on day one. It still exits non-zero on a
# BROKEN lane (unreadable SBOM, OSV unreachable after retries) — a broken scan is
# a non-result, not a pass. To promote to blocking once findings are triaged,
# pass `--fail-on HIGH` in the workflow and grow scripts/dev/osv-allowlist.json.
#
# Usage:
#   python3 scripts/dev/osv-scan.py --sbom sbom.cdx.json \
#       [--allowlist scripts/dev/osv-allowlist.json] \
#       [--fail-on HIGH] [--json-out findings.json]
#
# Pure stdlib.

import math
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

OSV_BATCH_URL = "https://api.osv.dev/v1/querybatch"
OSV_VULN_URL = "https://api.osv.dev/v1/vulns/"

SEVERITY_RANK = {"UNKNOWN": 0, "LOW": 1, "MODERATE": 2, "MEDIUM": 2, "HIGH": 3, "CRITICAL": 4}


def _http_json(url, payload=None, retries=4):
    """GET (payload=None) or POST JSON, with exponential backoff on transient
    errors. Raises after the last retry so the caller can fail the lane."""
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json", "User-Agent": "smatchet-osv-scan/1"}
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, data=data, headers=headers,
                                         method="POST" if data else "GET")
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.loads(resp.read().decode())
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as e:
            last = e
            # 404 on a vuln lookup is a real "not found", not transient.
            if isinstance(e, urllib.error.HTTPError) and e.code == 404:
                raise
            time.sleep(2 ** attempt)
    raise RuntimeError(f"OSV request failed after {retries} attempts: {url}: {last}")


def load_components(sbom_path):
    doc = json.loads(Path(sbom_path).read_text(encoding="utf-8"))
    comps = []
    for c in doc.get("components", []):
        commit = None
        for p in c.get("properties", []):
            if p.get("name") == "smatchet:git-commit":
                commit = p.get("value")
        comps.append({
            "name": c.get("name"),
            "version": c.get("version"),
            "purl": c.get("purl"),
            "commit": commit,
        })
    return comps


def cvss3_base_from_vector(vector):
    """Compute the CVSS v3.x base score from a vector string, or None if it is not
    a parseable CVSS v3 vector. OSV's `severity[].score` for type CVSS_V3 is the
    VECTOR string (e.g. "CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H"), not a
    number — float() raised and the caller fell through to UNKNOWN (rank 0),
    letting a real HIGH/CRITICAL CVE evade a `--fail-on HIGH` gate."""
    if not isinstance(vector, str) or not vector.upper().startswith("CVSS:3"):
        return None
    metrics = {}
    for part in vector.split("/")[1:]:
        key, sep, val = part.partition(":")
        if sep:
            metrics[key.upper()] = val.upper()
    changed = metrics.get("S") == "C"
    try:
        av = {"N": 0.85, "A": 0.62, "L": 0.55, "P": 0.2}[metrics["AV"]]
        ac = {"L": 0.77, "H": 0.44}[metrics["AC"]]
        ui = {"N": 0.85, "R": 0.62}[metrics["UI"]]
        pr = ({"N": 0.85, "L": 0.68, "H": 0.5} if changed else {"N": 0.85, "L": 0.62, "H": 0.27})[metrics["PR"]]
        cia = {"H": 0.56, "L": 0.22, "N": 0.0}
        conf, integ, avail = cia[metrics["C"]], cia[metrics["I"]], cia[metrics["A"]]
    except KeyError:
        return None
    iss = 1 - (1 - conf) * (1 - integ) * (1 - avail)
    impact = (7.52 * (iss - 0.029) - 3.25 * (iss - 0.02) ** 15) if changed else (6.42 * iss)
    if impact <= 0:
        return 0.0
    exploitability = 8.22 * av * ac * pr * ui
    raw = (1.08 * (impact + exploitability)) if changed else (impact + exploitability)
    # CVSS roundup: smallest 1-decimal value >= raw (capped at 10.0).
    return min(math.ceil(raw * 10) / 10.0, 10.0)


def _bucket_score(v):
    if v >= 9.0:
        return "CRITICAL"
    if v >= 7.0:
        return "HIGH"
    if v >= 4.0:
        return "MODERATE"
    if v > 0:
        return "LOW"
    return None


def severity_of(vuln):
    """Best-effort severity label from an OSV record."""
    ds = vuln.get("database_specific", {})
    if isinstance(ds, dict) and ds.get("severity"):
        return str(ds["severity"]).upper()
    # No label: bucket by base score — a bare number, or a CVSS v3 vector we score.
    for s in vuln.get("severity", []):
        score = str(s.get("score", ""))
        try:
            v = float(score)
        except ValueError:
            v = cvss3_base_from_vector(score)
            if v is None:
                continue
        bucket = _bucket_score(v)
        if bucket is not None:
            return bucket
    return "UNKNOWN"


def load_allowlist(path):
    if not path or not Path(path).exists():
        return {}
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    # { "GHSA-xxxx": "reason / triage note", ... }
    return doc.get("allow", doc) if isinstance(doc, dict) else {}


def main():
    ap = argparse.ArgumentParser(description="Scan the SBOM's pinned deps against OSV.dev.")
    ap.add_argument("--sbom", required=True)
    ap.add_argument("--allowlist", default="scripts/dev/osv-allowlist.json")
    ap.add_argument("--fail-on", choices=["LOW", "MODERATE", "HIGH", "CRITICAL"], default=None,
                    help="Exit non-zero if a non-allowlisted finding is at/above this severity. "
                         "Omit for advisory-on-introduction (report only).")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    comps = load_components(args.sbom)
    allow = load_allowlist(args.allowlist)

    scannable = [c for c in comps if c["commit"]]
    unscanned = [c for c in comps if not c["commit"]]

    # Batch query by commit (index-aligned results).
    findings = []
    if scannable:
        payload = {"queries": [{"commit": c["commit"]} for c in scannable]}
        results = _http_json(OSV_BATCH_URL, payload).get("results", [])
        for comp, res in zip(scannable, results):
            for v in (res or {}).get("vulns", []) or []:
                vid = v.get("id")
                if not vid:
                    continue
                detail = _http_json(OSV_VULN_URL + vid)
                findings.append({
                    "component": comp["name"],
                    "version": comp["version"],
                    "commit": comp["commit"],
                    "id": vid,
                    "aliases": detail.get("aliases", []),
                    "summary": (detail.get("summary") or "").strip(),
                    "severity": severity_of(detail),
                    "allowlisted": vid in allow or any(a in allow for a in detail.get("aliases", [])),
                    "allow_reason": allow.get(vid, ""),
                })

    # ---- Report (job summary + stdout) --------------------------------------
    lines = []
    lines.append("# Dependency CVE scan (OSV.dev, advisory)\n")
    lines.append(f"- SBOM components: **{len(comps)}** "
                 f"({len(scannable)} scanned by git commit, {len(unscanned)} not commit-pinned)\n")
    active = [f for f in findings if not f["allowlisted"]]
    allowed = [f for f in findings if f["allowlisted"]]
    if not findings:
        lines.append("\n**No known vulnerabilities matched.**\n")
    else:
        lines.append(f"\n**{len(active)} active finding(s)**, {len(allowed)} allowlisted.\n")
        lines.append("\n| Severity | Component | Version | Advisory | Summary | Status |")
        lines.append("\n|---|---|---|---|---|---|")
        for f in sorted(findings, key=lambda x: -SEVERITY_RANK.get(x["severity"], 0)):
            status = f"allowlisted ({f['allow_reason']})" if f["allowlisted"] else "ACTIVE"
            summ = (f["summary"][:80] + "…") if len(f["summary"]) > 80 else (f["summary"] or "—")
            lines.append(f"\n| {f['severity']} | {f['component']} | {f['version']} | "
                         f"{f['id']} | {summ} | {status} |")
        lines.append("\n")
    if unscanned:
        names = ", ".join(f"{c['name']} {c['version']}" for c in unscanned)
        lines.append(f"\n> Not commit-pinned (SBOM-only, not OSV-commit-scanned): {names}. "
                     f"Track these manually.\n")
    report = "".join(lines)
    print(report)
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as fh:
            fh.write(report + "\n")
    for f in active:
        print(f"::warning title=OSV {f['id']} ({f['severity']})::"
              f"{f['component']} {f['version']} — {f['id']} {f['summary'][:120]}")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({
            "components": comps, "findings": findings,
        }, indent=2) + "\n", encoding="utf-8")

    # ---- Exit policy --------------------------------------------------------
    if args.fail_on:
        threshold = SEVERITY_RANK[args.fail_on]
        breaching = [f for f in active if SEVERITY_RANK.get(f["severity"], 0) >= threshold]
        if breaching:
            print(f"\n::error::{len(breaching)} finding(s) at/above {args.fail_on} "
                  f"(not allowlisted). Failing the gate.")
            return 3
    print("\nosv-scan: advisory pass (report only)."
          if not args.fail_on else "\nosv-scan: no breaching findings.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:  # broken lane — do NOT mask as a pass
        print(f"::error::osv-scan lane failure: {e}", file=sys.stderr)
        sys.exit(2)
