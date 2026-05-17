#!/usr/bin/env python3
"""Internal JSON helper for the lock-*.sh scripts.

Subcommands:
  build-claim   — emit a claim.json on stdout from environment + WS_FILE.
                  Env: SLUG (required), WS_FILE (required), OWNER, BRANCH,
                  PLAN, STARTED, UPDATED, NOTES.
  read-field    — read a JSON object from stdin, print one field's value.
                  Usage: read-field <field>
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
            "usage: _lock-json.py <build-claim|read-field|format-table|format-json>",
            file=sys.stderr,
        )
        sys.exit(2)
    cmd = sys.argv[1]
    if cmd == "build-claim":
        build_claim()
    elif cmd == "read-field":
        read_field()
    elif cmd == "format-table":
        format_table()
    elif cmd == "format-json":
        format_json()
    else:
        print("unknown subcommand: " + cmd, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
