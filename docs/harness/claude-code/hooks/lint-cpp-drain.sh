#!/usr/bin/env bash
# Stop hook: drain phase of the lint-cpp pipeline.
# - Globs .claude/.lint-queue.* (per-PID inline-phase queues), dedups paths,
#   filters to existing first-party files.
# - Processes up to SMATCHET_LINT_DRAIN_CHUNK files per invocation (default 10);
#   any remainder is rewritten to a single queue file for the next Stop event.
# - Runs cppcheck + clang-tidy + (for .cpp outside tests/) dual-target syntax
#   on each chunked file. Surfaces consolidated findings via exit 2.
#
# Serialised by scripts/dev/lockfile.py on .claude/.lint-queue.lock so
# concurrent Stop events (e.g. orchestrator + subagent both ending close
# together) cannot trample each other.

set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"

LOCK_FILE="$PROJ_DIR/.claude/.lint-queue.lock"

# --- Serialise concurrent Stop events --------------------------------------
# Re-exec this script under a non-blocking exclusive lock so the lock is held
# for the whole drain, then released by the OS when the process exits. If
# another drain already holds it, --busy-rc 0 makes us exit silently — that
# drain will process the queue (or a later Stop event will).
#
# lockfile.py (not flock(1)): flock is util-linux, absent on Git Bash and
# stock Windows, so the lock would silently no-op on the exact host that runs
# concurrent orchestrator + subagent Stop events. See docs/harness/SETUP.md.
if [[ -z "${SMATCHET_LINT_DRAIN_LOCKED:-}" ]]; then
    LOCK_PY="$PROJ_DIR/scripts/dev/lockfile.py"
    for py in python python3; do
        # Exec-probe, not just command -v: on Windows `python3` is the Microsoft
        # Store alias stub — on PATH, but exits non-zero when run.
        if command -v "$py" >/dev/null 2>&1 && "$py" -c "" >/dev/null 2>&1 && [[ -f "$LOCK_PY" ]]; then
            export SMATCHET_LINT_DRAIN_LOCKED=1
            exec "$py" "$LOCK_PY" --lockfile "$LOCK_FILE" --nonblock --busy-rc 0 \
                -- bash "$0" "$@"
        fi
    done
    # No Python: run lock-free rather than skipping the drain entirely.
fi

# --- Library + filters -----------------------------------------------------
# shellcheck source=lint-cpp-common.sh
. "$PROJ_DIR/.claude/hooks/lint-cpp-common.sh"

# --- Collect queue entries -------------------------------------------------
shopt -s nullglob
QUEUE_FILES=()
for q in "$PROJ_DIR"/.claude/.lint-queue.*; do
    # .lint-queue.lock is the serialisation lock, not a per-PID queue file.
    # It must never be read as queue content nor rm'd out from under the
    # holder (on Windows the delete fails anyway: the lock keeps it open).
    [[ "$q" == *.lock ]] && continue
    QUEUE_FILES+=("$q")
done
shopt -u nullglob

if [[ ${#QUEUE_FILES[@]} -eq 0 ]]; then
    exit 0
fi

# Read all lines, dedup, keep order of first occurrence.
ALL_LINES=""
for q in "${QUEUE_FILES[@]}"; do
    [[ -f "$q" ]] || continue
    ALL_LINES+="$(cat "$q" 2>/dev/null || true)"$'\n'
done

# Awk-dedup preserving first-occurrence order.
UNIQUE_ABS_PATHS="$(printf '%s' "$ALL_LINES" | awk 'NF && !seen[$0]++')"

# Filter to existing first-party files.
declare -a CANDIDATES=()
while IFS= read -r abs; do
    [[ -z "$abs" ]] && continue
    [[ -f "$abs" ]] || continue
    rel="$(lint_normalize_path "$abs")"
    [[ -z "$rel" ]] && continue
    lint_is_first_party "$rel" || continue
    CANDIDATES+=("$abs")
done <<< "$UNIQUE_ABS_PATHS"

if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    # Nothing to do; tidy up the queue files we read.
    rm -f "${QUEUE_FILES[@]}"
    exit 0
fi

# --- Chunk: process up to N files this invocation --------------------------
CHUNK_SIZE="${SMATCHET_LINT_DRAIN_CHUNK:-10}"
if ! [[ "$CHUNK_SIZE" =~ ^[0-9]+$ ]] || [[ "$CHUNK_SIZE" -le 0 ]]; then
    CHUNK_SIZE=10
fi

declare -a CHUNK=()
declare -a REMAINDER=()
i=0
for abs in "${CANDIDATES[@]}"; do
    if [[ $i -lt $CHUNK_SIZE ]]; then
        CHUNK+=("$abs")
    else
        REMAINDER+=("$abs")
    fi
    i=$((i + 1))
done

# Consume the original per-PID queue files and write the remainder (if any)
# into a single new queue file keyed off the drain's own PID. Next Stop event
# will pick it up.
rm -f "${QUEUE_FILES[@]}"
if [[ ${#REMAINDER[@]} -gt 0 ]]; then
    REMAINDER_FILE="$PROJ_DIR/.claude/.lint-queue.$$"
    printf '%s\n' "${REMAINDER[@]}" > "$REMAINDER_FILE"
fi

# --- Run heavy tools on the chunk ------------------------------------------
ISSUES=""
PROCESSED_RELS=()
for abs in "${CHUNK[@]}"; do
    rel="$(lint_normalize_path "$abs")"
    PROCESSED_RELS+=("$rel")

    cppcheck_out="$(lint_run_cppcheck "$abs")"
    [[ -n "$cppcheck_out" ]] && ISSUES+="$cppcheck_out"$'\n'

    tidy_out="$(lint_run_clang_tidy "$abs" "$rel")"
    [[ -n "$tidy_out" ]] && ISSUES+="$tidy_out"$'\n'

    dual_out="$(lint_run_dual_target "$abs" "$rel")"
    [[ -n "$dual_out" ]] && ISSUES+="$dual_out"$'\n'

    catch_out="$(lint_run_catch_all "$abs")"
    [[ -n "$catch_out" ]] && ISSUES+="$catch_out"$'\n'
done

# --- Pillar 2 static gate (slice 2 of docs/plans/shipped/pillar-1-2-perf-review-system.md) ----
# Runs the canonical harness-agnostic scanner at scripts/dev/pillar2-scan.sh
# against every file in the chunk. Emits CRITICAL per sync-I/O reaching the
# UI thread without /* PILLAR2_WORKER_ONLY */ + est-latency annotation.
if [[ ${#CHUNK[@]} -gt 0 ]]; then
    declare -a P2_RELS=()
    for abs in "${CHUNK[@]}"; do
        P2_RELS+=("$(lint_normalize_path "$abs")")
    done
    # Capture rc (don't swallow with || true). Scanner contract:
    #   rc 0  = clean / WARN-only.
    #   rc 1  = CRITICAL found — surface as a real finding.
    #   rc >= 2 = scanner-internal failure (bad cwd, missing tools, etc.) —
    #             surface verbatim so the broken scanner does not silently
    #             disable Pillar 2 enforcement. (CodeRabbit PR #323 #3.)
    pillar2_out="$(bash "$PROJ_DIR/scripts/dev/pillar2-scan.sh" "${P2_RELS[@]}" 2>&1)"
    pillar2_rc=$?
    if [[ $pillar2_rc -ge 2 ]]; then
        ISSUES+="pillar2-scan internal failure (rc=$pillar2_rc):"$'\n'"$pillar2_out"$'\n'
    elif echo "$pillar2_out" | grep -q "^CRITICAL:"; then
        ISSUES+="$pillar2_out"$'\n'
    fi
fi

# --- Surface findings ------------------------------------------------------
if [[ -n "$ISSUES" ]]; then
    {
        chunk_n=${#CHUNK[@]}
        remain_n=${#REMAINDER[@]}
        if [[ $remain_n -gt 0 ]]; then
            echo "lint-cpp: deferred-drain issues across $chunk_n file(s); $remain_n file(s) remain queued — fix before responding."
        else
            echo "lint-cpp: deferred-drain issues across $chunk_n file(s) — fix before responding."
        fi
        echo "files: ${PROCESSED_RELS[*]}"
        printf '%s' "$ISSUES" | lint_format_issues
    } >&2
    exit 2
fi

exit 0
