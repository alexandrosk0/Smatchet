#!/usr/bin/env bash
# lock-table-cache.sh — the ONE shared plan-lock substrate for Layers A/B/C.
#
# Sourced by the edit-time guard (Layer A, guard-plan-lock.sh), the pre-push
# stop (Layer B) and the CI gate decision script (Layer C). Keeping all three on
# this single bash primitive is what makes "a path one layer blocks, all layers
# block" true BY CONSTRUCTION (same code, same language) rather than by a fragile
# cross-language conformance test. `_lock-json.py` is the only Python any layer
# touches — it parses JSON (lock-rows / format-json), it does NO coverage match.
#
# Two caches, two TTLs, two substrates (temp files keyed on the repo's common git
# dir, shared across that repo's worktrees):
#   - lock table  (~30s): the `refs/locks/*` claim set (who claimed WHAT).
#   - sibling count (~15s): repo-global live-agent count (who is ALIVE). A
#     `refs/locks/*` change does NOT bust it (different substrate); TTL-only.
#
# FAIL-OPEN CONTRACT (this is advisory plumbing, never a wedge):
#   - ltc_sibling_count  : prints 0 on ANY error (git/scan failure) -> "solo"
#                          -> Layer A takes no action. Under-reporting a
#                          newly-live sibling for <=TTL is accepted (B/C are the
#                          net). On a sibling's DEATH it over-reports for <=TTL
#                          (a now-solo agent stays gated on un-seeded paths for a
#                          bounded window) — self-heals at the next TTL/seed.
#   - ltc_covering_slug  : exit 2 = UNDETERMINED (table unavailable) so the
#                          caller fails OPEN (allow); exit 1 = table available,
#                          no covering lock (caller may deny under contention);
#                          exit 0 = covered (slug on stdout).
#
# Identity is keyed on BRANCH, never owner/AGENT_ID (unprovisioned in prod —
# every real lock is owner=orchestrator, so an owner check is a no-op). See
# docs/plans/plan-lock-enforcement.md § Approach (identity note).
#
# Test/override seams (read-only):
#   LTC_PROJ           force the repo root (default CLAUDE_PROJECT_DIR | $PWD)
#   LTC_TABLE_TTL      lock-table cache TTL secs (default 30; 0 = never serve)
#   LTC_COUNT_TTL      sibling-count cache TTL secs (default 15; 0 = never serve)
#   LTC_ROWS_OVERRIDE  file of pre-flattened TSV lock rows -> bypass locks-show
#                      (bats inject synthetic lock tables without the network)
#
# No side effects at source time beyond sourcing session-registry-lib.sh (which
# only defines functions). Run directly with --selftest to self-verify.

# Resolve our own dir so the COPIED-into-.claude/hooks sibling still finds the
# real scripts when this lib is sourced from agents/scripts/core/ via $PROJ.
_LTC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)"
# Sibling liveness primitives (sr_entry_is_live). Absent -> sibling count fails
# open to 0 (handled in _ltc_compute_sibling_count).
[ -n "$_LTC_DIR" ] && [ -f "$_LTC_DIR/session-registry-lib.sh" ] && \
  . "$_LTC_DIR/session-registry-lib.sh"

_ltc_proj() { printf '%s' "${LTC_PROJ:-${CLAUDE_PROJECT_DIR:-$PWD}}"; }
_ltc_cache_dir() { printf '%s' "${TMPDIR:-/tmp}"; }

_ltc_pybin() {
  local c
  # Exec-validate, don't just resolve: on Windows `python3` resolves to the
  # Microsoft Store App Execution Alias stub (on PATH, exits non-zero when run),
  # so a `command -v`-only probe hands back a stub that cannot execute the cache
  # helper while a working `python` sits later in the list.
  for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
  done
  printf 'python3'
}

# Stable per-repo cache key: the common git dir (one .git shared by all the
# repo's worktrees) hashed via POSIX cksum. Empty -> caller skips the cache.
_ltc_repo_key() {
  local proj cd
  proj="$(_ltc_proj)"
  cd="$(git -C "$proj" rev-parse --git-common-dir 2>/dev/null)" || return 1
  [ -n "$cd" ] || return 1
  case "$cd" in
    /*|[A-Za-z]:*|\\\\*) ;;
    *) cd="$(git -C "$proj" rev-parse --show-toplevel 2>/dev/null)/$cd" ;;
  esac
  printf '%s' "$cd" | cksum 2>/dev/null | awk '{print $1}'
}

# Normalize a path to repo-relative forward-slash form. A no-op on the ubuntu CI
# runner (Layer C), active on Windows git-bash (A/B) — correct on both.
_ltc_norm_path() {
  local p="$1"
  # Strip CR first. `_lock-json.py` writes its TSV rows through a Windows
  # text-mode stdout, so every row arrives CRLF-terminated and the trailing CR
  # lands on the LAST field -- the path. An unstripped CR makes the exact-match
  # below fail for every locked path, i.e. a held lock silently covers nothing.
  p="${p//$'\r'/}"
  p="${p//\\//}"
  case "$p" in ./*) p="${p#./}" ;; esac
  printf '%s' "$p"
}

# --- lock table (~30s cache) -------------------------------------------------
# Prints the lock-table JSON on stdout. exit 0 = available (incl. genuine "[]"),
# non-zero = UNAVAILABLE (locks-show errored) -> caller fails open.
ltc_lock_table_json() {
  local key cache now cached cts
  now="$(date -u +%s 2>/dev/null)" || return 1
  key="$(_ltc_repo_key)" || key=""
  if [ -n "$key" ]; then
    cache="$(_ltc_cache_dir)/.smatchet-planlock-table-$key"
    if cached="$(cat "$cache" 2>/dev/null)" && [ -n "$cached" ]; then
      cts="$(printf '%s\n' "$cached" | head -n1)"
      case "$cts" in
        ''|*[!0-9]*) : ;;
        *) if [ $((now - cts)) -lt "${LTC_TABLE_TTL:-30}" ]; then
             printf '%s\n' "$cached" | tail -n +2
             return 0
           fi ;;
      esac
    fi
  fi
  local json rc
  json="$(LOCKS_SHOW_FORMAT=json bash "$_LTC_DIR/locks-show.sh" 2>/dev/null)"; rc=$?
  [ "$rc" -eq 0 ] || return 1
  if [ -n "$key" ]; then
    { printf '%s\n' "$now"; printf '%s' "$json"; } > "$cache.tmp.$$" 2>/dev/null \
      && mv -f "$cache.tmp.$$" "$cache" 2>/dev/null || rm -f "$cache.tmp.$$" 2>/dev/null
  fi
  printf '%s' "$json"
  return 0
}

# Flattened TSV rows (branch<TAB>epoch<TAB>slug<TAB>path). exit 2 if the table is
# UNAVAILABLE (fail-open signal), 0 otherwise (rows on stdout, possibly empty).
_ltc_lock_rows() {
  if [ -n "${LTC_ROWS_OVERRIDE:-}" ]; then
    [ -f "$LTC_ROWS_OVERRIDE" ] && cat "$LTC_ROWS_OVERRIDE" 2>/dev/null
    return 0
  fi
  local json
  json="$(ltc_lock_table_json)" || return 2
  printf '%s' "$json" | "$(_ltc_pybin)" "$_LTC_DIR/_lock-json.py" lock-rows 2>/dev/null || return 2
  return 0
}

# Coverage primitive — the shared exact-path-or-dir-prefix match.
#   $1 repo-relative path          $2 mode: same|other
#   $3 my branch                   $4 (optional) stale-cutoff epoch
# mode=same  : a lock whose branch == mine (Layer A: my own seed -> allow).
# mode=other : a lock whose branch is non-empty and != mine (Layer B/C: block).
# An empty/detached lock branch is UNATTRIBUTABLE -> never matches "other"
# (bias-to-allow; the stale sweep ages it out). A stale-cutoff (B/C) skips locks
# older than it; Layer A passes none so its own seed always covers (no
# self-denial of long-running work — the F3 nuance).
# exit 0 covered (slug on stdout) | 1 not covered | 2 undetermined (fail-open).
ltc_covering_slug() {
  local path="$1" mode="$2" mybr="$3" cutoff="${4:-}" rows
  rows="$(_ltc_lock_rows)" || return 2
  path="$(_ltc_norm_path "$path")"
  [ -n "$path" ] || return 1
  local br ep slug lp
  while IFS="$(printf '\t')" read -r br ep slug lp; do
    [ -n "$lp" ] || continue
    case "$mode" in
      same)  [ "$br" = "$mybr" ] || continue ;;
      other) { [ -n "$br" ] && [ "$br" != "$mybr" ]; } || continue ;;
      *) continue ;;
    esac
    if [ -n "$cutoff" ] && [ -n "$ep" ]; then
      case "$ep" in ''|*[!0-9]*) : ;; *) [ "$ep" -ge "$cutoff" ] || continue ;; esac
    fi
    lp="$(_ltc_norm_path "$lp")"
    if [ "$path" = "$lp" ]; then printf '%s' "$slug"; return 0; fi
    case "$lp" in
      */) case "$path/" in "$lp"*) printf '%s' "$slug"; return 0 ;; esac ;;
    esac
  done <<LTC_ROWS
$rows
LTC_ROWS
  return 1
}

# --- repo-global sibling count (~15s cache) ----------------------------------
# Distinct live session_ids across EVERY worktree's registry, minus own. NOT a
# sum of sr_count_live_siblings (which can't dedup a sid present in two regdirs
# -> false-contention deny); a union-of-distinct over sr_entry_is_live costs the
# same liveness checks and is dedupable. Scanning every regdir is required
# because the registry writer's landing site is pwd-non-deterministic
# (session-tree-banner.sh: CLAUDE_PROJECT_DIR unset -> $(pwd)).
_ltc_compute_sibling_count() {
  local own="$1" proj="$2" now="$3" sids n=0
  command -v git >/dev/null 2>&1 || { printf '0'; return 0; }
  command -v sr_entry_is_live >/dev/null 2>&1 || { printf '0'; return 0; }
  sids="$(
    git -C "$proj" worktree list --porcelain 2>/dev/null | while IFS= read -r line; do
      case "$line" in
        "worktree "*)
          regdir="${line#worktree }/.claude/.active-sessions"
          [ -d "$regdir" ] || continue
          for f in "$regdir"/*; do
            [ -f "$f" ] || continue
            sr_entry_is_live "$f" "$now" && printf '%s\n' "${f##*/}"
          done
          ;;
      esac
    done | sort -u
  )"
  [ -n "$own" ] && sids="$(printf '%s\n' "$sids" | grep -vxF -- "$own" 2>/dev/null || true)"
  [ -n "$sids" ] && n="$(printf '%s\n' "$sids" | grep -c . 2>/dev/null || printf '0')"
  printf '%s' "$n"
}

ltc_sibling_count() { # $1 = own session_id ; prints distinct live sibling count
  local own="${1:-}" proj now key cache cached cts cval count
  proj="$(_ltc_proj)"
  now="$(date -u +%s 2>/dev/null)" || { printf '0'; return 0; }
  key="$(_ltc_repo_key)" || key=""
  if [ -n "$key" ]; then
    cache="$(_ltc_cache_dir)/.smatchet-planlock-count-$key"
    if cached="$(cat "$cache" 2>/dev/null)"; then
      cts="${cached%% *}"; cval="${cached#* }"
      case "$cts" in
        ''|*[!0-9]*) : ;;
        *) case "$cval" in
             ''|*[!0-9]*) : ;;
             *) [ $((now - cts)) -lt "${LTC_COUNT_TTL:-15}" ] && { printf '%s' "$cval"; return 0; } ;;
           esac ;;
      esac
    fi
  fi
  count="$(_ltc_compute_sibling_count "$own" "$proj" "$now")"
  case "$count" in ''|*[!0-9]*) count=0 ;; esac
  if [ -n "$key" ]; then
    printf '%s %s' "$now" "$count" > "$cache.tmp.$$" 2>/dev/null \
      && mv -f "$cache.tmp.$$" "$cache" 2>/dev/null || rm -f "$cache.tmp.$$" 2>/dev/null
  fi
  printf '%s' "$count"
}

# --- selftest ----------------------------------------------------------------
_ltc_selftest() {
  local n=0 fail=0 td out rc
  _t_ok()   { n=$((n + 1)); printf 'ok %s - %s\n' "$n" "$1"; }
  _t_fail() { n=$((n + 1)); fail=$((fail + 1)); printf 'not ok %s - %s\n' "$n" "$1"; }
  _t_eq()   { if [ "$1" = "$2" ]; then _t_ok "$3"; else _t_fail "$3 (want '$2' got '$1')"; fi; }

  # path normalization
  _t_eq "$(_ltc_norm_path 'a\b\c.cpp')" "a/b/c.cpp" "norm backslash->slash"
  _t_eq "$(_ltc_norm_path './x/y')" "x/y" "norm strips leading ./"

  td="$(mktemp -d 2>/dev/null)" || { printf '1..%s\n# mktemp failed\n' "$n"; return 1; }
  # Synthetic lock rows: branch<TAB>epoch<TAB>slug<TAB>path
  printf 'develop\t1781049600\tmine\tSource/Core/src/Sync/A.cpp\n'  > "$td/rows"
  printf 'develop\t1781049600\tmine\tSource/Core/src/Persistence/\n' >> "$td/rows"
  printf 'feat/x\t1781049600\tother\tSource/Core/src/Tracker/B.cpp\n' >> "$td/rows"
  printf '\t1781049600\tnobr\tSource/Core/src/Unattributed.cpp\n'   >> "$td/rows"
  export LTC_ROWS_OVERRIDE="$td/rows"

  # same-branch coverage (Layer A allow)
  out="$(ltc_covering_slug 'Source/Core/src/Sync/A.cpp' same develop)"; rc=$?
  { [ "$rc" -eq 0 ] && [ "$out" = "mine" ]; } && _t_ok "same: exact path covered by my branch" || _t_fail "same: exact (rc=$rc out=$out)"
  out="$(ltc_covering_slug 'Source/Core/src/Persistence/Db.cpp' same develop)"; rc=$?
  { [ "$rc" -eq 0 ] && [ "$out" = "mine" ]; } && _t_ok "same: dir-prefix covered" || _t_fail "same: dir-prefix (rc=$rc out=$out)"
  # selftest: asserts-failure — a sibling path under no write_set entry is NOT covered.
  ltc_covering_slug 'Source/Core/src/Sync/NOT_LISTED.cpp' same develop >/dev/null; rc=$?
  [ "$rc" -eq 1 ] && _t_ok "same: uncovered sibling path -> rc 1 (deny path)" || _t_fail "same: uncovered want rc1 got $rc"
  # owner-collision canary: the other-branch lock must NOT count as my cover.
  ltc_covering_slug 'Source/Core/src/Tracker/B.cpp' same develop >/dev/null; rc=$?
  [ "$rc" -eq 1 ] && _t_ok "branch-key: other-branch lock is not my cover (owner-collision canary)" || _t_fail "branch-key canary got rc=$rc"
  # other-branch overlap (Layer B/C block basis)
  out="$(ltc_covering_slug 'Source/Core/src/Tracker/B.cpp' other develop)"; rc=$?
  { [ "$rc" -eq 0 ] && [ "$out" = "other" ]; } && _t_ok "other: cross-branch overlap detected" || _t_fail "other: overlap (rc=$rc out=$out)"
  # unattributable (empty branch) never blocks as "other"
  ltc_covering_slug 'Source/Core/src/Unattributed.cpp' other develop >/dev/null; rc=$?
  [ "$rc" -eq 1 ] && _t_ok "other: empty-branch lock is unattributable (non-blocking)" || _t_fail "other: empty-branch got rc=$rc"
  # staleness cutoff (F3): a cutoff newer than the lock's epoch hides it from "other"
  ltc_covering_slug 'Source/Core/src/Tracker/B.cpp' other develop 9999999999 >/dev/null; rc=$?
  [ "$rc" -eq 1 ] && _t_ok "stale: lock older than cutoff is non-blocking (F3)" || _t_fail "stale: cutoff got rc=$rc"

  # undetermined -> fail-open signal (rc 2) when the table is unavailable.
  unset LTC_ROWS_OVERRIDE
  export LTC_ROWS_OVERRIDE="$td/does-not-exist"
  # An override file that does not exist yields zero rows (rc 0, not covered),
  # NOT the unavailable signal — the unavailable signal is a locks-show error,
  # exercised in the bats integration suite. Assert the empty-rows -> rc 1 here.
  ltc_covering_slug 'Source/Core/src/Sync/A.cpp' same develop >/dev/null; rc=$?
  [ "$rc" -eq 1 ] && _t_ok "empty rows -> not covered (rc 1)" || _t_fail "empty rows got rc=$rc"
  unset LTC_ROWS_OVERRIDE

  rm -rf "$td" 2>/dev/null || true
  printf '1..%s\n' "$n"
  [ "$fail" -eq 0 ]
}

if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
  case "${1:-}" in
    --selftest) _ltc_selftest; exit $? ;;
    *) printf 'lock-table-cache.sh — source me, or run with --selftest\n'; exit 0 ;;
  esac
fi
