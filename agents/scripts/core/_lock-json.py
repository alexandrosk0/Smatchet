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
  lock-rows     — read a JSON array of claim records from stdin, emit one
                  TSV row per (lock, write_set path):
                  `branch <TAB> latest_epoch <TAB> slug <TAB> path`.
                  latest_epoch = max(started, updated) as epoch seconds.
                  Pure parsing for the shared bash coverage primitive in
                  lock-table-cache.sh (Layers A/B/C); does NO coverage match.

Pure stdlib. Targets Python 3.7+; emits timezone-aware UTC via
datetime.now(timezone.utc) (datetime.utcnow is deprecated in 3.12+).
Underscore-prefixed name marks the file as internal-helper to the lock
scripts and not part of the supported scripts/dev/ surface.
"""
from __future__ import print_function

import datetime
import json
import os
import sys


def _utc_now_z():
    # datetime.utcnow() is deprecated in Python 3.12+; emit timezone-aware
    # then format with literal Z suffix to match Smatchet's claim-timestamp
    # grammar (strftime('%z') would produce '+0000', not 'Z').
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


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


def _iso_to_epoch_str(s):
    """ISO-8601 string -> integer epoch seconds as a string; '' on error.

    Accepts both 'Z' and '+00:00' tz suffixes. Shared by iso-to-epoch and
    lock-rows so the two never diverge on timestamp parsing.
    """
    if not s:
        return ""
    try:
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        return str(int(datetime.datetime.fromisoformat(s).timestamp()))
    except (ValueError, TypeError):
        return ""


def iso_to_epoch():
    """Read ISO-8601 timestamp from LATEST_TS env, print integer epoch seconds.

    Accepts both 'Z' and '+00:00' tz suffixes. Empty string on parse
    error. Reads via env var (NOT argv or stdin) so the bash caller can
    pass attacker-influenced timestamps without shell-into-python
    source interpolation (see Phase 4 security fix recorded in the
    plan doc).
    """
    sys.stdout.write(_iso_to_epoch_str(os.environ.get("LATEST_TS", "") or "") + "\n")


def format_table():
    """Render a fixed-width table of claim records.

    Columns auto-size to the widest value per column so long slugs
    (up to the 64-char schema max) or unusual owner names don't break
    alignment. Header row is "SLUG OWNER BRANCH STARTED PATHS
    FIRST-PATH" — paths column is right-aligned integer, others
    left-aligned.
    """
    records = json.load(sys.stdin)
    if not records:
        print("(no plan-locks held)")
        return

    headers = ("SLUG", "OWNER", "BRANCH", "STARTED", "PATHS", "FIRST-PATH")
    rows = []
    for r in records:
        slug = r.get("_slug") or r.get("slug") or "?"
        owner = r.get("owner") or "?"
        branch = r.get("branch") or "?"
        started = r.get("started") or "?"
        paths = r.get("write_set") or []
        first = paths[0] if paths else "—"
        rows.append((slug, owner, branch, started, str(len(paths)), first))

    # Compute max width per column across header + every data row.
    widths = [
        max(len(headers[i]), *(len(row[i]) for row in rows))
        for i in range(len(headers))
    ]
    # Last column has no padding (lets long FIRST-PATH values trail off
    # without right-padding the line). Paths column right-aligned for
    # integer-style read.
    fmt_parts = []
    for i in range(len(headers)):
        if i == len(headers) - 1:
            fmt_parts.append("{}")
        elif i == 4:  # PATHS — right-align
            fmt_parts.append("{:>" + str(widths[i]) + "}")
        else:
            fmt_parts.append("{:<" + str(widths[i]) + "}")
    fmt = " ".join(fmt_parts)

    print(fmt.format(*headers))
    print(fmt.format(*("-" * w for w in widths)))
    for row in rows:
        print(fmt.format(*row))


def format_json():
    records = json.load(sys.stdin)
    json.dump(records, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


def lock_rows():
    """Flatten a lock-table JSON array into TSV rows, one per (lock, path).

    Columns (tab-separated): branch <TAB> latest_epoch <TAB> slug <TAB> path.
    latest_epoch is max(started, updated) as integer epoch seconds ('' when
    unparseable). This is the shared-parser half of the three-layer coverage
    check — the exact-path / dir-prefix / branch / staleness decisions all live
    in the one bash primitive (lock-table-cache.sh), so A/B/C match byte-for-byte
    by construction. Fail-soft: a malformed array prints nothing (caller treats
    an empty row set as "no covering lock").
    """
    try:
        records = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return
    if not isinstance(records, list):
        return
    for r in records:
        if not isinstance(r, dict):
            continue
        branch = r.get("branch") or ""
        slug = r.get("_slug") or r.get("slug") or ""
        started = r.get("started") or ""
        updated = r.get("updated") or ""
        ts = updated if (updated and (not started or updated >= started)) else (started or "")
        epoch = _iso_to_epoch_str(ts)
        for p in (r.get("write_set") or []):
            # Repo-relative paths never contain a tab/newline; skip any that do
            # so a crafted write_set can't smuggle extra TSV columns/rows.
            if not isinstance(p, str) or "\t" in p or "\n" in p:
                continue
            sys.stdout.write("%s\t%s\t%s\t%s\n" % (branch, epoch, slug, p))


def main():
    if len(sys.argv) < 2:
        print(
            "usage: _lock-json.py <build-claim|read-field|latest-ts|iso-to-epoch|format-table|format-json|lock-rows>",
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
    elif cmd == "lock-rows":
        lock_rows()
    else:
        print("unknown subcommand: " + cmd, file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
