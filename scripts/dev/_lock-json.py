#!/usr/bin/env python3
"""Internal JSON helper for the lock-*.sh scripts.

Subcommands:
  build-claim   — emit a claim.json on stdout from environment + WS_FILE.
                  Env: SLUG (required), WS_FILE (required), OWNER, BRANCH,
                  PLAN, STARTED, UPDATED, NOTES.
  read-field    — read a JSON object from stdin, print one field's value.
                  Usage: read-field <field>
  latest-ts     — read a claim.json from stdin, print
                  `max(started, updated)` as the latest activity timestamp.
                  Falls back to started when updated is empty / missing.
                  Prints empty string when both are absent.
  iso-to-epoch  — read an ISO-8601 timestamp from the LATEST_TS env var,
                  print integer epoch seconds. Accepts both "Z" and
                  "+00:00" timezone suffixes. Prints empty on parse error.
                  Reads via env var so caller can avoid shell-into-python
                  source interpolation (see git-ref-plan-locks.md §
                  Phase 4 security fix).
  format-table  — read a JSON array of claim records from stdin, print as
                  a fixed-width table.
  format-json   — read a JSON array, pretty-print to stdout.

Pure stdlib. Targets Python 3.7+ (datetime.utcnow available everywhere).
Underscore-prefixed name marks the file as internal-helper to the lock
scripts and not part of the supported scripts/dev/ surface.
"""
from __future__ import print_function

import datetime
import json
import os
import sys


def _utc_now_z():
    return datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ")


def build_claim():
    ws_path = os.environ.get("WS_FILE")
    slug = os.environ.get("SLUG")
    if not ws_path or not slug:
        print("build-claim: SLUG and WS_FILE env vars required", file=sys.stderr)
        sys.exit(2)
    paths = []
    with open(ws_path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            paths.append(line)
    started = os.environ.get("STARTED") or _utc_now_z()
    claim = {
        "schema": "smatchet-plan-lock/1",
        "slug": slug,
        "owner": os.environ.get("OWNER", "orchestrator"),
        "branch": os.environ.get("BRANCH", ""),
        "originating_plan": os.environ.get("PLAN", ""),
        "started": started,
        "updated": os.environ.get("UPDATED", started),
        "write_set": paths,
        "notes": os.environ.get("NOTES", ""),
    }
    json.dump(claim, sys.stdout, separators=(",", ":"), sort_keys=True)
    sys.stdout.write("\n")


def read_field():
    if len(sys.argv) < 3:
        print("usage: _lock-json.py read-field <field>", file=sys.stderr)
        sys.exit(2)
    data = json.load(sys.stdin)
    value = data.get(sys.argv[2], "")
    if isinstance(value, (list, dict)):
        json.dump(value, sys.stdout)
        sys.stdout.write("\n")
    else:
        print(value if value is not None else "")


def latest_ts():
    """Print max(started, updated) from a claim.json read on stdin.

    Falls back to started when updated is empty or absent. Empty string
    on both missing. Stable behaviour for the staleness sweep —
    long-running slices that bump via lock-claim-update.sh push their
    `updated` forward and stay fresh.
    """
    try:
        data = json.loads(sys.stdin.read() or "{}")
    except json.JSONDecodeError:
        sys.stdout.write("\n")
        return
    started = data.get("started") or ""
    updated = data.get("updated") or ""
    # Use updated when non-empty AND later than started by lexicographic
    # ISO-8601 ordering. ISO-8601 strings sort identically to chronological
    # order when all share the same timezone suffix (Z or +00:00).
    if updated and (not started or updated >= started):
        sys.stdout.write(updated + "\n")
    elif started:
        sys.stdout.write(started + "\n")
    else:
        sys.stdout.write("\n")


def iso_to_epoch():
    """Read ISO-8601 timestamp from LATEST_TS env, print integer epoch seconds.

    Accepts both 'Z' and '+00:00' tz suffixes. Empty string on parse
    error. Reads via env var (NOT argv or stdin) so the bash caller can
    pass attacker-influenced timestamps without shell-into-python
    source interpolation (see Phase 4 security fix recorded in the
    plan doc).
    """
    s = os.environ.get("LATEST_TS", "") or ""
    if not s:
        sys.stdout.write("\n")
        return
    try:
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        dt = datetime.datetime.fromisoformat(s)
        sys.stdout.write(str(int(dt.timestamp())) + "\n")
    except (ValueError, TypeError):
        sys.stdout.write("\n")


def format_table():
    records = json.load(sys.stdin)
    if not records:
        print("(no plan-locks held)")
        return
    fmt = "{:<32} {:<24} {:<32} {:<20} {:>5} {}"
    print(fmt.format("SLUG", "OWNER", "BRANCH", "STARTED", "PATHS", "FIRST-PATH"))
    print(fmt.format("----", "-----", "------", "-------", "-----", "----------"))
    for r in records:
        slug = r.get("_slug") or r.get("slug", "?")
        owner = r.get("owner") or "?"
        branch = r.get("branch") or "?"
        started = r.get("started") or "?"
        paths = r.get("write_set") or []
        first = paths[0] if paths else "—"
        print(fmt.format(slug, owner, branch, started, len(paths), first))


def format_json():
    records = json.load(sys.stdin)
    json.dump(records, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def main():
    if len(sys.argv) < 2:
        print(
            "usage: _lock-json.py <build-claim|read-field|latest-ts|iso-to-epoch|format-table|format-json>",
            file=sys.stderr,
        )
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "build-claim":
        build_claim()
    elif cmd == "read-field":
        read_field()
    elif cmd == "latest-ts":
        latest_ts()
    elif cmd == "iso-to-epoch":
        iso_to_epoch()
    elif cmd == "format-table":
        format_table()
    elif cmd == "format-json":
        format_json()
    else:
        print("unknown subcommand: " + cmd, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
