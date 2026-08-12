#!/usr/bin/env bash
# test-plan-index.sh — keep the shipped-plan index in sync with the archive dir.
#
# Regenerates the table of shipped/archived plans from the actual *.md files in
# the archive directory, into the index file between the markers
#   <!-- BEGIN auto-plan-index --> ... <!-- END auto-plan-index -->
# Surrounding prose (the human §Notes etc.) is never touched.
#
# Modes:
#   (default) / --check   regenerate in memory, diff vs committed; exit 1 on drift
#   --fix                 rewrite the block in place from the live archive
#
# Per row: slug (filename), link (relative href), approx date (first-commit date
# via `git log --follow`), one-line summary (an in-file
# `<!-- index-summary: ... -->` override if present, else the plan's H1 title).
# Output is byte-stable (no timestamps) so --check is deterministic.
#
# Paths are config-driven below — the Phase-D rename (design/archive -> plans/
# shipped, BACKLOG_PLANS.md -> plans/INDEX.md) is a two-line edit here.
set -uo pipefail

# Resolve paths against the git root, not $PWD. When --fix is invoked from a
# subdir or a sibling worktree, a relative ARCHIVE_DIR/INDEX_FILE would either
# silently NO-OP (archive dir not found here) or index the WRONG tree. cd to the
# toplevel so the live archive + INDEX are always THIS repo's
# (plan-index-fix-wrong-cwd-silent-noop). Honour an explicit absolute override.
#
# Capture our OWN dir as an ABSOLUTE path BEFORE the cd: the later
# `. "$_SCRIPT_DIR/lib/..."` source and the self-re-invocation (`"$_SCRIPT_PATH"
# --check`) both rely on $0, which is relative for a relative invocation from a
# subdir — after `cd "$_GIT_ROOT"` a relative $0 no longer resolves. Resolve
# once here while $PWD is still the caller's dir.
_SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
_SCRIPT_PATH="$_SCRIPT_DIR/$(basename "$0")"
_GIT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || _GIT_ROOT=""
if [ -n "$_GIT_ROOT" ]; then
  cd "$_GIT_ROOT" || { echo "test-plan-index: cannot cd to git root '$_GIT_ROOT'" >&2; exit 2; }
fi

ARCHIVE_DIR="${PLAN_INDEX_ARCHIVE_DIR:-docs/plans/shipped}"
INDEX_FILE="${PLAN_INDEX_FILE:-docs/plans/INDEX.md}"
BEGIN_MARK="<!-- BEGIN auto-plan-index -->"
END_MARK="<!-- END auto-plan-index -->"

# Shared shallow-clone guard: on a shallow clone, git-log --follow dates drift
# from CI (which runs on full history), so a local --fix would commit an INDEX
# the required check rejects. Sourced lib auto-unshallows under --fix (matching
# CI) or refuses; WARNs under --check. See lib/plan-history-guard.sh header.
# shellcheck source=lib/plan-history-guard.sh
. "$_SCRIPT_DIR/lib/plan-history-guard.sh"

MODE="check"
for a in "$@"; do
  case "$a" in
    --fix) MODE="fix" ;;
    --check) MODE="check" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: $0 [--check|--fix|--selftest]" >&2; exit 2 ;;
  esac
done

# --selftest — prove (1) the script resolves paths against the git root (so a
# subdir invocation can't silent-NO-OP), and (2) the shallow-clone guard's
# check-mode WARN/refuse contract holds. No network; uses the live repo's own
# non-shallow state for the guard smoke. Asserts-behaviour, not snapshots.
if [ "$MODE" = "selftest" ]; then
  fail=0
  # (1) git-root resolution: ARCHIVE_DIR must resolve to an existing dir AFTER
  # the cd above, regardless of where the selftest was launched from.
  if [ ! -d "$ARCHIVE_DIR" ]; then
    echo "test-plan-index --selftest: FAIL — ARCHIVE_DIR '$ARCHIVE_DIR' not found after git-root cd" >&2
    fail=1
  fi
  # (2) guard contract: check-mode must NEVER exit non-zero on this (non-shallow)
  # repo, and the function must be defined by the sourced lib.
  if ! type is_shallow_or_refuse >/dev/null 2>&1; then
    echo "test-plan-index --selftest: FAIL — is_shallow_or_refuse not sourced" >&2
    fail=1
  else
    ( is_shallow_or_refuse check ) || {
      echo "test-plan-index --selftest: FAIL — guard check-mode returned non-zero on non-shallow repo" >&2
      fail=1
    }
  fi
  # (3) negative case.
  # selftest: asserts-failure — feed known-bad input (a non-existent archive
  # dir) and require a NON-ZERO exit. A regression that makes the script silently
  # NO-OP on a missing/wrong archive dir (the wrong-cwd class) would wrongly
  # succeed here. Re-invoke ourselves with the override so the real arg-parse +
  # archive-dir check runs end-to-end.
  #
  # TWO defects were fixed in this assertion; both made it unable to fail.
  #
  #   1. It executed $_SCRIPT_PATH directly. This file is mode 100644 in git, so
  #      that exec fails with 126 (Permission denied) EVERYWHERE, including CI —
  #      and 126 is non-zero, so the assertion was satisfied by the permission
  #      error rather than by the archive-dir guard. Hence `bash "$..."`.
  #   2. It accepted ANY non-zero status. With the guard deleted the script still
  #      exits non-zero, on an unhandled listdir traceback — so it stayed green
  #      even with the behaviour it names removed. Hence matching the REFUSAL
  #      MESSAGE, not merely the status.
  #
  # Both verified by deleting the guard and re-running: green before, red after.
  # An `asserts-failure` marker only means a negative exists; it cannot tell that
  # the negative is reachable for the stated reason.
  _st_neg="$(PLAN_INDEX_ARCHIVE_DIR="docs/plans/__no_such_archive_dir__$$" \
       bash "$_SCRIPT_PATH" --check 2>&1)"
  _st_neg_rc=$?
  if [ "$_st_neg_rc" -eq 0 ]; then
    echo "test-plan-index --selftest: FAIL — a missing archive dir did NOT fail (silent NO-OP)" >&2
    fail=1
  elif ! printf '%s' "$_st_neg" | grep -q "archive dir .* not found"; then
    echo "test-plan-index --selftest: FAIL — a missing archive dir failed for the WRONG reason (wanted the archive-dir refusal, got: $(printf '%s' "$_st_neg" | head -1))" >&2
    fail=1
  fi
  # (4) plan-date marker (plan-index-date-derived-from-mutable-git-history).
  # Both cases run in a THROWAWAY repo whose plan has exactly ONE commit dated
  # today — the post-squash shape, where `git log --follow` returns the squash
  # date rather than the plan's real first-commit date. That is the state which
  # reddened `develop` on #1937, and it cannot be staged inside this repo.
  # NB: every assertion below propagates with an explicit `|| exit N`, and the
  # subshell's status is captured on its OWN line. `set -e` inside a subshell
  # that is the LEFT operand of `||` is SUPPRESSED — the shell disables it for
  # any command in a `&&`/`||` list — so a `set -e` version of this block ran
  # every step regardless of failure and reported only the LAST command's
  # status. It passed with the fix disabled. Distinct exit codes name which
  # assertion broke, so a future failure does not need re-derivation.
  # The temp dir is REQUIRED to be non-empty before the subshell runs. `cd ""`
  # SUCCEEDS in bash (it is a no-op), so an unchecked `mktemp -d` failure would
  # run this whole fixture in the repo root — re-`git init`, overwrite the real
  # INDEX with a 4-line stub, then `git add -A && git commit` the entire tree.
  _st_tmp="$(mktemp -d)" || _st_tmp=""
  if [ -z "$_st_tmp" ] || [ ! -d "$_st_tmp" ]; then
    echo "test-plan-index --selftest: FAIL — could not create a temp dir for the marker fixture" >&2
    fail=1
  else
  (
    cd "$_st_tmp" || exit 90
    git init -q || exit 90
    git config user.email t@t.t || exit 90
    git config user.name t || exit 90
    # A global commit.gpgsign would fail every commit here and report a fixture
    # build error instead of the assertion (same guard the other throwaway-repo
    # fixtures carry — test-archive-plan.sh, historical_review_survivors.bats).
    git config commit.gpgsign false || exit 90
    # Paths are BUILT, never written as `docs/plans/<tier>/<slug>.md` literals:
    # test-plan-ref-integrity.sh greps every file in the repo for that shape and
    # resolves it against the real archive, so a fixture literal reads as a
    # dangling plan reference and REDS the required doc job. (It did — caught
    # pre-push; the literal form failed that gate on this branch and passed at
    # HEAD.)
    _pd="docs/plans"
    _shipped="$_pd/shipped"
    mkdir -p "$_shipped" || exit 90
    printf '# Plan — Marked\n\n<!-- plan-date: 2020-01-02 -->\n\nBody.\n' > "$_shipped/marked.md" || exit 90
    # An IMPOSSIBLE date matches the marker's shape but is not a calendar date.
    # It must be treated as absent (else a typo pins the row permanently, and
    # --fix can never replace it) and REPLACED in place rather than joined by a
    # second marker.
    printf '# Plan — Bad\n\n<!-- plan-date: 2026-99-99 -->\n\nBody.\n' > "$_shipped/bad.md" || exit 90
    # A MALFORMED declaration (not even date-shaped) must also be normalised to a
    # single valid marker. Only shape-matching values were replaced before, so
    # 2026-99-99 was fixed while `invalid` was left beside a second inserted
    # marker — the same contradiction, reached by a different payload.
    printf '# Plan — Junk\n\n<!-- plan-date: invalid -->\n\nBody.\n' > "$_shipped/junk.md" || exit 90
    # An unstamped plan that this change does NOT touch. Stamping is scoped to
    # the diff, so it must come out UNCHANGED — see (4d).
    printf '# Plan — Untouched\n\nBody.\n' > "$_shipped/untouched.md" || exit 90
    printf '# INDEX\n\n<!-- BEGIN auto-plan-index -->\n<!-- END auto-plan-index -->\n' > "$_pd/INDEX.md" || exit 90
    git add -A || exit 90
    git commit -qm only-commit-is-today || exit 90
    # The plans under test have to be IN the change for --fix to stamp them,
    # which is the real shape too: a plan being archived is part of the diff.
    # They must still be COMMITTED first — an untracked plan has no history at
    # any path, so no date resolves and there is nothing to stamp.
    printf 'Touched.\n' >> "$_shipped/bad.md" || exit 90
    printf 'Touched.\n' >> "$_shipped/junk.md" || exit 90
    # (4a) STABILITY: the row must carry the MARKER date, not the (today) commit
    # date, even though the file's entire history is that one commit.
    bash "$_SCRIPT_PATH" --fix >/dev/null 2>&1 || exit 91
    grep -q '| 2020-01-02 |' "$_pd/INDEX.md" || exit 92
    # (4c) INVALID marker: not authoritative, and replaced rather than duplicated.
    grep -q '2026-99-99' "$_pd/INDEX.md" && exit 94
    [ "$(grep -c 'plan-date' "$_shipped/bad.md")" = "1" ] || exit 95
    grep -q 'plan-date: 2026-99-99' "$_shipped/bad.md" && exit 95
    [ "$(grep -c 'plan-date' "$_shipped/junk.md")" = "1" ] || exit 96
    grep -q 'plan-date: invalid' "$_shipped/junk.md" && exit 96
    # (4d) SCOPE: an unstamped plan OUTSIDE the change must not be rewritten.
    # Unscoped, --fix stamped every unstamped plan, so the CI autosync pushed
    # all 188 back-catalogue markers onto the first PR branch that triggered it
    # — 192 files, past CodeRabbit's 100-file limit, and the required CR gate
    # could never pass. Observed on PR #1999.
    grep -q 'plan-date' "$_shipped/untouched.md" && exit 97
    # (4e) and the one-shot migration escape still reaches it.
    SMATCHET_PLAN_STAMP_ALL=1 bash "$_SCRIPT_PATH" --fix >/dev/null 2>&1 || exit 98
    grep -q 'plan-date' "$_shipped/untouched.md" || exit 98
    # (4f) marker injection: a summary carrying the literal END marker must be
    # REFUSED (exit 2 naming the plan), not written into the block — once written,
    # content.index(end) splits at the injected copy and --fix corrupts INDEX.md
    # further on every run, with no fixed point. The marker string is BUILT here,
    # never written literally, so this file cannot inject into the real INDEX.
    _mk="$(printf '%s END auto-plan-index %s' '<!--' '-->')"
    printf '# Plan — Evil\n\n<!-- index-summary: x %s y -->\n\nBody.\n' "$_mk" > "$_shipped/evil.md" || exit 90
    _inj_out="$(bash "$_SCRIPT_PATH" --fix 2>&1)"; _inj_rc=$?
    rm -f "$_shipped/evil.md"
    [ "$_inj_rc" = "2" ] || exit 99
    case "$_inj_out" in *REFUSING*evil.md*) ;; *) exit 99 ;; esac
    # (4b) DISAGREEMENT: a marker that no longer matches its committed row must
    # FAIL --check rather than be silently re-derived from git.
    sed -i.bak 's/plan-date: 2020-01-02/plan-date: 2019-05-05/' "$_shipped/marked.md" || exit 90
    rm -f "$_shipped/marked.md.bak"
    if bash "$_SCRIPT_PATH" --check >/dev/null 2>&1; then exit 93; fi
    exit 0
  )
  _st_rc=$?
  case "$_st_rc" in
    0)  ;;
    90) echo "test-plan-index --selftest: FAIL — could not build the marker fixture repo" >&2; fail=1 ;;
    91) echo "test-plan-index --selftest: FAIL — --fix errored on the marker fixture" >&2; fail=1 ;;
    92) echo "test-plan-index --selftest: FAIL — plan-date marker NOT authoritative: a squash-shaped history (single commit, today) moved the row off the stamped date" >&2; fail=1 ;;
    93) echo "test-plan-index --selftest: FAIL — a marker disagreeing with its committed row did not fail --check" >&2; fail=1 ;;
    94) echo "test-plan-index --selftest: FAIL — an impossible plan-date (2026-99-99) was treated as authoritative" >&2; fail=1 ;;
    95) echo "test-plan-index --selftest: FAIL — an impossible plan-date was not REPLACED in place (duplicate or surviving marker)" >&2; fail=1 ;;
    96) echo "test-plan-index --selftest: FAIL — a MALFORMED plan-date declaration was not replaced in place (duplicate or surviving marker)" >&2; fail=1 ;;
    97) echo "test-plan-index --selftest: FAIL — --fix stamped a plan OUTSIDE the change; unscoped, this makes CI autosync push the whole back catalogue onto a PR branch (PR #1999: 192 files, CR review skipped, required gate wedged)" >&2; fail=1 ;;
    98) echo "test-plan-index --selftest: FAIL — SMATCHET_PLAN_STAMP_ALL=1 did not stamp an untouched plan (the one-shot migration escape is broken)" >&2; fail=1 ;;
    99) echo "test-plan-index --selftest: FAIL — a summary containing the literal END marker was not refused (it would corrupt the generated INDEX block with no fixed point)" >&2; fail=1 ;;
    *)  echo "test-plan-index --selftest: FAIL — marker fixture exited $_st_rc" >&2; fail=1 ;;
  esac
  rm -rf "$_st_tmp"
  fi

  if [ "$fail" = "0" ]; then
    echo "test-plan-index --selftest: PASS (git-root path resolution + shallow guard + missing-archive refusal + plan-date marker authority)"
    echo "Passed: 1  Failed: 0"
    exit 0
  fi
  echo "Passed: 0  Failed: 1"
  exit 1
fi

# Guard the real run: shallow clones drift git-log dates from CI. Under --fix
# this auto-unshallows (or hard-refuses, exit 2); under --check it WARNs.
is_shallow_or_refuse "$MODE"

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""; for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
[ -z "$PY" ] && { echo "test-plan-index: python not found" >&2; exit 2; }

"$PY" - "$ARCHIVE_DIR" "$INDEX_FILE" "$BEGIN_MARK" "$END_MARK" "$MODE" <<'PY'
import datetime, os, re, subprocess, sys

archive_dir, index_file, begin, end, mode = sys.argv[1:6]

def _git_first_date_at(git_path):
    try:
        out = subprocess.run(["git", "log", "--follow", "--format=%ad", "--date=short", "--", git_path],
                             capture_output=True, text=True, check=False).stdout.strip().splitlines()
        return out[-1] if out else ""
    except Exception:
        return ""

DATE_RE = re.compile(r'^\s*<!--\s*plan-date:\s*(\d{4}-\d{2}-\d{2})\s*-->\s*$')
# Any plan-date DECLARATION, whatever its payload. Reading uses DATE_RE (a valid
# calendar date or nothing); REPLACING uses this, so a malformed marker is
# normalised rather than left beside a freshly inserted one. Without the split,
# `2026-99-99` was replaced (it matches the digit shape) but `invalid` was not —
# an arbitrary difference that left the same two-marker contradiction the
# in-place replacement exists to prevent.
PLAN_DATE_DECL_RE = re.compile(r'^\s*<!--\s*plan-date:.*?-->\s*$')

def marker_date(path):
    """The plan's own stamped date, or "" — the AUTHORITATIVE source when present.

    Why this exists (plan-index-date-derived-from-mutable-git-history, tooling P1):
    every other source for this value is git metadata that the merge itself
    rewrites. A squash-merge collapses the branch, so `git log --follow` at the
    new path finds exactly ONE commit and returns the SQUASH date. When branch
    work and the merge fall on different calendar days -- routine for an evening
    merge -- the row the PR committed and the row CI regenerates disagree, and
    the required `Doc anchors + agent contract` check goes RED on the develop tip
    the instant the PR lands, with no pre-merge state that could have passed.
    Under block-on-any-red that red is inherited by every open PR.

    Content in the file survives squash, shallow clone and staged rename
    identically, which none of the git lookups do.
    """
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = DATE_RE.match(line)
                if m:
                    # The shape regex alone accepts 2026-99-99. An impossible date
                    # would then become AUTHORITATIVE and, being a present marker,
                    # would never be re-stamped by --fix — a typo that pins the row
                    # permanently. Treat an unparseable date as ABSENT so the git
                    # fallback and the stamping path both still apply.
                    try:
                        datetime.date(*(int(p) for p in m.group(1).split("-")))
                    except ValueError:
                        return ""
                    return m.group(1)
    except OSError:
        pass
    return ""

def git_first_date(path):
    git_path = path.replace(os.sep, "/")
    d = _git_first_date_at(git_path)
    if not d:
        # A freshly `git mv`'d plan (active/ -> shipped/) has NO committed history at the
        # NEW path while the rename is only STAGED, so `git log --follow <new-path>` is
        # empty and the row gets a blank "—" date — which CI then re-fixes to the real
        # first-add date, drifting the committed index (the #1061 / #1092 archive
        # date-drift, twice). Fall back to the SIBLING tier: the rename source still
        # carries the history. Covers active<->shipped in both directions.
        if "/plans/shipped/" in git_path:
            d = _git_first_date_at(git_path.replace("/plans/shipped/", "/plans/active/"))
        elif "/plans/active/" in git_path:
            d = _git_first_date_at(git_path.replace("/plans/active/", "/plans/shipped/"))
    return d or "—"

def summary_for(path):
    h1 = ""
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'^\s*<!--\s*index-summary:\s*(.+?)\s*-->\s*$', line)
            if m:
                return m.group(1).strip()
            if not h1 and line.startswith("# "):
                h1 = line[2:].strip()
    return h1 or "_(summary TODO)_"

if not os.path.isdir(archive_dir):
    print("test-plan-index: archive dir %s not found" % archive_dir, file=sys.stderr); sys.exit(2)

PLACEHOLDER = "—"  # em-dash: emitted when no first-commit date resolves.

def _touched_plans():
    """Plans this change actually touches, vs origin/develop + working tree.

    STAMPING SCOPE, and it is load-bearing. `--fix` stamping every unstamped
    plan turns the CI autosync into a bulk migration: with all 188 archived
    plans unstamped, the first PR to trigger autosync had 188 marker commits
    pushed onto its branch by github-actions[bot]. That took PR #1999 to 192
    files, CodeRabbit refuses to review past 100, and the required CR gate can
    then never pass — the PR wedges with no self-healing path. Observed, not
    theorised: it happened to the very PR that introduced the marker.

    The behaviour worth keeping is narrow — a plan being ARCHIVED in this change
    gets its date pinned before the squash can rewrite it — and that plan is by
    definition in the diff. Migrating the back catalogue is a deliberate one-shot
    (`SMATCHET_PLAN_STAMP_ALL=1` in the environment — there is no CLI flag; the
    arg parser rejects unknown options), reviewed on its own, not a side effect
    of touching a doc.

    An EMPTY set on git failure is the safe answer: it stamps nothing rather
    than everything, so a git hiccup cannot re-trigger the mass rewrite.
    """
    touched = set()
    for args in (["diff", "--name-only", "origin/develop...HEAD"],
                 ["diff", "--name-only", "HEAD"],
                 ["ls-files", "--others", "--exclude-standard"]):
        try:
            out = subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, OSError):
            continue
        touched.update(p.strip() for p in out.splitlines() if p.strip())
    return touched


STAMP_ALL = os.environ.get("SMATCHET_PLAN_STAMP_ALL") == "1"
touched_plans = None if STAMP_ALL else _touched_plans()

rows = []
placeholder_slugs = []
unstamped = []   # (path, resolved_date) for plans with no plan-date marker yet
for fn in sorted(os.listdir(archive_dir)):
    if not fn.endswith(".md") or fn.startswith("_"):
        continue
    slug = fn[:-3]
    path = os.path.join(archive_dir, fn)
    # Marker FIRST; git only for plans not yet stamped (legacy, or a plan being
    # archived in this very run). See marker_date().
    date = marker_date(path)
    if not date:
        date = git_first_date(path)
        if date != PLACEHOLDER:
            # Row still reports the git-derived date either way; only WRITING is
            # scoped. An out-of-scope plan simply stays unstamped, exactly as it
            # is on develop today.
            if touched_plans is None or path.replace(os.sep, "/") in touched_plans:
                unstamped.append((path, date))
    if date == PLACEHOLDER:
        placeholder_slugs.append(slug)
    summ = summary_for(path).replace("|", "\\|")
    # A summary/H1 carrying the literal block markers would be written into the
    # generated block, and the next run's content.index(end) would split at the
    # INJECTED marker inside the row — every subsequent --fix appends garbage and
    # --check has no fixed point (verified by repro: fix -> DRIFT -> fix appends
    # a duplicate row-fragment + second END marker; the printed "run --fix"
    # remedy makes it strictly worse, and under a PLAN_INDEX_PAT the autosync
    # would loop bot-commits). Filenames are covered by the kebab-case naming
    # gate; summary CONTENT has no other gate, so refuse here, naming the plan.
    if begin in summ or end in summ:
        print("test-plan-index: REFUSING — %s's index-summary/H1 contains the "
              "auto-plan-index block marker; remove it (it would corrupt the "
              "generated INDEX block irrecoverably)" % path, file=sys.stderr)
        sys.exit(2)
    # link relative to the index file's directory
    href = os.path.relpath(path, os.path.dirname(index_file)).replace(os.sep, "/")
    rows.append((date, slug, "| [`%s`](%s) | %s | %s |" % (slug, href, date, summ)))

rows.sort(key=lambda r: (r[0], r[1]))
block = [begin,
         "<!-- Generated by agents/scripts/core/test-plan-index.sh — do not hand-edit between the markers.",
         "     Override a row's summary with an `<!-- index-summary: ... -->` comment in the plan file. -->",
         "",
         "| Plan (slug) | Approx. date | One-line summary |",
         "|---|---|---|"]
block += [r[2] for r in rows]
block.append(end)
new_block = "\n".join(block)

with open(index_file, encoding="utf-8") as f:
    content = f.read()

if begin not in content or end not in content:
    print("test-plan-index: markers not found in %s" % index_file, file=sys.stderr)
    print("  add a '%s' / '%s' pair where the shipped-plan table should live." % (begin, end), file=sys.stderr)
    sys.exit(2)

pre = content[:content.index(begin)]
post = content[content.index(end) + len(end):]
rebuilt = pre + new_block + post

def stamp_markers():
    """Write `<!-- plan-date: YYYY-MM-DD -->` into plans that lack one.

    Only under --fix, and only from a date git actually resolved -- never the
    placeholder. Placed directly after the H1 so it sits with the other
    machine-read marker (`index-summary`) rather than at an arbitrary offset.
    Idempotent: a plan that already carries a marker is never in `unstamped`.
    """
    stamped = []
    for path, date in unstamped:
        with open(path, encoding="utf-8") as f:
            lines = f.readlines()
        marker = "<!-- plan-date: %s -->\n" % date
        # A plan can reach here WITH a marker already present: marker_date()
        # rejects an impossible date like 2026-99-99, so the plan counts as
        # unstamped. REPLACE that line rather than inserting a second marker —
        # two markers in one file read as a contradiction to anyone opening it,
        # even though the first (valid) one would win.
        replaced = False
        for i, line in enumerate(lines):
            if PLAN_DATE_DECL_RE.match(line):
                lines[i] = marker
                replaced = True
                break
        if replaced:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.writelines(lines)
            stamped.append(os.path.basename(path))
            continue
        ins = 0
        for i, line in enumerate(lines):
            if line.startswith("# "):
                ins = i + 1
                break
        # Keep a blank line between the H1 and the marker when the H1 is
        # immediately followed by prose, so the rendered page is unchanged.
        block = ["\n", marker] if ins > 0 and ins < len(lines) and lines[ins].strip() else [marker]
        lines[ins:ins] = block
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.writelines(lines)
        stamped.append(os.path.basename(path))
    return stamped

if rebuilt == content:
    # The index is already correct, but plans may still be UNSTAMPED — that is
    # the migration case, and also the case for a plan archived by this very run
    # whose git date happens to match. Stamping is what makes the date immune to
    # the next squash, so --fix must still do it.
    if mode == "fix" and unstamped:
        stamped = stamp_markers()
        print("test-plan-index: index up to date (%d plans); stamped plan-date into %d plan(s)"
              % (len(rows), len(stamped)))
        print("Passed: 1  Failed: 0")
        sys.exit(0)
    print("test-plan-index: index up to date (%d plans)" % len(rows))
    print("Passed: 1  Failed: 0")
    sys.exit(0)

if mode == "fix":
    # Deterministic-date guard (test-plan-index-fix-shipped-date-placeholder):
    # CI's autosync runs this same script on FULL history, so every shipped plan
    # there resolves a real git-log first-commit date. If a local --fix would
    # write the "—" placeholder for any row, the resulting INDEX is NOT
    # byte-identical to what CI regenerates — committing it drifts the required
    # check on a date the author can't reproduce. REFUSE rather than emit the
    # placeholder, with the same remedy the shallow guard names.
    if placeholder_slugs:
        print("test-plan-index: REFUSING --fix — no git-log first-commit date resolves for:",
              file=sys.stderr)
        for s in placeholder_slugs:
            print("    - %s" % s, file=sys.stderr)
        print("  CI derives these from FULL history; emitting the '—' placeholder here would",
              file=sys.stderr)
        print("  drift the committed INDEX from CI's regeneration. Likely a shallow clone or an",
              file=sys.stderr)
        print("  uncommitted plan file.", file=sys.stderr)
        print("  Remedy: git fetch --unshallow  (or commit the plan), then re-run --fix.",
              file=sys.stderr)
        print("Passed: 0  Failed: 1")
        sys.exit(2)
    stamped = stamp_markers()
    with open(index_file, "w", encoding="utf-8", newline="\n") as f:
        f.write(rebuilt)
    if stamped:
        print("test-plan-index: rewrote index (%d plans); stamped plan-date into %d plan(s)"
              % (len(rows), len(stamped)))
    else:
        print("test-plan-index: rewrote index (%d plans)" % len(rows))
    print("Passed: 1  Failed: 0")
    sys.exit(0)

# check mode: report drift
print("test-plan-index: DRIFT — shipped-plan index out of sync (%d plans in archive)." % len(rows), file=sys.stderr)
print("  run: bash agents/scripts/core/test-plan-index.sh --fix", file=sys.stderr)
print("Passed: 0  Failed: 1")
sys.exit(1)
PY
