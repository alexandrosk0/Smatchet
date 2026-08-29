#!/usr/bin/env bash
# archive-backlog-entry.sh — archive a per-entry self-improvement backlog file
# into the union-merged `applied.md`, fixing BOTH directions of link breakage
# that the raw `cat >> … && git rm` recipe causes.
#
# WHY THIS EXISTS (per-entry-archival-breaks-relative-links, tooling P2)
#   The documented recipe was:
#       cat docs/self-improvement/categories/<cat>/<file>.md >> …/applied.md
#       git rm docs/self-improvement/categories/<cat>/<file>.md
#   Per-entry files live at `categories/<cat>/`; `applied.md` lives one level up
#   at `categories/`. So the recipe breaks links twice over:
#
#   OUTBOUND — every relative link in the body is off by one directory the
#   instant it is appended. `cat` cannot fail here, so it fails SILENTLY; the
#   only signal is the docs gate reporting on applied.md later. Live cost when
#   archiving `2026-08-06-gate-tooling-run-from-stale-session-branch`: 7 dangling
#   links in one entry.
#
#   INBOUND — and this is the worse half — the `git rm` deletes a path other
#   documents cite. Those referrers are unmodified, so a diff-scoped check cannot
#   see the breakage; only a repo-wide `--all` sweep does. The same archival left
#   2 inbound dangling links (from postmortems.md and a sibling entry) and
#   surfaced as a RED REQUIRED CHECK rather than a local failure.
#
#   A script is the right shape rather than a documented sed: the outbound
#   transform depends on the source entry's depth, which is exactly the detail a
#   human copying a command out of a doc will not re-derive.
#
# WHAT IT DOES
#   1. Rewrites the body's relative links for their new home (see re-depth note
#      below) — link targets only, never prose.
#   2. Finds inbound references to the entry across the repo and repoints markdown
#      links at `applied.md`, at each referrer's own correct depth. REFUSES to
#      delete while any inbound markdown link remains unresolved.
#   3. Appends the rewritten body to applied.md, `git rm`s the source, stages both.
#   4. Re-runs `test-markdown-links.sh --all` as a self-check — `--all` is
#      load-bearing: the default diff scope sees neither the re-parented body nor
#      the orphaned referrers.
#
# Usage:
#   bash agents/scripts/core/archive-backlog-entry.sh <path-to-entry.md>
#   bash agents/scripts/core/archive-backlog-entry.sh --dry-run <path>   # print, touch nothing
#   bash agents/scripts/core/archive-backlog-entry.sh --selftest
#
# Env: ARCHIVE_SKIP_LINK_CHECK=1  skip step 4 (bats uses this — the self-check
#      scans the real repo, which a fixture archival must not depend on).
#
# Exit codes: 0 ok · 2 bad args / missing file · 3 unresolved inbound reference
#             · 4 post-archive link check failed.

set -euo pipefail
ARCHIVE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$(git rev-parse --show-toplevel)" 2>/dev/null || true

# Gate-logic staleness guard (script-freshness-remaining-callers). Advisory.
archive_freshness_lib="$ARCHIVE_DIR/lib/script-freshness.sh"
if [ -r "$archive_freshness_lib" ]; then
    # shellcheck source=agents/scripts/core/lib/script-freshness.sh
    . "$archive_freshness_lib"
fi

DRY_RUN=false
SELFTEST=false
ENTRY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)  DRY_RUN=true ;;
        --selftest) SELFTEST=true ;;
        -*) echo "usage: $0 [--dry-run] <path-to-entry.md> | --selftest" >&2; exit 2 ;;
        *)  if [ -n "$ENTRY" ]; then
                echo "archive-backlog-entry: one entry at a time (got '$ENTRY' and '$1')" >&2; exit 2
            fi
            ENTRY="$1" ;;
    esac
    shift
done

PY=""
for c in python3 python py; do
    # Probe-EXECUTE, don't just resolve: on Windows `python3` resolves to the
    # Store alias stub, which is on PATH but exits non-zero on run.
    if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "archive-backlog-entry: no working python interpreter" >&2; exit 2; }

# ── The re-depth + inbound engine ────────────────────────────────────────────
# Kept in one python program because both halves need real path normalization:
# the transform is NOT a fixed set of prefix rewrites. A link X written relative
# to `categories/<cat>/` resolves to `categories/<cat>/X`; from applied.md's home
# at `categories/` the equivalent link is normalize("<cat>/" + X). That single
# rule subsumes every case the backlog entry enumerated by hand —
# `../../../../agents/…` → `../../../agents/…`, `../tooling/slug.md` →
# `tooling/slug.md`, `../applied.md` → `applied.md` — and, unlike a sed table,
# it stays correct if the per-entry convention ever nests deeper.
archive_py() {  # <mode> <args...>  — mode: redepth | inbound-scan | inbound-fix
    "$PY" - "$@" <<'PY'
import os, posixpath, re, sys

MODE = sys.argv[1]

# Markdown inline links: [text](target) and the CommonMark titled form
# [text](target "Title"). The title is captured separately and preserved
# verbatim — matching only `([^)\s]+)\)` silently SKIPS every titled link, which
# means the outbound half leaves it at the wrong depth and the inbound half
# leaves it dangling after the git rm, both at exit 0. test-markdown-links.sh
# does handle titles, so a stricter parser here would disagree with the gate that
# judges the result.
LINK_RE = re.compile(r'(\]\()([^)\s]+)((?:\s+"[^"]*"|\s+\'[^\']*\')?\))')
REFDEF_RE = re.compile(r'(^\s*\[[^\]]+\]:\s+)(\S+)', re.MULTILINE)

def is_relative(t):
    if not t or t.startswith(("#", "/")):
        return False
    return "://" not in t and not t.startswith(("mailto:", "tel:"))

def split_frag(t):
    for sep in ("#", "?"):
        if sep in t:
            i = t.index(sep)
            return t[:i], t[i:]
    return t, ""

def rebase(target, prefix):
    """Rewrite a link written relative to <prefix>/ so it reads from one level up."""
    if not is_relative(target):
        return target
    path, frag = split_frag(target)
    if not path:
        return target
    new = posixpath.normpath(posixpath.join(prefix, path))
    # normpath eats a trailing slash; preserve it so directory links still read
    # as directory links.
    if path.endswith("/") and not new.endswith("/"):
        new += "/"
    return new + frag

def rewrite_links(text, fn):
    text = LINK_RE.sub(lambda m: m.group(1) + fn(m.group(2)) + m.group(3), text)
    return REFDEF_RE.sub(lambda m: m.group(1) + fn(m.group(2)), text)

if MODE == "redepth":
    # argv: redepth <body-file> <category-dir-name>
    body = open(sys.argv[2], encoding="utf-8").read()
    cat = sys.argv[3]
    # Flip the entry's Status line on the way in: applied.md's header spec says
    # "Archive moves immediately on Status -> applied", but this script moved
    # bodies VERBATIM, so every archival landed a `Status: open` inside the
    # applied archive (5 accumulated before the sweep that added this). Anchored
    # to the whole line so prose mentioning the literal string is untouched.
    # [ \t], not \s: in multiline mode \s*$ also eats the NEWLINE(S) after the
    # line, deleting the blank separator below the Status line (CR repro).
    body = re.sub(r'(?m)^([ \t]*-?[ \t]*)Status: open[ \t]*$',
                  r'\1Status: applied (flipped at archival)', body)
    sys.stdout.write(rewrite_links(body, lambda t: rebase(t, cat)))

elif MODE in ("inbound-scan", "inbound-fix"):
    # argv: <mode> <entry-relpath> <applied-relpath> <file>...
    entry = sys.argv[2]
    applied = sys.argv[3]
    files = sys.argv[4:]
    entry_base = posixpath.basename(entry)
    for f in files:
        if os.path.abspath(f) == os.path.abspath(entry):
            continue
        try:
            text = open(f, encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        if entry_base not in text:
            continue
        fdir = posixpath.dirname(f.replace(os.sep, "/")) or "."
        changed = []

        def fix(target):
            if not is_relative(target):
                return target
            path, frag = split_frag(target)
            resolved = posixpath.normpath(posixpath.join(fdir, path))
            if resolved != posixpath.normpath(entry):
                return target
            # Repoint at applied.md, expressed from THIS referrer's own depth.
            # applied.md has no per-entry anchors, so any fragment is dropped
            # rather than carried onto a target that cannot honour it.
            new = posixpath.relpath(applied, fdir).replace(os.sep, "/")
            changed.append(target)
            return new

        new_text = rewrite_links(text, fix)
        if changed and MODE == "inbound-fix":
            with open(f, "w", encoding="utf-8") as fh:
                fh.write(new_text)
        for c in changed:
            print("LINK\t%s\t%s" % (f, c))

        # Any surviving mention of the filename that is NOT a markdown link we
        # just repointed — a code span, a bare path in prose. These do not dangle
        # (the link gate does not follow code spans, per the
        # backlog-code-span-paths-unchecked entry), so they are reported as
        # advisory rather than blocking: only a human can restate the prose.
        for i, line in enumerate(new_text.splitlines(), 1):
            if entry_base in line:
                print("MENTION\t%s\t%s\t%s" % (f, i, line.strip()[:160]))
PY
}

# ── Selftest ─────────────────────────────────────────────────────────────────
# Proves the re-depth rule on the exact shapes the backlog entry recorded from
# the live breakage, so a regression here fails loudly rather than at CI.
if [ "$SELFTEST" = true ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cat > "$tmp/body.md" <<'MD'
- see [`ship-loops.md`](../../../agent-rules/ship-loops.md)
- see [`script-freshness.sh`](../../../../agents/scripts/core/lib/script-freshness.sh)
- see [sibling](../tooling/2026-08-06-some-entry.md)
- see [applied](../applied.md)
- see [same-dir](2026-08-07-other.md)
- external [x](https://example.com/a.md) and anchor [y](#z)
- prose mentioning Status: open mid-line stays untouched
- Status: open

After-blank prose survives.
  Status: open
  Last-reviewed: 2026-08-11
MD
    got="$(archive_py redepth "$tmp/body.md" process)"
    fail=0
    check() {  # <needle>
        case "$got" in *"$1"*) ;; *) echo "  selftest MISS: $1" >&2; fail=1 ;; esac
    }
    check "](../../agent-rules/ship-loops.md)"
    check "](../../../agents/scripts/core/lib/script-freshness.sh)"
    check "](tooling/2026-08-06-some-entry.md)"
    check "](applied.md)"
    check "](process/2026-08-07-other.md)"
    check "](https://example.com/a.md)"
    check "](#z)"
    # Status flip: the LINE flips (indented AND list-marker forms), the mid-line
    # prose mention does not, and the BLANK line after a flipped Status survives
    # — `\s*$` in multiline mode also ate the newline(s) below the line, gluing
    # the next paragraph onto it (CR finding; pinned with [ \t]).
    check "  Status: applied (flipped at archival)"
    check "- Status: applied (flipped at archival)"
    check "prose mentioning Status: open mid-line stays untouched"
    # $'\n\n' — NOT $(printf '\n\n'), which command-substitutes to EMPTY (trailing
    # newlines are stripped), silently degrading this into a plain substring check
    # that passes even with the blank line eaten. Caught by this selftest failing.
    check "archival)"$'\n\n'"After-blank prose survives."
    case "$got" in
        *$'\n  Status: open'*|*$'\n- Status: open'*) echo "  selftest MISS: a Status line was NOT flipped to applied" >&2; fail=1 ;;
    esac
    # selftest: asserts-failure — the positive checks above only prove the happy
    # path, and a re-depth that silently no-ops would satisfy every one of them if
    # the harness itself were broken. These feed known-bad input and require a
    # REFUSAL, so the selftest can distinguish "the transform works" from "nothing
    # is being checked". (Same false-green shape this script exists to close: a
    # check that reports success without verifying anything.)
    neg_fail=0
    # Assert the REASON, not just a non-zero exit. A bare "it exited non-zero"
    # negative is vacuous here: remove any one guard and the script still dies
    # further downstream for an unrelated reason, so the assertion passes while
    # testing nothing. (Verified: all three of these passed with their own guard
    # deleted, until they were pinned to the specific refusal message.)
    expect_refusal() {  # <label> <stderr-fragment> <args...>
        local _label="$1" _want="$2"; shift 2
        local _err _rc=0
        _err="$( ( "$@" ) 2>&1 >/dev/null )" || _rc=$?
        if [ "$_rc" -eq 0 ]; then
            echo "  selftest NEGATIVE MISS: $_label was accepted, expected refusal" >&2
            neg_fail=1
            return
        fi
        case "$_err" in
            *"$_want"*) ;;
            *) echo "  selftest NEGATIVE MISS: $_label refused for the WRONG reason (wanted '$_want', got: ${_err%%$'\n'*})" >&2
               neg_fail=1 ;;
        esac
    }
    expect_refusal "path outside categories/<cat>/" "expected docs/self-improvement/categories" \
        bash "$ARCHIVE_DIR/archive-backlog-entry.sh" docs/self-improvement/categories/applied.md
    expect_refusal "missing entry file" "not found" \
        bash "$ARCHIVE_DIR/archive-backlog-entry.sh" docs/self-improvement/categories/process/no-such-entry.md
    expect_refusal "two entries at once" "one entry at a time" \
        bash "$ARCHIVE_DIR/archive-backlog-entry.sh" \
            docs/self-improvement/categories/process/a.md \
            docs/self-improvement/categories/process/b.md
    # The positive harness must itself be falsifiable: if `rebase` stopped
    # normalizing, this un-normalized form would survive and every check() above
    # would still pass, since they only look for the correct strings.
    case "$got" in
        *"](process/../../agent-rules/ship-loops.md)"*)
            echo "  selftest NEGATIVE MISS: un-normalized path present; rebase did not normalize" >&2
            neg_fail=1 ;;
    esac

    if [ "$fail" -ne 0 ] || [ "$neg_fail" -ne 0 ]; then
        echo "archive-backlog-entry --selftest: FAIL" >&2
        exit 1
    fi
    echo "archive-backlog-entry --selftest: PASS — re-depth correct for parent-escaping, sibling-category, same-dir, applied.md, external and anchor links; bad paths, missing files and multi-entry invocations all refused."
    exit 0
fi

[ -n "$ENTRY" ] || { echo "usage: $0 [--dry-run] <path-to-entry.md> | --selftest" >&2; exit 2; }
[ -f "$ENTRY" ] || { echo "archive-backlog-entry: '$ENTRY' not found" >&2; exit 2; }

# Normalise to a repo-relative posix path.
ENTRY="${ENTRY#./}"
case "$ENTRY" in
    docs/self-improvement/categories/*/*.md) ;;
    *) echo "archive-backlog-entry: expected docs/self-improvement/categories/<cat>/<file>.md (got '$ENTRY')" >&2; exit 2 ;;
esac

CAT_DIR="$(dirname "$ENTRY")"          # docs/self-improvement/categories/<cat>
CAT="$(basename "$CAT_DIR")"           # <cat>
APPLIED="$(dirname "$CAT_DIR")/applied.md"
[ -f "$APPLIED" ] || { echo "archive-backlog-entry: '$APPLIED' not found" >&2; exit 2; }

echo "archive-backlog-entry: $ENTRY -> $APPLIED (re-depth prefix '$CAT/')"

# ── Step 0: everything that can REFUSE, before anything is written ────────────
# The inbound pass rewrites referrers on disk and the append is not reversible by
# this script, so every refusal has to happen before either. Ordering these after
# the first write is what leaves a tree half-archived, which is worse than any
# clean failure: a retry double-appends.
body="$(archive_py redepth "$ENTRY" "$CAT")"
[ -n "$body" ] || { echo "archive-backlog-entry: re-depth produced an empty body — refusing to archive" >&2; exit 2; }

entry_tracked=true
git ls-files --error-unmatch -- "$ENTRY" >/dev/null 2>&1 || entry_tracked=false

# ── Step 1: inbound references ───────────────────────────────────────────────
# UNTRACKED markdown counts: a doc written and archived in the same session is
# not in `git ls-files` yet, and silently orphaning it is the exact failure this
# script exists to prevent. `--others --exclude-standard` adds untracked files
# while still honouring .gitignore.
mapfile -t md_files < <(
    { git ls-files '*.md' 2>/dev/null
      git ls-files --others --exclude-standard '*.md' 2>/dev/null
    } | sort -u
)
if [ "${#md_files[@]}" -eq 0 ]; then
    mapfile -t md_files < <(find . -name '*.md' -not -path './.git/*' | sed 's|^\./||')
fi
inbound_mode="inbound-fix"
[ "$DRY_RUN" = true ] && inbound_mode="inbound-scan"
inbound="$(archive_py "$inbound_mode" "$ENTRY" "$APPLIED" "${md_files[@]}")"

n_links=0
n_mentions=0
touched=()
while IFS=$'\t' read -r kind f rest; do
    [ -n "$kind" ] || continue
    case "$kind" in
        LINK)
            n_links=$((n_links+1))
            # Track the exact referrers so staging can name them. `git add -u`
            # would stage every modified tracked file in the tree, sweeping
            # unrelated in-progress work into the archival commit.
            #
            # Element-wise, not a flattened `" ${touched[*]} "` substring test:
            # that joins on spaces, so a path CONTAINING a space would match a
            # different pair of entries and the referrer would silently not be
            # staged — a rewritten-but-uncommitted file, which is the quiet half
            # of the bug this script exists to fix.
            _seen=false
            for _t in ${touched[@]+"${touched[@]}"}; do
                if [ "$_t" = "$f" ]; then _seen=true; break; fi
            done
            [ "$_seen" = true ] || touched+=("$f")
            echo "  INBOUND-LINK  $f -> repointed at applied.md (was '$rest')" ;;
        MENTION)
            n_mentions=$((n_mentions+1))
            echo "  INBOUND-PROSE $f:$rest" >&2 ;;
    esac
done <<< "$inbound"

[ "$n_links" -gt 0 ] && echo "  $n_links inbound link(s) repointed at applied.md"
if [ "$n_mentions" -gt 0 ]; then
    echo "archive-backlog-entry: WARN $n_mentions prose/code-span mention(s) of this entry remain (listed above). They do not dangle, but they now name a file that is gone — restate them against applied.md." >&2
fi

if [ "$DRY_RUN" = true ]; then
    echo "archive-backlog-entry: --dry-run — nothing written. Re-depthed body would be:"
    printf '%s\n' "$body"
    exit 0
fi

# VERIFY the guarantee rather than assume it. The fix pass above rewrites what it
# recognises; this re-scans and refuses to delete if any inbound LINK still
# resolves to the entry — a link shape the parser missed would otherwise be
# orphaned silently at exit 0, which is the original bug wearing a new mask.
residual="$(archive_py inbound-scan "$ENTRY" "$APPLIED" "${md_files[@]}" | grep '^LINK' || true)"
if [ -n "$residual" ]; then
    echo "archive-backlog-entry: inbound link(s) still resolve to $ENTRY after the rewrite pass — refusing to delete:" >&2
    printf '%s\n' "$residual" >&2
    exit 3
fi

# ── Step 2: append the re-depthed body, then remove the source ───────────────
# $body and $entry_tracked were computed in Step 0, before anything was written.
# A blank separator line keeps entries from running together in the ledger.
printf '\n%s\n' "$body" >> "$APPLIED"
if [ "$entry_tracked" = true ]; then
    # -f is correct, not a shortcut: the local modification is the whole point
    # (the Status flip), and the WORKING-TREE body is what was just appended, so
    # nothing is lost by discarding the file.
    git rm -q -f "$ENTRY"
else
    rm -f "$ENTRY"
fi
git add "$APPLIED"
if [ "${#touched[@]}" -gt 0 ]; then
    git add -- "${touched[@]}"
fi

# ── Step 2b: keep the head bounded ───────────────────────────────────────────
# Rotate months older than current+previous out of applied.md into flat
# applied-YYYY-MM.md siblings (see rotate-applied-md.sh header for why flat).
# Runs after every append so the head stays bounded by construction; stages
# whatever the rotation touched.
bash "$(dirname "${BASH_SOURCE[0]}")/rotate-applied-md.sh"
git add "$APPLIED" "$(dirname "$APPLIED")"/applied-*.md 2>/dev/null || true

# ── Step 3: the self-check that would have caught both halves ────────────────
if [ "${ARCHIVE_SKIP_LINK_CHECK:-0}" = "1" ]; then
    echo "archive-backlog-entry: link self-check SKIPPED (ARCHIVE_SKIP_LINK_CHECK=1)"
else
    echo "archive-backlog-entry: running test-markdown-links.sh --all (the --all is load-bearing — diff scope sees neither half)"
    if ! bash "$ARCHIVE_DIR/test-markdown-links.sh" --all; then
        echo "archive-backlog-entry: post-archive link check FAILED — the archival is staged but broke links; fix before committing." >&2
        exit 4
    fi
fi

echo "archive-backlog-entry: archived $ENTRY into $APPLIED ($n_links inbound link(s) repointed)."

if command -v warn_if_script_stale >/dev/null 2>&1; then
    SCRIPT_FRESHNESS_ROOT_HINT="$ARCHIVE_DIR" \
        warn_if_script_stale "archive-backlog-entry logic" \
            "agents/scripts/core/archive-backlog-entry.sh" \
            "agents/scripts/core/lib/script-freshness.sh"
fi
exit 0
