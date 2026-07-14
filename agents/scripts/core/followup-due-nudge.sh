#!/usr/bin/env bash
# followup-due-nudge.sh — SessionStart nudge when a deferred, condition-gated
# follow-up becomes DUE. Reads optional `Triggered-follow-up:` lines on backlog
# entries in both self-improvement sources — the legacy monolith
# docs/self-improvement/categories/*.md AND the new per-entry files
# docs/self-improvement/categories/*/*.md — evaluates each `when=` trigger, and
# emits a nudge listing every firing entry — exactly like the
# memory-drain / postmortem-owed SessionStart nudges.
#
# Why: some backlog items are follow-ups GATED ON A FUTURE CONDITION ("re-measure
# after ~10 PRs", "after ~20 PRs flip the gate"). As prose in a `Concrete next
# action:` line nothing machine-checks the condition, so the firing relies on
# manual triage and silently never happens once the originating plan archives.
# Plan: docs/plans/triggered-followup-tracking.md.
#
# Entry-line grammar (one optional line per backlog entry, fields in this order):
#   Triggered-follow-up: when=<kind>:<spec>; action=<one-line>; baseline=<prose>; fired=<never|YYYY-MM-DD>
# Four `when=` kinds (`;`-delimited key=val within the spec; no space after the `;`):
#   pr-count:base=<branch>;since=<YYYY-MM-DD>;n=<N>  — N squash-merged PRs to base since a date
#   date:<YYYY-MM-DD>                                 — calendar deadline reached
#   plan-shipped:<slug>                              — docs/plans/shipped/<slug>.md exists
#   file-age:<path>;days=<N>                          — <path> last touched (git) >= N days ago
# An entry whose `fired=` is a real date is suppressed (already acted on).
# Entries with no `Triggered-follow-up:` line are invisible (fully backward-compat).
#
# Modes: --nudge (default; SessionStart block, silent when nothing due) / --list
#        (plain, always prints a header) / --selftest (grammar + FIRE/SKIP asserts).
#
# READ-ONLY: never stamps `fired=` itself (preserves the "nudges emit to stdout,
# never write tracked files" invariant + dodges concurrent-PR shared-file
# conflicts). The orchestrator stamps `fired=<date>` via the normal PR flow when
# it acts → a due-but-unaddressed follow-up re-nudges every session until then
# (same "keeps nagging until resolved" semantics as postmortem-owed).
#
# Advisory — exit 0 always. gh-unavailable / malformed / ambiguous → SKIP (never
# a false fire, never a hard fail).
#
# Env overrides: FOLLOWUP_FETCH_N (pr-count gh --limit, default 200),
#   FOLLOWUP_TODAY (today's date, for deterministic selftest), FOLLOWUP_CAT_DIR.

set -euo pipefail
cd "$(dirname "$0")/../../.." || exit 0

MODE="nudge"
case "${1:-}" in
    --list) MODE="list" ;;
    --nudge|"") MODE="nudge" ;;
    --selftest) MODE="selftest" ;;
    *) echo "usage: followup-due-nudge.sh [--list|--nudge|--selftest]" >&2; exit 2 ;;
esac

CAT_DIR="${FOLLOWUP_CAT_DIR:-docs/self-improvement/categories}"
FETCH_N="${FOLLOWUP_FETCH_N:-200}"
TODAY="${FOLLOWUP_TODAY:-$(date +%Y-%m-%d 2>/dev/null || echo 1970-01-01)}"

# --- key=val lookup within a `;`-delimited spec ----------------------------
_kv() {  # _kv "<spec>" "<key>" → value (empty if absent)
    local spec="$1" key="$2" tok
    local IFS=';'
    for tok in $spec; do
        case "$tok" in "$key="*) printf '%s' "${tok#*=}"; return ;; esac
    done
}

# --- four trigger evaluators: echo FIRE | SKIP | UNAVAILABLE | MALFORMED ----
_eval_pr_count() {  # spec: base=<b>;since=<date>;n=<N>
    local spec="$1" base since n count=""
    base="$(_kv "$spec" base)"; since="$(_kv "$spec" since)"; n="$(_kv "$spec" n)"
    { [ -n "$base" ] && [ -n "$since" ] && [ -n "$n" ]; } || { echo MALFORMED; return; }
    case "$n" in ''|*[!0-9]*) echo MALFORMED; return ;; esac
    # gh-primary: explicit --limit "$FETCH_N" is load-bearing — without it
    # `gh pr list ... --jq length` silently caps at the default 30 and the
    # count UNDER-fires once >30 PRs land in the window (the exact "silent
    # never-fires" failure this whole tool exists to kill; same completeness
    # guard postmortem-owed.sh adopted in #868).
    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        count="$(gh pr list --base "$base" --state merged --search "merged:>=$since" \
                    --limit "$FETCH_N" --json number --jq 'length' 2>/dev/null || true)"
    fi
    # offline fallback: count squash-landed `(#N)` PR subjects on origin/base
    # since the date. Merge commits ("Merge pull request #N") + direct-pushed
    # non-PR commits lack the `(#N)` parens and are correctly excluded.
    if ! { case "$count" in ''|*[!0-9]*) false ;; *) true ;; esac; }; then
        count="$(git log "origin/$base" --since="$since" --format='%s' 2>/dev/null \
                    | grep -cE '\(#[0-9]+\)' || true)"
    fi
    case "$count" in ''|*[!0-9]*) echo UNAVAILABLE; return ;; esac
    [ "$count" -ge "$n" ] && echo FIRE || echo SKIP
}

_eval_date() {  # spec: YYYY-MM-DD
    local spec="$1"
    case "$spec" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
        *) echo MALFORMED; return ;;
    esac
    # FIRE when the deadline is today or earlier (ISO dates compare lexically).
    if [ "$spec" \> "$TODAY" ]; then echo SKIP; else echo FIRE; fi
}

_eval_plan_shipped() {  # spec: <slug>
    local slug="$1"
    [ -n "$slug" ] || { echo MALFORMED; return; }
    case "$slug" in *[!a-z0-9-]*) echo MALFORMED; return ;; esac
    [ -f "docs/plans/shipped/${slug}.md" ] && echo FIRE || echo SKIP
}

_eval_file_age() {  # spec: <path>;days=<N>
    local spec="$1" path days last now age
    path="${spec%%;*}"; days="$(_kv "$spec" days)"
    { [ -n "$path" ] && [ -n "$days" ]; } || { echo MALFORMED; return; }
    case "$days" in ''|*[!0-9]*) echo MALFORMED; return ;; esac
    # git-commit age (robust to checkout mtime resets), not filesystem mtime.
    last="$(git log -1 --format='%ct' -- "$path" 2>/dev/null || true)"
    case "$last" in ''|*[!0-9]*) echo UNAVAILABLE; return ;; esac
    now="$(date +%s 2>/dev/null || echo 0)"
    age=$(( (now - last) / 86400 ))
    [ "$age" -ge "$days" ] && echo FIRE || echo SKIP
}

eval_trigger() {  # eval_trigger "<when-value>" → FIRE|SKIP|UNAVAILABLE|MALFORMED
    local when="$1" kind spec
    kind="${when%%:*}"; spec="${when#*:}"
    case "$kind" in
        pr-count)     _eval_pr_count "$spec" ;;
        date)         _eval_date "$spec" ;;
        plan-shipped) _eval_plan_shipped "$spec" ;;
        file-age)     _eval_file_age "$spec" ;;
        *)            echo MALFORMED ;;
    esac
}

# --- field extraction (canonical order: when; action; baseline; fired) ------
_field() {  # _field "<line>" "<key>" "<next-key-or-empty>" → value
    local line="$1" key="$2" nxt="$3" v
    case "$line" in *"${key}="*) ;; *) return ;; esac
    v="${line#*"${key}"=}"
    [ -n "$nxt" ] && v="${v%%; "${nxt}"=*}"
    v="${v%$'\r'}"
    printf '%s' "$v"
}

# --- scan the category files, collect DUE entries ---------------------------
due=()    # "P<n>\t<file>\t<action>\t<baseline>"
warns=()  # "<file>: MALFORMED Triggered-follow-up: <when>"

scan() {
    local f pri line when action baseline fired verdict
    # Two sources, read in union: the legacy monolith categories/<cat>.md (flat,
    # *.md) PLUS the new per-entry files categories/<cat>/<date>-<slug>.md (one
    # entry each, in a per-category subdir). The */*.md glob picks up the new
    # layout so a Triggered-follow-up: on a new per-entry file is not silently
    # invisible. Unmatched globs stay literal and are skipped by the [ -f ] guard
    # (so a flat $CAT_DIR with no subdirs — the bats fixture — still works).
    for f in "$CAT_DIR"/*.md "$CAT_DIR"/*/*.md; do
        [ -f "$f" ] || continue
        pri="P3"
        while IFS= read -r line || [ -n "$line" ]; do
            line="${line%$'\r'}"
            # Track the current entry's priority (header: "- DATE · … · P<n> — title").
            if printf '%s' "$line" | grep -qE '^- .* P[0-3] '; then
                pri="$(printf '%s' "$line" | grep -oE 'P[0-3]' | head -1)"
                continue
            fi
            case "$line" in *"Triggered-follow-up:"*) ;; *) continue ;; esac
            when="$(_field "$line" when action)"
            action="$(_field "$line" action baseline)"
            baseline="$(_field "$line" baseline fired)"
            fired="$(_field "$line" fired '')"
            # Suppress entries already acted on (fired= is a real date).
            case "$fired" in
                [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) continue ;;
            esac
            [ -n "$when" ] || { warns+=("$f: empty when= in Triggered-follow-up"); continue; }
            verdict="$(eval_trigger "$when")"
            case "$verdict" in
                FIRE)      due+=("$pri"$'\t'"$f"$'\t'"$action"$'\t'"$baseline") ;;
                MALFORMED) warns+=("$f: MALFORMED when=$when") ;;
                *)         : ;;  # SKIP / UNAVAILABLE → silent
            esac
        done < "$f"
    done
}

# --- emit (priority-sorted P0→P3) -------------------------------------------
emit_block() {
    local n="${#due[@]}"
    printf '🔔 === follow-up due (%s) ===\n' "$n"
    printf '%s\n' "${due[@]}" | sort -t$'\t' -k1,1 | while IFS=$'\t' read -r pri file action baseline; do
        printf '  [%s] %s\n' "$pri" "$action"
        [ -n "$baseline" ] && printf '        baseline: %s\n' "$baseline"
        printf '        source: %s — stamp `fired=%s` via PR once acted.\n' "$file" "$TODAY"
    done
    for w in "${warns[@]}"; do printf '  ⚠ %s\n' "$w" >&2; done
}

case "$MODE" in
    selftest) ;;  # handled below
    *)
        scan
        if [ "${#due[@]}" -eq 0 ]; then
            [ "$MODE" = "list" ] && echo "followup-due-nudge: no follow-ups due."
            for w in "${warns[@]:-}"; do [ -n "$w" ] && printf '  ⚠ %s\n' "$w" >&2; done
            exit 0
        fi
        emit_block
        exit 0
        ;;
esac

# --- selftest ---------------------------------------------------------------
ST_PASS=0; ST_FAIL=0
st() {  # st "<desc>" "<expected>" "<actual>"
    if [ "$2" = "$3" ]; then ST_PASS=$((ST_PASS+1)); printf '  ok   %s\n' "$1"
    else ST_FAIL=$((ST_FAIL+1)); printf '  FAIL %s (want %s got %s)\n' "$1" "$2" "$3"; fi
}
FOLLOWUP_TODAY="2026-06-05"; TODAY="2026-06-05"
st "date past → FIRE"        FIRE      "$(_eval_date 2026-01-01)"
st "date today → FIRE"       FIRE      "$(_eval_date 2026-06-05)"
st "date future → SKIP"      SKIP      "$(_eval_date 2027-01-01)"
# selftest: asserts-failure — malformed/bad-input triggers must classify as MALFORMED (detection path).
st "date malformed → MAL"    MALFORMED "$(_eval_date 2026-6-5)"
st "plan-shipped real → FIRE" FIRE     "$(_eval_plan_shipped gate-escape-postmortem)"
st "plan-shipped absent → SKIP" SKIP   "$(_eval_plan_shipped no-such-plan-xyz)"
st "plan-shipped bad slug → MAL" MALFORMED "$(_eval_plan_shipped 'Bad Slug')"
st "pr-count missing key → MAL" MALFORMED "$(_eval_pr_count 'base=develop;n=10')"
st "pr-count bad n → MAL"    MALFORMED "$(_eval_pr_count 'base=develop;since=2026-01-01;n=ten')"
st "file-age missing days → MAL" MALFORMED "$(_eval_file_age 'AGENTS.md')"
st "unknown kind → MAL"      MALFORMED "$(eval_trigger 'bogus:whatever')"
st "_kv extracts"            develop   "$(_kv 'base=develop;since=x;n=10' base)"
st "_kv n"                   10        "$(_kv 'base=develop;since=x;n=10' n)"
# field extraction on a canonical line
_L='Triggered-follow-up: when=pr-count:base=develop;since=2026-06-05;n=10; action=do the thing; baseline=3.2 heads/PR; fired=never'
st "field when"  'pr-count:base=develop;since=2026-06-05;n=10' "$(_field "$_L" when action)"
st "field action" 'do the thing' "$(_field "$_L" action baseline)"
st "field baseline" '3.2 heads/PR' "$(_field "$_L" baseline fired)"
st "field fired"  'never'          "$(_field "$_L" fired '')"
echo "followup-due-nudge selftest — Passed: $ST_PASS  Failed: $ST_FAIL"
[ "$ST_FAIL" -eq 0 ] || exit 1
exit 0
