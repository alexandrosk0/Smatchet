#!/usr/bin/env python3
# scripts/dev/gen-sbom.py — emit a CycloneDX SBOM from the FetchContent pin set.
#
# Closes the "no SBOM" half of the Tier-6 supply-chain gap
# (docs/security/DEEPER_AUDIT_PLAYBOOK.md target 31; the OSV/SBOM lane, item 4
# of the tooling epic in docs/self-improvement/categories/tooling.md).
#
# Why hand-author instead of run Syft/Trivy over build/_deps?
#   Smatchet has NO package-manager lockfile: every third-party dependency is a
#   CMake `FetchContent_Declare` pinned by an immutable `GIT_TAG <sha>` (plus one
#   plain-HTTP Lua tarball pinned by sha256). A source-tree scanner would have to
#   first `configure` the project — which clones ~13 repos over the network and
#   is slow/flaky in CI — and would still only recover versions from whatever
#   metadata each upstream happens to ship. The pin set in the CMake files IS the
#   authoritative bill of materials, so we read it directly. CMake stays the
#   single source of truth; this generator only reflects it (no separate manifest
#   to drift).
#
# Output: CycloneDX 1.5 JSON on stdout (or --output <path>). Each component
# carries the git commit both in its purl and in a `smatchet:git-commit`
# property so scripts/dev/osv-scan.py can query OSV.dev by commit — the precise
# match key for source deps pinned by SHA (see that script's header).
#
# Usage:
#   python3 scripts/dev/gen-sbom.py [--output sbom.cdx.json] [--repo-root .]
#
# Pure stdlib — no third-party imports, runs anywhere Python 3.8+ is present.

import argparse
import datetime
import json
import re
import sys
import uuid
from pathlib import Path

# CMake files that declare pinned third-party dependencies. Kept explicit (not a
# glob) so a new deps-bearing CMake file is a conscious add here — the SBOM must
# never silently miss a fetch site.
CMAKE_DEP_FILES = [
    "CMakeLists.txt",
    "cmake/ImGuiTestEngine.cmake",
    "tests/CMakeLists.txt",
]

_RE_REPO = re.compile(r"GIT_REPOSITORY\s+(\S+)")
# 40-hex git SHA + optional trailing "# comment" (the repo's pin convention:
#   GIT_TAG <sha>  # <name> <version>).
_RE_TAG = re.compile(r"GIT_TAG\s+([0-9a-fA-F]{40})\b\s*(?:#\s*(.*?)\s*)?$")
# Fallback: a "# v1.2.3 ..." version comment on a line ABOVE a bare GIT_TAG
# (Dear ImGui pins to a docking-branch commit with the version noted above it).
_RE_VER_COMMENT = re.compile(r"#\s*(v?\d[\w.\-]*)")


def _owner_repo(url: str):
    """github.com/<owner>/<repo>(.git) -> (owner, repo) or (None, None)."""
    m = re.search(r"github\.com[/:]([^/]+)/([^/]+?)(?:\.git)?$", url.strip())
    if not m:
        return None, None
    return m.group(1), m.group(2)


def parse_git_deps(root: Path):
    """Pair each GIT_REPOSITORY with the GIT_TAG that follows it, in file order.

    Within a FetchContent_Declare the repository always precedes its tag, and no
    stray GIT_TAG appears without a preceding repository, so an order-preserving
    walk pairs them unambiguously across single- and multi-line declarations.
    """
    deps = []
    for rel in CMAKE_DEP_FILES:
        path = root / rel
        if not path.exists():
            print(f"gen-sbom: WARNING — {rel} not found; skipping", file=sys.stderr)
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        pending_repo = None
        pending_repo_line = -1
        for i, line in enumerate(lines):
            mr = _RE_REPO.search(line)
            if mr:
                pending_repo = mr.group(1)
                pending_repo_line = i
                # No `continue`: the single-line declaration form puts
                # GIT_REPOSITORY and GIT_TAG on the SAME line, so fall through to
                # the tag check below.
            mt = _RE_TAG.search(line)
            if mt and pending_repo is not None:
                commit = mt.group(1)
                comment = (mt.group(2) or "").strip()
                if not comment:
                    # Look back a few lines for a version comment (ImGui shape).
                    for j in range(i - 1, max(pending_repo_line - 1, i - 5), -1):
                        mv = _RE_VER_COMMENT.search(lines[j])
                        if mv:
                            comment = mv.group(1)
                            break
                owner, repo = _owner_repo(pending_repo)
                # Human name: prefer the leading token of the pin comment when it
                # is not itself a version; else the repo basename.
                name = repo or "unknown"
                version = comment or commit[:12]
                if comment:
                    toks = comment.split()
                    if toks and not re.match(r"^v?\d", toks[0]):
                        name = toks[0]
                        version = " ".join(toks[1:]) or commit[:12]
                deps.append({
                    "name": name,
                    "version": version,
                    "repo_url": pending_repo,
                    "owner": owner,
                    "repo": repo,
                    "commit": commit,
                    "source_file": rel,
                })
                pending_repo = None
    return deps


def supplemental_deps():
    """Deps NOT declared as git FetchContent — hand-tracked, kept in lockstep
    with CMakeLists.txt by review (each carries its source-line note)."""
    return [
        {
            # CMakeLists.txt ~L720-777: plain-HTTP tarball, TOFU-pinned by sha256
            # (lua.org publishes no git ref / no checksum companion).
            "name": "lua",
            "version": "5.3.6",
            "repo_url": "https://www.lua.org/ftp/lua-5.3.6.tar.gz",
            "owner": None,
            "repo": None,
            "commit": None,
            "sha256": "fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60",
            "source_file": "CMakeLists.txt",
        },
        {
            # CMakeLists.txt ~L1289/1911: vendored Font Awesome 6 icon-name header
            # (zlib) + optional Solid TTF (SIL OFL 1.1). Font asset — carried for
            # SBOM completeness; not a code-execution CVE surface.
            "name": "fontawesome",
            "version": "6",
            "repo_url": "https://github.com/FortAwesome/Font-Awesome",
            "owner": "FortAwesome",
            "repo": "Font-Awesome",
            "commit": None,
            "source_file": "CMakeLists.txt",
        },
    ]


def to_cyclonedx(deps):
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    components = []
    for d in deps:
        purl = None
        if d.get("owner") and d.get("repo"):
            ref = d["commit"] or d["version"]
            purl = f"pkg:github/{d['owner']}/{d['repo']}@{ref}"
        props = [{"name": "smatchet:source-file", "value": d["source_file"]}]
        if d.get("commit"):
            props.append({"name": "smatchet:git-commit", "value": d["commit"]})
        comp = {
            "type": "library",
            "bom-ref": purl or f"smatchet:{d['name']}@{d['version']}",
            "name": d["name"],
            "version": str(d["version"]),
            "scope": "required",
            "externalReferences": [
                {"type": "vcs", "url": d["repo_url"]},
            ],
            "properties": props,
        }
        if purl:
            comp["purl"] = purl
        if d.get("sha256"):
            comp["hashes"] = [{"alg": "SHA-256", "content": d["sha256"]}]
        components.append(comp)

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": now,
            "tools": [{"vendor": "Smatchet", "name": "gen-sbom.py", "version": "1"}],
            "component": {
                "type": "application",
                "bom-ref": "pkg:github/alexandrosk0/Smatchet",
                "name": "Smatchet",
                "version": "0.0.0",
            },
        },
        "components": components,
    }


def main():
    ap = argparse.ArgumentParser(description="Emit a CycloneDX SBOM from the CMake FetchContent pin set.")
    ap.add_argument("--output", "-o", help="Write to this path instead of stdout.")
    ap.add_argument("--repo-root", default=None, help="Repo root (default: two levels up from this script).")
    args = ap.parse_args()

    root = Path(args.repo_root) if args.repo_root else Path(__file__).resolve().parents[2]
    deps = parse_git_deps(root) + supplemental_deps()
    if not deps:
        print("gen-sbom: ERROR — no dependencies parsed (CMake layout changed?)", file=sys.stderr)
        return 2

    sbom = to_cyclonedx(deps)
    text = json.dumps(sbom, indent=2)
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
        print(f"gen-sbom: wrote {len(deps)} components -> {args.output}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
