#!/usr/bin/env bash
# scripts/dev/perf-run.sh — single-scenario perf driver for Pillar 1 review.
#
# Wraps the existing `--spawn` ephemeral-instance pattern (see
# `test-grid-edit-perf-postfix.sh` for the prior art). Runs ONE scenario via
# `scenario.run`, writes the result JSON to a known path, prints the file
# location on stdout so the caller (`perf-compare.py`, CI workflow, etc.) can
# pick it up.
#
# Harness-agnostic by design — pure bash, runs identically under Claude Code,
# Codex, Cursor. The driver itself does no comparison; that's perf-compare.py.
#
# Usage:
#   bash scripts/dev/perf-run.sh <scenario-id> [--frames=N] [--out=<path>] [--build]
#
# Examples:
#   bash scripts/dev/perf-run.sh idle
#   bash scripts/dev/perf-run.sh priority-grid-scroll --frames=900 --build
#   bash scripts/dev/perf-run.sh command-palette-fuzzy --out=/tmp/cpf.json
#
# Exit codes:
#   0 — scenario ran cleanly, JSON file written to the reported path
#   1 — scenario failed (build error, exe crash, JSON missing, etc.)
#   2 — bad usage (missing scenario-id, unknown flag)

set -euo pipefail

command -v cmake >/dev/null 2>&1 || { echo "cmake required" >&2; exit 2; }

usage() {
    cat >&2 <<'USAGE'
Usage: bash scripts/dev/perf-run.sh <scenario-id> [--frames=N] [--out=<path>] [--build] [--exe=<path>]

  <scenario-id>     Scenario name as registered in BuiltinCommands.cpp.
                    Run `scenario.list --spawn` to enumerate.
  --frames=N        Frames to run. Default 600.
  --out=<path>      Write result JSON to this path. Default:
                    build/perf-runs/<scenarioId>-<timestamp>.json
  --build           Force a rebuild of SmatchetStandalone before running.
                    Default: rebuild only when .claude/.tree-dirty exists.
  --exe=<path>      Override Smatchet.exe path. Default:
                    build/ninja-iter-msvc/Smatchet.exe
USAGE
    exit 2
}

# -- args --------------------------------------------------------------------
SCENARIO_ID=""
FRAMES=600
OUT_PATH=""
FORCE_BUILD=0
EXE="${SMATCHET_EXE:-build/ninja-iter-msvc/Smatchet.exe}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames=*)      FRAMES="${1#--frames=}"; shift ;;
        --frames)        [ "$#" -ge 2 ] || { echo "perf-run: --frames requires a value" >&2; usage; }
                         FRAMES="$2"; shift 2 ;;
        --out=*)         OUT_PATH="${1#--out=}"; shift ;;
        --out)           [ "$#" -ge 2 ] || { echo "perf-run: --out requires a value" >&2; usage; }
                         OUT_PATH="$2"; shift 2 ;;
        --build)         FORCE_BUILD=1; shift ;;
        --exe=*)         EXE="${1#--exe=}"; shift ;;
        --exe)           [ "$#" -ge 2 ] || { echo "perf-run: --exe requires a value" >&2; usage; }
                         EXE="$2"; shift 2 ;;
        -h|--help)       usage ;;
        --*)             echo "unknown flag: $1" >&2; usage ;;
        *)
            if [[ -z "$SCENARIO_ID" ]]; then
                SCENARIO_ID="$1"
            else
                echo "unexpected positional arg: $1" >&2; usage
            fi
            shift
            ;;
    esac
done

if [[ -z "$SCENARIO_ID" ]]; then
    echo "missing scenario-id" >&2
    usage
fi

# -- paths -------------------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

if [[ -z "$OUT_PATH" ]]; then
    TS="$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p "build/perf-runs"
    OUT_PATH="build/perf-runs/${SCENARIO_ID}-${TS}.json"
fi

# Normalise to forward-slash form so the Smatchet CLI (which runs through
# `cmd.exe` from msys2 in some configurations) sees a path it can open.
case "$OUT_PATH" in
    /*)            ABS_OUT="$OUT_PATH" ;;
    [A-Za-z]:/*)   ABS_OUT="$OUT_PATH" ;;  # Windows drive-letter absolute (e.g. C:/tmp/x.json) — already absolute under MSYS2.
    *)             ABS_OUT="$REPO_ROOT/$OUT_PATH" ;;
esac

# -- build (incremental) -----------------------------------------------------
TREE_DIRTY_FILE="$REPO_ROOT/.claude/.tree-dirty"
if [[ $FORCE_BUILD -eq 1 ]] || [[ -f "$TREE_DIRTY_FILE" ]] || [[ ! -f "$EXE" ]]; then
    echo "[perf-run] building SmatchetStandalone (force=$FORCE_BUILD, dirty=$([ -f "$TREE_DIRTY_FILE" ] && echo y || echo n), exe-present=$([ -f "$EXE" ] && echo y || echo n))..."
    cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
fi

if [[ ! -f "$EXE" ]]; then
    echo "FAIL: $EXE not found after build attempt." >&2
    exit 1
fi

# -- run ---------------------------------------------------------------------
echo "[perf-run] running scenario.run name=$SCENARIO_ID frames=$FRAMES --spawn"
echo "[perf-run] output -> $ABS_OUT"

# Wipe any stale result file at the destination — closes the
# `WaitForFile`-returns-stale-data footgun documented in
# docs/backlog/agent-self-improvement/tooling.md (2026-05-19 entry).
rm -f "$ABS_OUT"

# perf.reset clears the perf monitor's accumulators before the scenario runs;
# scenario.run --outPath ensures the result JSON lands at our path.
"$EXE" cmd perf.reset --spawn --yes >/dev/null 2>&1 || true
"$EXE" cmd scenario.run --name="$SCENARIO_ID" --frames="$FRAMES" --outPath="$ABS_OUT" --spawn --yes

if [[ ! -f "$ABS_OUT" ]]; then
    echo "FAIL: scenario.run reported success but $ABS_OUT does not exist." >&2
    exit 1
fi

echo "[perf-run] done. result: $ABS_OUT"
# Print ONLY the path on the last line so callers can `RESULT=$(perf-run.sh ...)`
# without grepping. Above logs go to stdout too but the path is what matters.
echo "$ABS_OUT"
