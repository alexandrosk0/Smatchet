#!/usr/bin/env bash
# pr-body-edit.sh — marker-preserving PR-body edits (sourced; also runnable).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (tooling backlog 2026-08-17-pr-body-rewrite-drops-lock-slug-marker,
# item 2; 2026-08-16-verdict-head-hex-hand-copied-into-pr-body)
#   A full PR-body rewrite composed a fresh body and dropped the bare
#   `lock-slug:` line, so the close-time cleanup released nothing and still
#   reported success. The body is the record lock-cleanup.yml reads, and
#   anything that edits it can delete that record. Scripted body edits go
#   through pbe_write, which REFUSES a new body that lost a lock marker the
#   current body carries.
#
# CONTRACT (functions return, never exit — the caller owns its exit codes)
#   pbe_fetch <repo> <pr> <out-file>
#       GET PR <pr>'s current body into <out-file>, byte-exact (the newline
#       `gh --jq` appends is stripped, so a fetch→PATCH round trip is stable).
#       rc 0 ok · 2 gh missing / API failure.
#   pbe_upsert_line <prefix-regex> <line> <in-file> <out-file>
#       Replace the first line OUTSIDE an HTML comment whose start matches the
#       Python regex <prefix-regex> with <line>, and drop any later such line
#       (a second live copy would be stale); append <line> when there is none.
#       Comment-aware, so a template's commented placeholder is never taken
#       for the live line. Keeps the body's line endings. rc 0 · 2 no python.
#   pbe_dropped_markers <old-file> <new-file>
#       Print each lock marker <old-file> has and <new-file> lost.
#       rc 0 none lost · 1 some lost · 2 no python.
#       A marker is a `lock-slug:` / `holds-lock:` line, bare or inside a
#       one-line `<!-- -->`; the template placeholder slug `your-slug-here` is
#       not one. A bare marker must stay bare (commenting it out disarms it);
#       a commented one may be uncommented.
#   pbe_write <repo> <pr> <old-file> <new-file>
#       pbe_dropped_markers guard, a re-read of the live body (it must still
#       equal <old-file>), then PATCH the body.
#       rc 0 written · 1 refused (a marker was lost; nothing sent) · 2 gh
#       missing / API failure · 3 the body changed since <old-file> was read
#       (a concurrent edit; nothing sent — re-fetch, re-derive, retry).
#
# Run directly — the marker-guarded replacement for `gh pr edit --body-file`
# (Intent-gate remediation, any whole-body rewrite):
#   bash agents/scripts/core/lib/pr-body-edit.sh <pr> <new-body-file>
#   REPO=<owner/name> overrides the repo gh resolves from the checkout.
#   Exit: 0 written · 1 refused · 2 usage / gh / API failure · 3 the body
#   was edited concurrently (nothing sent; re-run against the new body).
#
# gh is the only GitHub client (stubbable on PATH in bats); python does the
# text work and the JSON encoding. Sourcing defines functions only.
# ----------------------------------------------------------------------------

_PBE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$_PBE_DIR/resolve-py.sh"

_pbe_py() {
    resolve_py || { echo "pr-body-edit: python3 required (no working interpreter on PATH)" >&2; return 2; }
}

_pbe_gh() {
    command -v gh >/dev/null 2>&1 || { echo "pr-body-edit: gh not on PATH" >&2; return 2; }
}

pbe_fetch() {
    local repo="$1" pr="$2" out="$3" body
    _pbe_gh || return 2
    if ! gh api "repos/${repo}/pulls/${pr}" --jq '.body // ""' > "$out"; then
        echo "pr-body-edit: could not read the body of ${repo}#${pr}" >&2
        return 2
    fi
    # `--jq` prints a string result followed by ONE newline that is not part of
    # the body. Keep it and every sync would PATCH the body back one newline
    # longer. Strip exactly that one (the `x` sentinel keeps $(...) from
    # eating the body's own trailing newlines).
    body="$(cat "$out"; printf x)" || return 2
    body="${body%x}"
    printf '%s' "${body%$'\n'}" > "$out" || return 2
}

pbe_upsert_line() {
    local prefix="$1" line="$2" in="$3" out="$4" py
    py="$(_pbe_py)" || return 2
    PBE_PREFIX="$prefix" PBE_LINE="$line" "$py" - "$in" "$out" <<'PY'
import os, re, sys
prefix = re.compile(os.environ["PBE_PREFIX"])
new_line = os.environ["PBE_LINE"]
src = open(sys.argv[1], encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in src else "\n"
spans = [(m.start(), m.end()) for m in re.finditer(r"(?s)<!--.*?-->", src)]
out, pos, done = [], 0, False
for raw in src.splitlines(keepends=True):
    text = raw.rstrip("\r\n")
    live = prefix.match(text) and not any(a <= pos < b for a, b in spans)
    if live:
        if not done:
            out.append(new_line + raw[len(text):])
            done = True
    else:
        out.append(raw)
    pos += len(raw)
res = "".join(out)
if not done:
    res = (res.rstrip("\r\n") + nl + nl if res.strip() else "") + new_line + nl
open(sys.argv[2], "w", encoding="utf-8", newline="").write(res)
PY
}

pbe_dropped_markers() {
    local old="$1" new="$2" py
    py="$(_pbe_py)" || return 2
    "$py" - "$old" "$new" <<'PY'
import re, sys
MARK = re.compile(r"^\s*(<!--\s*)?(lock-slug|holds-lock):\s*([a-z0-9][a-z0-9-]{0,63})")

def markers(path):
    found = {}
    for line in open(path, encoding="utf-8", newline="").read().splitlines():
        m = MARK.match(line)
        if not m or m.group(3) == "your-slug-here":
            continue
        key = (m.group(2), m.group(3))
        found[key] = found.get(key, False) or m.group(1) is None
    return found

old, new = markers(sys.argv[1]), markers(sys.argv[2])
lost = 0
for (kind, slug), bare in sorted(old.items()):
    if (kind, slug) not in new:
        print("%s: %s (removed)" % (kind, slug))
        lost += 1
    elif bare and not new[(kind, slug)]:
        print("%s: %s (was a bare line, now only commented out)" % (kind, slug))
        lost += 1
sys.exit(1 if lost else 0)
PY
}

pbe_write() {
    local repo="$1" pr="$2" old="$3" new="$4" dropped rc=0 py payload
    dropped="$(pbe_dropped_markers "$old" "$new")" || rc=$?
    if [ "$rc" -eq 1 ]; then
        echo "pr-body-edit: REFUSED — the new body for ${repo}#${pr} loses lock marker(s) the current body carries:" >&2
        printf '%s\n' "$dropped" | sed 's/^/    /' >&2
        echo "  lock-cleanup.yml reads these lines at PR close; carry them into the new body (bare, not commented) and retry." >&2
        return 1
    fi
    [ "$rc" -eq 0 ] || return 2
    _pbe_gh || return 2
    py="$(_pbe_py)" || return 2
    # Concurrency guard: the PATCH replaces the whole body, so an edit made
    # since <old-file> was read would be silently overwritten. Re-read right
    # before writing and refuse when the body moved on. (GitHub takes no
    # If-Match on this endpoint; the window left is the re-read-to-PATCH gap.)
    local now
    now="$(mktemp)" || return 2
    if ! pbe_fetch "$repo" "$pr" "$now"; then
        rm -f "$now"
        return 2
    fi
    if ! cmp -s "$old" "$now"; then
        rm -f "$now"
        echo "pr-body-edit: ${repo}#${pr} body changed since it was read (a concurrent edit) — NOT overwriting it; re-read and retry." >&2
        return 3
    fi
    rm -f "$now"
    payload="$(mktemp)" || return 2
    if ! "$py" -c 'import json, sys; json.dump({"body": open(sys.argv[1], encoding="utf-8", newline="").read()}, sys.stdout)' \
        "$new" > "$payload"; then
        rm -f "$payload"
        return 2
    fi
    if ! gh api -X PATCH "repos/${repo}/pulls/${pr}" --input "$payload" >/dev/null; then
        rm -f "$payload"
        echo "pr-body-edit: PATCH of ${repo}#${pr} failed" >&2
        return 2
    fi
    rm -f "$payload"
}

if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
    set -uo pipefail
    if [ "$#" -ne 2 ]; then
        echo "usage: bash agents/scripts/core/lib/pr-body-edit.sh <pr> <new-body-file>" >&2
        exit 2
    fi
    case "$1" in
        '' | 0* | *[!0-9]*) echo "pr-body-edit: PR number must be a positive integer, got '$1'" >&2; exit 2 ;;
    esac
    [ -f "$2" ] || { echo "pr-body-edit: new body file not found: $2" >&2; exit 2; }
    # shellcheck source=agents/scripts/core/lib/resolve-repo.sh
    . "$_PBE_DIR/resolve-repo.sh"
    _pbe_repo="$(resolve_repo)" || { echo "pr-body-edit: cannot resolve owner/name (run from the repo with gh authed, or set REPO=owner/name)" >&2; exit 2; }
    _pbe_old="$(mktemp)" || exit 2
    pbe_fetch "$_pbe_repo" "$1" "$_pbe_old" || { rm -f "$_pbe_old"; exit 2; }
    _pbe_rc=0
    pbe_write "$_pbe_repo" "$1" "$_pbe_old" "$2" || _pbe_rc=$?
    rm -f "$_pbe_old"
    [ "$_pbe_rc" -eq 0 ] && echo "pr-body-edit: ${_pbe_repo}#$1 body updated (lock markers preserved)."
    exit "$_pbe_rc"
fi
