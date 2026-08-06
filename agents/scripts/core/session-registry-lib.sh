#!/usr/bin/env bash
# session-registry-lib.sh — shared liveness primitives for the concurrent-session
# registry (<tree>/.claude/.active-sessions/<session_id>).
#
# WHY THIS EXISTS — the ppid=1 defect.
# The registry writer used to record `ppid=$PPID`. On Windows git-bash a hook
# bash spawned directly by claude.exe has no resolvable Win32 parent, so MSYS
# reports $PPID=1. A stored pid of 1 is useless: `kill -0 1` ALWAYS fails on
# Win32, so the PID-liveness arm of every reader was dead and liveness collapsed
# onto the 30-min ts-freshness arm alone. Two consequences: (1) a sibling that
# closed minutes ago kept BLOCKING HEAD-moving ops for up to 30 min; (2) the
# registry accreted dead entries that nothing could prove dead, so nothing pruned.
#
# THE FIX — store an AUTHORITATIVE pid (the first claude.exe ancestor's Win32 pid
# on Windows, $PPID on POSIX) and check it correctly per-OS (tasklist on Windows,
# kill -0 on POSIX). Readers prefer the authoritative pid and fall back to ts
# freshness ONLY for legacy/non-authoritative entries (ppid=1) — so legacy entries
# behave exactly as before (no regression) while new entries get instant,
# accurate liveness + dead+stale pruning.
#
# All non-bash readers (merge-watcher.py ctypes OpenProcess) already handle a
# real Win32 pid correctly; this only repairs the
# bash side and centralises the logic so the writer, the guard and the banner
# agree byte-for-byte (DRY Quality Pillar).
#
# Sourced by session-tree-banner.sh (writer/cleanup) and guard-shared-tree.sh
# (PreToolUse aggressor guard). Run directly with --selftest to self-verify.
#
# No side effects at source time — only function definitions. Test/override seams
# (read-only, no command-from-env injection):
#   SMATCHET_REGISTRY_OS=windows|posix   force the OS branch (default: autodetect)
#   SMATCHET_REGISTRY_FRESH_SECS=<n>     ts freshness / stale window (default 1800)
#   SMATCHET_SESSION_IMAGE=<name>        Win32 image to match (default claude.exe)
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.

# --- OS branch ---------------------------------------------------------------
sr_is_windows() {
  case "${SMATCHET_REGISTRY_OS:-}" in
    windows) return 0 ;;
    posix)   return 1 ;;
  esac
  case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

# --- pid authority -----------------------------------------------------------
# A pid is AUTHORITATIVE (worth a real liveness check) only when it is a number
# greater than 4: 0=Idle, 4=System on Windows, and 1 is the MSYS legacy sentinel
# the old writer produced. Non-authoritative pids fall back to ts freshness.
sr_pid_authoritative() {
  case "${1:-}" in
    ''|*[!0-9]*) return 1 ;;
  esac
  [ "$1" -gt 4 ] 2>/dev/null
}

# --- pid liveness ------------------------------------------------------------
# Memoised per-process Win32 snapshot so a registry scan costs ONE tasklist call,
# not one per entry (the guard runs on every Bash/PowerShell PreToolUse).
_sr_snapshot_ready=""
_sr_live_snapshot=""

sr_pid_alive() { # $1 = pid ; 0 = alive
  sr_pid_authoritative "${1:-}" || return 1
  local pid="$1"
  if sr_is_windows; then
    # tasklist is the Win32 liveness probe. If it is somehow off PATH we cannot
    # prove the pid dead, so treat an authoritative pid as alive — conservative:
    # never false-prune a live session, never false-allow a HEAD-moving op. On a
    # real Windows host tasklist is always present. (Also satisfies the
    # SHELL_LINT_DEPS preflight contract for the allowlisted `tasklist`.)
    command -v tasklist >/dev/null 2>&1 || return 0
    if [ -z "$_sr_snapshot_ready" ]; then
      _sr_snapshot_ready=1
      _sr_live_snapshot="$(tasklist //FO CSV //NH //FI "IMAGENAME eq ${SMATCHET_SESSION_IMAGE:-claude.exe}" 2>/dev/null \
        | sed -n 's/^"[^"]*","\([0-9][0-9]*\)".*/\1/p')"
    fi
    local nl
    nl=$'\n'
    case "$nl$_sr_live_snapshot$nl" in
      *"$nl$pid$nl"*) return 0 ;;
      *) return 1 ;;
    esac
  else
    kill -0 "$pid" 2>/dev/null
  fi
}

# --- record this session's pid -----------------------------------------------
# POSIX: the hook bash's parent IS the claude process, so $PPID is correct.
# Windows: $PPID is the MSYS sentinel (1), so walk the Win32 parent chain from
# this bash up to the first claude.exe ancestor (the session-lifetime process).
# Falls back to $PPID (the legacy value) if the walk can't resolve — a
# non-authoritative pid degrades safely to ts-only liveness, never a wrong block.
sr_win_ancestor_pid() {
  local self_winpid
  self_winpid="$(cat "/proc/$$/winpid" 2>/dev/null)"
  case "$self_winpid" in ''|*[!0-9]*) return 1 ;; esac
  local img="${SMATCHET_SESSION_IMAGE:-claude.exe}"
  # NOTE: the PowerShell loop var is $cur, NOT $pid — $PID is a read-only
  # AUTOMATIC variable in PowerShell (the shell's own pid); assigning to it
  # aborts the walk, which is how an earlier cut silently fell back to ppid=1.
  powershell.exe -NoProfile -NonInteractive -Command "
    \$wanted = '$img'
    \$cur = $self_winpid
    for (\$i = 0; \$i -lt 24 -and \$cur -gt 4; \$i++) {
      \$p = Get-CimInstance Win32_Process -Filter \"ProcessId=\$cur\" -ErrorAction SilentlyContinue
      if (-not \$p) { break }
      if (\$p.Name -eq \$wanted) { Write-Output \$p.ProcessId; break }
      \$cur = \$p.ParentProcessId
    }" 2>/dev/null | tr -d '[:space:]'
}

sr_session_pid() {
  if sr_is_windows; then
    local pid
    pid="$(sr_win_ancestor_pid)"
    if sr_pid_authoritative "$pid"; then
      printf '%s' "$pid"
      return 0
    fi
  fi
  printf '%s' "$PPID"
}

# --- entry liveness (the one true rule) --------------------------------------
# Authoritative pid -> trust the pid (alive? live : dead, regardless of ts).
# Non-authoritative pid (legacy ppid=1) -> fall back to ts freshness.
# This is what fixes over-blocking: a real-pid sibling that has exited is NOT
# live even while its ts is still inside the freshness window.
sr_entry_is_live() { # $1 = entry file, $2 = now (epoch s) ; 0 = live
  local f="$1" now="$2" fts fpid
  fpid="$(sed -n 's/^ppid=//p' "$f" 2>/dev/null | head -n1)"
  if sr_pid_authoritative "$fpid"; then
    sr_pid_alive "$fpid"
    return $?
  fi
  fts="$(sed -n 's/^ts=//p' "$f" 2>/dev/null | head -n1)"
  case "$fts" in ''|*[!0-9]*) return 1 ;; esac
  [ $((now - fts)) -lt "${SMATCHET_REGISTRY_FRESH_SECS:-1800}" ]
}

sr_count_live_siblings() { # $1 = regdir, $2 = own sid, $3 = now ; prints count
  local dir="$1" own="$2" now="$3" live=0 f
  if [ ! -d "$dir" ]; then printf '0'; return 0; fi
  for f in "$dir"/*; do
    [ -f "$f" ] || continue
    [ -n "$own" ] && [ "$(basename "$f")" = "$own" ] && continue
    sr_entry_is_live "$f" "$now" && live=$((live + 1))
  done
  printf '%s' "$live"
}

# --- cleanup -----------------------------------------------------------------
# Conservative prune: remove an entry ONLY when it is BOTH provably dead (an
# authoritative pid that is not alive) AND ts-stale (older than the freshness
# window). Never prunes:
#   - the current session's own entry ($2),
#   - an idle-but-live session (pid alive => not dead),
#   - a legacy/non-authoritative entry (pid unprovable => not "dead"; those age
#     out via the writer's separate >7d sweep instead).
sr_prune_dead_stale() { # $1 = regdir, $2 = own sid, $3 = now
  local dir="$1" own="$2" now="$3" f base fts fpid stale dead
  [ -d "$dir" ] || return 0
  for f in "$dir"/*; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    [ -n "$own" ] && [ "$base" = "$own" ] && continue
    fpid="$(sed -n 's/^ppid=//p' "$f" 2>/dev/null | head -n1)"
    sr_pid_authoritative "$fpid" || continue          # can't prove dead -> keep
    dead=0; sr_pid_alive "$fpid" || dead=1
    [ "$dead" = 1 ] || continue                       # alive -> keep
    fts="$(sed -n 's/^ts=//p' "$f" 2>/dev/null | head -n1)"
    stale=0
    case "$fts" in
      ''|*[!0-9]*) : ;;
      *) [ $((now - fts)) -ge "${SMATCHET_REGISTRY_FRESH_SECS:-1800}" ] && stale=1 ;;
    esac
    [ "$stale" = 1 ] && rm -f "$f" 2>/dev/null || true
  done
}

# --- selftest ----------------------------------------------------------------
sr_selftest() {
  local n=0 fail=0 td
  _sr_ok()   { n=$((n + 1)); printf 'ok %s - %s\n' "$n" "$1"; }
  _sr_fail() { n=$((n + 1)); fail=$((fail + 1)); printf 'not ok %s - %s\n' "$n" "$1"; }
  _sr_expect() { if [ "$1" = "$2" ]; then _sr_ok "$3"; else _sr_fail "$3 (want '$2' got '$1')"; fi; }

  # Determinism: force the POSIX branch so kill -0 is the liveness check
  # everywhere (Linux CI and Windows git-bash agree).
  export SMATCHET_REGISTRY_OS=posix
  export SMATCHET_REGISTRY_FRESH_SECS=1800

  # pid authority
  sr_pid_authoritative 12345 && _sr_ok "authoritative: 12345" || _sr_fail "authoritative: 12345"
  # selftest: asserts-failure — known-bad pids (0/4/empty/non-numeric) must be rejected as non-authoritative.
  for bad in 1 0 4 "" abc 12x; do
    if sr_pid_authoritative "$bad"; then _sr_fail "non-authoritative rejects '$bad'"; else _sr_ok "non-authoritative rejects '$bad'"; fi
  done

  # pid liveness (POSIX path): self is alive, a huge pid is dead.
  sr_pid_alive "$$" && _sr_ok "alive: self ($$)" || _sr_fail "alive: self ($$)"
  if sr_pid_alive 4000000000; then _sr_fail "dead: 4000000000"; else _sr_ok "dead: 4000000000"; fi

  # session pid is a positive integer.
  local sp; sp="$(sr_session_pid)"
  case "$sp" in ''|*[!0-9]*) _sr_fail "session pid numeric ($sp)" ;; *) [ "$sp" -gt 0 ] && _sr_ok "session pid numeric ($sp)" || _sr_fail "session pid numeric ($sp)" ;; esac

  # entry liveness + counting + pruning on a temp registry.
  td="$(mktemp -d 2>/dev/null)" || { printf '1..%s\n# mktemp failed\n' "$n"; return 1; }
  local now; now="$(date -u +%s)"
  local own="self-sid"
  printf 'branch=develop\nsha=abc\nppid=%s\nts=%s\n' "$$" "$now"            > "$td/$own"
  printf 'branch=develop\nsha=abc\nppid=%s\nts=%s\n' "$$" "$now"            > "$td/live-realpid"
  printf 'branch=develop\nsha=abc\nppid=4000000000\nts=%s\n' "$now"         > "$td/dead-fresh"
  printf 'branch=develop\nsha=abc\nppid=4000000000\nts=%s\n' "$((now-9000))"> "$td/dead-stale"
  printf 'branch=develop\nsha=abc\nppid=1\nts=%s\n' "$now"                  > "$td/legacy-fresh"
  printf 'branch=develop\nsha=abc\nppid=1\nts=%s\n' "$((now-9000))"         > "$td/legacy-stale"

  sr_entry_is_live "$td/live-realpid" "$now" && _sr_ok "live: real alive pid" || _sr_fail "live: real alive pid"
  if sr_entry_is_live "$td/dead-fresh" "$now"; then _sr_fail "dead pid not live even when ts fresh (over-block fix)"; else _sr_ok "dead pid not live even when ts fresh (over-block fix)"; fi
  sr_entry_is_live "$td/legacy-fresh" "$now" && _sr_ok "legacy ppid=1 fresh -> live (ts fallback)" || _sr_fail "legacy ppid=1 fresh -> live (ts fallback)"
  if sr_entry_is_live "$td/legacy-stale" "$now"; then _sr_fail "legacy ppid=1 stale -> not live"; else _sr_ok "legacy ppid=1 stale -> not live"; fi

  # count excludes own sid; live-realpid + legacy-fresh are the 2 live siblings.
  local c; c="$(sr_count_live_siblings "$td" "$own" "$now")"
  _sr_expect "$c" "2" "count live siblings excludes own + dead"

  # prune removes ONLY dead-stale; keeps live, dead-fresh, legacy-*, own.
  sr_prune_dead_stale "$td" "$own" "$now"
  [ ! -f "$td/dead-stale" ]  && _sr_ok "prune removed dead+stale" || _sr_fail "prune removed dead+stale"
  [ -f "$td/dead-fresh" ]    && _sr_ok "prune kept dead+fresh"    || _sr_fail "prune kept dead+fresh"
  [ -f "$td/live-realpid" ]  && _sr_ok "prune kept live"          || _sr_fail "prune kept live"
  [ -f "$td/legacy-stale" ]  && _sr_ok "prune kept legacy (unprovable)" || _sr_fail "prune kept legacy (unprovable)"
  [ -f "$td/$own" ]          && _sr_ok "prune kept own entry"     || _sr_fail "prune kept own entry"

  rm -rf "$td" 2>/dev/null || true
  printf '1..%s\n' "$n"
  [ "$fail" -eq 0 ]
}

# Run selftest only when executed directly (never when sourced).
if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
  case "${1:-}" in
    --selftest) sr_selftest; exit $? ;;
    *) printf 'session-registry-lib.sh — source me, or run with --selftest\n'; exit 0 ;;
  esac
fi
