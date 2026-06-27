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
# docs/self-improvement/categories/tooling.md (2026-05-19 entry).
rm -f "$ABS_OUT"

# perf.reset clears the perf monitor's accumulators before the scenario runs.
"$EXE" cmd perf.reset --spawn --yes >/dev/null 2>&1 || true

# scenario.run --spawn no longer takes our --outPath: the child confines every
# caller-supplied outPath under <userDataDir>/perf and REJECTS absolute / ".."
# paths (PathConfinement, #1566) — an absolute --outPath now fails with
# kExitValidation(3). Instead the spawn PARENT emits the confined result file's
# full content back in the stdout envelope's `data` field
# (CliCommandRunner SpawnAndRunHandleAsync reads the child-reported confined
# path, then re-emits its content). We capture that envelope and write `data`
# (the bare scenario result, identical to the legacy file format) to $ABS_OUT
# ourselves. This keeps the security confinement intact (no bypass) while still
# landing the result at the caller-visible path perf-compare.py expects. In
# --spawn mode the child's own stdout/stderr go to its log file, so the parent's
# stdout is exactly the one envelope line.
#
# The envelope is authoritative over the exit code: on the CI runner the
# post-scenario app.quit handshake can flake non-zero AFTER the scenario
# completed and emitted its ok:true envelope (observed on the perf-gate-revival
# step-3 run: scenarios printed ok:true JSON, then set -e killed this script on
# the CLI's exit code and the workflow counted them as run_failures). Tolerate
# rc!=0 iff the envelope carries a usable data block.
rc=0
spawn_err="$(mktemp)"
trap 'rm -f "$spawn_err"' EXIT
spawn_out="$("$EXE" cmd scenario.run --name="$SCENARIO_ID" --frames="$FRAMES" --spawn --yes 2>"$spawn_err")" || rc=$?

if ! printf '%s' "$spawn_out" | python -c "
import json, sys
try:
    env = json.loads(sys.stdin.read())
except Exception as e:
    sys.stderr.write('scenario.run --spawn: stdout is not valid JSON: %s\n' % e)
    sys.exit(1)
data = env.get('data')
if not isinstance(data, dict) or not data.get('rows'):
    sys.stderr.write('scenario.run --spawn: envelope has no data.rows\n')
    sys.exit(1)
with open(sys.argv[1], 'w', encoding='utf-8') as f:
    json.dump(data, f)
" "$ABS_OUT"; then
    echo "FAIL: scenario.run --spawn produced no usable result envelope (exit=$rc)." >&2
    printf '%s\n' "----- captured stdout -----" "$spawn_out" >&2
    printf '%s\n' "----- captured stderr -----" >&2; cat "$spawn_err" >&2
    exit 1
fi
if [[ "$rc" -ne 0 ]]; then
    echo "[perf-run] WARN: scenario.run exited rc=$rc but the result envelope is valid (quit-handshake flake) — continuing." >&2
fi
# Guard against a torn/empty write: the file must parse as JSON with rows.
# Mirrors extract_rows() in perf-compare.py: when `data` exists and carries
# `rows`, that IS the row set — an empty data.rows must FAIL, never fall back
# to a top-level `rows`. (CodeRabbit PR #937 #2.)
if ! python -c "
import json, sys
d = json.load(open(sys.argv[1], encoding='utf-8'))
data = d.get('data')
if isinstance(data, dict) and 'rows' in data:
    rows = data.get('rows')
else:
    rows = d.get('rows')
sys.exit(0 if isinstance(rows, list) and rows else 1)
" "$ABS_OUT"; then
    echo "FAIL: $ABS_OUT exists but is not valid JSON with non-empty rows (exit=$rc)." >&2
    exit 1
fi

echo "[perf-run] done. result: $ABS_OUT"
# Print ONLY the path on the last line so callers can `RESULT=$(perf-run.sh ...)`
# without grepping. Above logs go to stdout too but the path is what matters.
echo "$ABS_OUT"
