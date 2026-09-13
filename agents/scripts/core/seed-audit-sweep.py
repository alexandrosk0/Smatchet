#!/usr/bin/env python3
"""seed-audit-sweep.py — publication-audit sweep over the history a seed will publish.

Companion to seed-agent-layer-repo.sh and seed-agent-layer-repo.d/docs/seed-audit.md.
It scans exactly what `git filter-repo --paths-from-file <manifest>` keeps:

  * every ADDED line in `git log -p --cc --full-history --no-renames <range> -- <pathspecs>`
    (--no-renames: a file renamed INTO an allowed path shows as a full add, so its
    content is scanned — filter-repo does not follow renames either; --full-history
    and --cc: side-branch commits and merge conflict resolutions are not skipped);
  * the commit messages and author/committer identities of those commits.

It reports an INVENTORY of distinct matched values per category (secrets, hosts,
tickets, personal data, local paths, P4 specifics) with counts and example
(commit, path) pairs. It does not decide anything: every value is triaged by a
reviewer and the verdict recorded in seed-audit.md. It is NOT a substitute for the
phase-4b gitleaks history scan — it complements it with categories a secret
scanner does not target (internal hosts, ticket URLs, personal data).

Usage:
  seed-audit-sweep.py                      full history of HEAD (initial audit)
  seed-audit-sweep.py --since <sha>        only <sha>..HEAD (delta re-audit)
  seed-audit-sweep.py --json out.json      also write the full inventory as JSON

Exit: 0 sweep completed (triage is human) · 1 coverage gap in full mode (a file in
the manifest at HEAD never appeared in the scanned history — the sweep missed it)
· 2 usage / git error.
"""
import argparse
import collections
import io
import json
import os
import re
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MANIFEST = os.path.join(SCRIPT_DIR, "seed-agent-layer-repo.d", "docs", "seed-paths.txt")

PATTERNS = {
    # credentials
    "secret:github-token": r"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}\b|\bgithub_pat_[A-Za-z0-9_]{22,}",
    "secret:slack": r"\bxox[baprs]-[A-Za-z0-9-]{10,}",
    "secret:aws": r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b",
    "secret:anthropic-openai": r"\bsk-(?:ant-|proj-)?[A-Za-z0-9_-]{24,}",
    "secret:stripe": r"\b(?:sk|rk)_(?:live|test)_[A-Za-z0-9]{10,}",
    "secret:google": r"\bAIza[0-9A-Za-z_-]{35}\b",
    "secret:gitlab": r"\bglpat-[A-Za-z0-9_-]{20}\b",
    "secret:atlassian": r"\bATATT3[A-Za-z0-9_=-]{20,}",
    "secret:private-key": r"-----BEGIN [A-Z ]*PRIVATE KEY-----",
    "secret:jwt": r"\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}",
    "secret:auth-header": r"(?i)authorization:\s*(?:basic|bearer)\s+[A-Za-z0-9+/=._-]{16,}",
    "secret:url-creds": r"\b[a-z][a-z0-9+.-]*://[^/\s:@'\"]+:[^/\s@'\"$]{3,}@",
    "secret:assignment": r"(?i)\b(?:password|passwd|pwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token|client[_-]?secret)\b\s*[:=]\s*['\"]([^'\"\s$<>{}]{8,})['\"]",
    # infrastructure
    "host:url": r"\b[a-z][a-z0-9+.-]*://([A-Za-z0-9._-]+(?::\d+)?)",
    "host:atlassian": r"\b[a-z0-9-]+\.atlassian\.net\b",
    "host:jira": r"(?i)\bhttps?://(?:jira|[a-z0-9-]+\.jira)[a-z0-9.-]*",
    "host:private-ip": r"\b(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b",
    "host:internal-tld": r"\b[a-z0-9][a-z0-9.-]*\.(?:local|lan|corp|internal|intranet|home)\b",
    "p4:port": r"(?i)\b(?:ssl:)?[a-z0-9][a-z0-9.-]*:1666\b|P4PORT\s*=\s*\S+",
    "p4:depot-path": r"//(?:depot|streams|[A-Z][A-Za-z0-9_]{2,})/[A-Za-z0-9_./-]+",
    "p4:client-ws": r"(?i)\bP4CLIENT\s*=\s*[^\s'\"$]+",
    # tickets
    "ticket:browse-url": r"/browse/[A-Z][A-Z0-9]+-\d+",
    "ticket:key": r"\b([A-Z][A-Z0-9]{1,9})-\d{1,6}\b",
    # personal data / machine layout
    "personal:email": r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\b",
    "personal:user-path": r"(?i)\b[A-Z]:[\\/]+Users[\\/]+([^\\/\s'\"]+)|/Users/([^/\s'\"]+)|/home/([^/\s'\"]+)",
    "local:abs-dev-path": r"(?i)\b[A-Z]:[\\/]+(?:Dev|Projects|Work|src|repos)[\\/]+[^\s'\"`)]+",
}
RX = {k: re.compile(v) for k, v in PATTERNS.items()}


def read_pathspecs(manifest):
    with io.open(manifest, encoding="utf-8") as fh:
        return [ln.strip() for ln in fh if ln.strip() and not ln.lstrip().startswith("#")]


def covered(path, specs):
    return any(path == s or (s.endswith("/") and path.startswith(s)) for s in specs)


def git_lines(repo, args):
    proc = subprocess.Popen(["git", "-C", repo] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for raw in proc.stdout:
        yield raw.decode("utf-8", "replace").rstrip("\r\n")
    err = proc.stderr.read().decode("utf-8", "replace")
    if proc.wait() != 0:
        sys.stderr.write("seed-audit-sweep: git %s failed: %s\n" % (args[0], err.strip()))
        sys.exit(2)


class Inventory(object):
    def __init__(self, specs):
        self.specs = specs
        self.rows = {k: {} for k in PATTERNS}

    def scan(self, text, sha, path):
        for cat, rx in RX.items():
            for m in rx.finditer(text):
                groups = [g for g in m.groups() if g] if m.groups() else []
                value = groups[0] if groups else m.group(0)
                row = self.rows[cat].setdefault(value, {"count": 0, "specs": set(), "examples": []})
                row["count"] += 1
                row["specs"].update([s for s in self.specs if covered(path, [s])] or [path])
                if len(row["examples"]) < 3:
                    row["examples"].append({"sha": sha[:10], "path": path, "line": text.strip()[:220]})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--since", help="audit only <sha>..HEAD (delta re-audit)")
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST, help="seed-paths.txt to read pathspecs from")
    ap.add_argument("--json", dest="json_out", help="write the full inventory as JSON")
    ap.add_argument("--repo", default=".", help="repository to scan (default: cwd)")
    args = ap.parse_args()

    if not os.path.isfile(args.manifest):
        sys.stderr.write("seed-audit-sweep: manifest not found: %s\n" % args.manifest)
        return 2
    specs = read_pathspecs(args.manifest)
    rng = ("%s..HEAD" % args.since) if args.since else "HEAD"
    inv = Inventory(specs)
    seen, commits, added, binaries = set(), 0, 0, set()

    sha, path = "", ""
    for line in git_lines(args.repo, ["log", "-p", "--cc", "--full-history", "--no-renames", "--no-color",
                                      "--no-ext-diff", "--format=@@COMMIT@@%H", rng, "--"] + specs):
        if line.startswith("@@COMMIT@@"):
            sha, commits = line[10:], commits + 1
        elif line.startswith("diff --git ") or line.startswith("diff --cc "):
            path = line.split(" b/", 1)[-1] if " b/" in line else line[len("diff --cc "):]
            seen.add(path)
        elif line.startswith("Binary files"):
            binaries.add(path)
        elif line.startswith("+++ ") or line.startswith("--- "):
            continue
        elif "+" in line[:2]:
            added += 1
            inv.scan(line, sha, path)

    identities = collections.Counter()
    sha, k = "", 0
    for line in git_lines(args.repo, ["log", "--full-history", "--no-renames",
                                      "--format=@@COMMIT@@%H%n%an <%ae>%n%cn <%ce>%n%B", rng, "--"] + specs):
        if line.startswith("@@COMMIT@@"):
            sha, k = line[10:], 0
            continue
        k += 1
        if k in (1, 2):
            identities[line] += 1
        else:
            inv.scan(line, sha, "<commit-message>")

    unseen = []
    if not args.since:
        head_files = [f for f in git_lines(args.repo, ["ls-files", "--"] + specs) if f]
        unseen = sorted(set(head_files) - seen)

    out = sys.stdout
    if hasattr(out, "reconfigure"):
        out.reconfigure(encoding="utf-8", errors="replace")
    out.write("range=%s commits=%d added_lines=%d binaries=%d identities=%d\n"
              % (rng, commits, added, len(binaries), len(identities)))
    for ident, n in identities.most_common():
        out.write("  identity %5d  %s\n" % (n, ident))
    for path in sorted(binaries):
        out.write("  BINARY (inspect by hand): %s\n" % path)
    for cat in PATTERNS:
        rows = sorted(inv.rows[cat].items(), key=lambda kv: -kv[1]["count"])
        out.write("== %s (%d distinct) ==\n" % (cat, len(rows)))
        for value, row in rows:
            out.write("  [%d] %s\n" % (row["count"], value if len(value) <= 80 else value[:80] + "..."))
            for ex in row["examples"][:2]:
                out.write("       %s %s :: %s\n" % (ex["sha"], ex["path"], ex["line"][:160]))

    if args.json_out:
        doc = {
            "range": rng, "commits": commits, "added_lines": added, "binaries": sorted(binaries),
            "identities": identities.most_common(), "unseen_head_files": unseen,
            "inventory": {cat: [dict(value=v, count=r["count"], specs=sorted(r["specs"]), examples=r["examples"])
                                for v, r in sorted(inv.rows[cat].items(), key=lambda kv: -kv[1]["count"])]
                          for cat in PATTERNS},
        }
        with io.open(args.json_out, "w", encoding="utf-8", newline="") as fh:
            fh.write(json.dumps(doc, indent=1))

    if unseen:
        out.write("COVERAGE GAP: %d manifest file(s) at HEAD never appeared in the scanned history:\n" % len(unseen))
        for f in unseen:
            out.write("  %s\n" % f)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
