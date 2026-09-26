#!/usr/bin/env bash
# 74-offline-heuristic.sh — Quality Pillar 6 (offline-first) HEURISTIC rules (sourced by
# test-lint-rules.sh, not run directly). WARN-FIRST / ADVISORY: never touches $rc; diff-scoped to changed
# first-party .cpp files. Each graduates to blocking on its own per ADR-0026.
#
# offline-loading-only-render — a "Loading" line drawn in a TU that fetches, with no freshness cue within
# the window. Offline it stays up for the whole retry window (up to ~90 s) even when cached data exists.
# offline-inflight-latch-unguarded — an in-flight flag set true shortly before a launch/kick with no
# ScopeExit / RunKeyedFetch / TryBeginFetch / LaunchIntoSlot / catch nearby: a throw or a dropped task
# leaves the UI on "Loading" forever.
# offline-failure-cached-as-loaded — `loaded = true` next to a failure log with no retry/backoff token: an
# offline failure is remembered as final and never retried after reconnect.
# offline-cache-cleared — clearing catalog / user / component state; offline it cannot be fetched back
# (only backend-switch and pane-retirement resets are legitimate — mark those with a deviation).
# offline-network-read-ungated (file-level) — a backend Fetch/Search/List call outside Tracker/ and Sync/
# in a file that never consults connectivity.
#
# Escape: // SMATCHET_DEVIATION(rule=<id>; ...) within the 3 lines above the hit (anywhere in the file for
# offline-network-read-ungated).

OFFLINE_LOADING_RE='(TextDisabled|TextUnformatted|TextWrapped|Text|SetTooltip)[[:space:]]*\(.*("Loading|T\("[a-z0-9_.]*loading)'
OFFLINE_FETCHER_RE='LaunchBackgroundTask\(|std::async\(|Ensure[A-Z][A-Za-z]*Loaded\(|Kick[A-Z][A-Za-z]*\('
OFFLINE_CUE_RE='DataFreshnessCue|ClassifyFreshness|ShouldRenderContent'
OFFLINE_LATCH_RE='[A-Za-z_]*[Ii]n[Ff]light[A-Za-z_]*[[:space:]]*=[[:space:]]*true'
OFFLINE_LAUNCH_RE='std::async\(|Kick[A-Z][A-Za-z]*\(|Launch[A-Z][A-Za-z]*\('
OFFLINE_LATCH_GUARD_RE='ScopeExit|RunKeyedFetch|TryBeginFetch|LaunchIntoSlot|catch[[:space:]]*\('
OFFLINE_LOADED_TRUE_RE='[Ll]oaded[A-Za-z_]*[[:space:]]*=[[:space:]]*true'
OFFLINE_FAIL_LOG_RE='LOG_(WARN|ERROR)\(.*([Ff]ail|[Ee]rror)'
OFFLINE_BACKOFF_RE='[Rr]etry|CompleteFailure|[Bb]ackoff'
OFFLINE_CLEAR_RE='(AvailableFields|AvailableComponents|AvailableIssueTypeMeta|AvailableUsers|projectComponentOptions_)\.clear\(\)|SetAvailableUsers\([[:space:]]*\{\}[[:space:]]*\)'
OFFLINE_NET_READ_RE='(FieldCatalog\(\)|Collaboration\(\)|Reader\(\)|Connectivity\(\))[[:space:]]*(->|\.)[[:space:]]*(Fetch|Search|List)[A-Za-z]*\('
OFFLINE_NET_GATE_RE='ShouldAttemptNetwork|IsOfflineState|IsTrackerOffline|TryBeginFetch|RunKeyedFetch|RouteWrite|TrackerConnectivity\(\)'
OFFLINE_HEUR_WINDOW="${SMATCHET_OFFLINE_WINDOW:-25}"

_offline_dev_above() {
    # $1 = line index, $2 = rule id. True when a deviation for the rule sits within the 3 lines above.
    local i="$1" rule="$2" j
    for ((j = i - 1; j >= 0 && j >= i - 3; j--)); do
        case "${_OFF_LINES[$j]}" in *"SMATCHET_DEVIATION(rule=$rule"*) return 0 ;; esac
    done
    return 1
}

_offline_window_has() {
    # $1..$2 = inclusive index range (clamped), $3 = ERE. True when any line in range matches.
    local from="$1" to="$2" re="$3" j n=${#_OFF_LINES[@]}
    [ "$from" -lt 0 ] && from=0
    [ "$to" -ge "$n" ] && to=$((n - 1))
    for ((j = from; j <= to; j++)); do
        [[ "${_OFF_LINES[$j]}" =~ $re ]] && return 0
    done
    return 1
}

scan_offline_heuristic_file() {
    # $1 = file to read; $2 = logical repo path (defaults to $1).
    local f="$1" logical="${2:-$1}"
    [ -f "$f" ] || return 0
    case "$logical" in Source/*.cpp) ;; *) return 0 ;; esac
    case "$logical" in */ThirdParty/*) return 0 ;; esac
    _OFF_LINES=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do _OFF_LINES+=("$line"); done < "$f"
    local n=${#_OFF_LINES[@]} i raw s code w="$OFFLINE_HEUR_WINDOW" file_text
    file_text="$(cat "$f")"
    local file_fetches=0 file_gated=0 net_reported=0 in_domain=0
    [[ "$file_text" =~ $OFFLINE_FETCHER_RE ]] && file_fetches=1
    [[ "$file_text" =~ $OFFLINE_NET_GATE_RE ]] && file_gated=1
    case "$logical" in Source/Core/src/Tracker/*|Source/Core/src/Sync/*) in_domain=1 ;; esac
    for ((i = 0; i < n; i++)); do
        raw="${_OFF_LINES[$i]}"
        s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac
        code="${raw%%//*}"
        if [ "$file_fetches" -eq 1 ] && [[ "$code" =~ $OFFLINE_LOADING_RE ]] \
            && ! _offline_window_has $((i - w)) $((i + w)) "$OFFLINE_CUE_RE" \
            && ! _offline_dev_above "$i" offline-loading-only-render; then
            printf 'offline-loading-only-render\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_LATCH_RE ]] \
            && _offline_window_has "$i" $((i + w)) "$OFFLINE_LAUNCH_RE" \
            && ! _offline_window_has $((i - 5)) $((i + 60)) "$OFFLINE_LATCH_GUARD_RE" \
            && ! _offline_dev_above "$i" offline-inflight-latch-unguarded; then
            printf 'offline-inflight-latch-unguarded\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_LOADED_TRUE_RE ]] \
            && _offline_window_has $((i - 6)) $((i + 6)) "$OFFLINE_FAIL_LOG_RE" \
            && ! _offline_window_has $((i - 10)) $((i + 10)) "$OFFLINE_BACKOFF_RE" \
            && ! _offline_dev_above "$i" offline-failure-cached-as-loaded; then
            printf 'offline-failure-cached-as-loaded\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_CLEAR_RE ]] && ! _offline_dev_above "$i" offline-cache-cleared; then
            printf 'offline-cache-cleared\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [ "$net_reported" -eq 0 ] && [ "$in_domain" -eq 0 ] && [ "$file_gated" -eq 0 ] \
            && [[ "$code" =~ $OFFLINE_NET_READ_RE ]] \
            && [[ "$file_text" != *"SMATCHET_DEVIATION(rule=offline-network-read-ungated"* ]]; then
            printf 'offline-network-read-ungated\t%s:%s\n' "$logical" "$((i + 1))"
            net_reported=1
        fi
    done
}

compute_offline_heuristic_violations() {
    local f
    while IFS= read -r f; do [ -n "$f" ] && scan_offline_heuristic_file "$f"; done < <(list_first_party_cpp_files | grep -E '\.cpp$' || true)
}
