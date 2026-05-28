#!/usr/bin/env bash
# scripts/dev/perf-baseline.sh — manage docs/perf/baselines/<scenarioId>.<host>.json.
#
# Three subcommands:
#
#   list                                 — show all checked-in baselines
#   init <scenario> [--host=dev]         — capture a fresh baseline (fails if
#                                          one already exists for the pair)
#   bump <scenario> [--host=dev] [--yes] — re-capture an existing baseline +
#                                          overwrite the checked-in file
#
# Baselines are PER-HOST per docs/design/archive/pillar-1-2-perf-review-system.md § D1.
# Default host=dev (developer machine, advisory). CI workflows pass host=
# ci-windows-latest for the gating baseline.
#
# This script is the only sanctioned writer of docs/perf/baselines/*.json.
# Anything else (perf-compare.py, perf-run.sh) reads them.

set -euo pipefail

command -v python >/dev/null 2>&1 || { echo "python required" >&2; exit 2; }

usage() {
    cat >&2 <<'USAGE'
Usage:
  bash scripts/dev/perf-baseline.sh list
  bash scripts/dev/perf-baseline.sh init <scenario> [--host=<host>]
  bash scripts/dev/perf-baseline.sh bump <scenario> [--host=<host>] [--yes]

Examples:
  bash scripts/dev/perf-baseline.sh list
  bash scripts/dev/perf-baseline.sh init idle
  bash scripts/dev/perf-baseline.sh bump idle --host=ci-windows-latest --yes
USAGE
    exit 2
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

CMD="${1:-}"
shift || true
SCENARIO=""
HOST="dev"
ASSUME_YES=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host=*) HOST="${1#--host=}"; shift ;;
        --host)
            [ "$#" -ge 2 ] || { echo "perf-baseline: --host requires a value" >&2; usage; }
            HOST="$2"; shift 2 ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --*) echo "unknown flag: $1" >&2; usage ;;
        *)
            if [[ -z "$SCENARIO" ]]; then
                SCENARIO="$1"
            else
                echo "unexpected positional: $1" >&2; usage
            fi
            shift
            ;;
    esac
done

# Allowlist regex for scenario id + host label. Anchors at both ends; rejects
# `..`, `/`, leading dot, empty string. Path traversal in `scenario` / `host`
# would otherwise let `--scenario ../../etc/passwd` (etc.) escape the
# docs/perf/baselines/ directory on write. (CodeRabbit PR #321 #2.)
_BASELINE_ID_RE='^[a-z0-9][a-z0-9._-]*$'

# validate_baseline_arg <label> <value> — exits 2 on mismatch.
validate_baseline_arg() {
    local label="$1"
    local value="$2"
    if ! [[ "$value" =~ $_BASELINE_ID_RE ]]; then
        echo "ERROR: invalid $label='$value' — must match $_BASELINE_ID_RE (lowercase alnum + . _ -, leading alnum)" >&2
        exit 2
    fi
}

baseline_path() {
    validate_baseline_arg "scenario" "$1"
    validate_baseline_arg "host" "$2"
    echo "docs/perf/baselines/${1}.${2}.json"
}

# capture <scenario> <host> <out_path>
# Runs the scenario, writes a fully-formed baseline file at out_path.
capture() {
    local scen="$1"
    local host="$2"
    local outPath="$3"
    local tmpRun
    tmpRun="$(mktemp -t "perf-baseline-XXXXXX.json")"
    # Defer build to perf-run.sh — it knows the .claude/.tree-dirty contract.
    echo "[perf-baseline] capturing $scen ($host) -> $outPath"
    if ! bash scripts/dev/perf-run.sh "$scen" --out="$tmpRun"; then
        echo "FAIL: perf-run.sh exited non-zero for $scen" >&2
        rm -f "$tmpRun"
        return 1
    fi
    if [[ ! -s "$tmpRun" ]]; then
        echo "FAIL: perf-run.sh produced no output for $scen" >&2
        rm -f "$tmpRun"
        return 1
    fi
    local commit
    commit="$(git rev-parse HEAD)"
    local now
    now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    # Re-package the scenario.run JSON envelope into the baseline schema.
    # Python here is required tooling per the 2026-05-20 tooling-required-clis
    # backlog entry; same Python the lint pipeline already uses.
    mkdir -p "$(dirname "$outPath")"
    python - "$tmpRun" "$outPath" "$scen" "$host" "$commit" "$now" <<'PY'
import json, sys, os
src, dst, scen, host, commit, now = sys.argv[1:7]
with open(src, "r", encoding="utf-8") as f:
    raw = json.load(f)
# scenario.run JSON shape: {ok, command, data: {..., rows: [...]}}. Some
# scenarios may embed rows at top-level too — accept either.
rows = []
data = raw.get("data") if isinstance(raw, dict) else None
if isinstance(data, dict) and isinstance(data.get("rows"), list):
    rows = data["rows"]
elif isinstance(raw.get("rows"), list):
    rows = raw["rows"]
# Empty rows means scenario.run captured no markers — writing an
# empty baseline would silently let future regressions pass. Bail.
# (CodeRabbit PR #321 #3.)
if not rows:
    print(f"ERROR: no rows[] payload in {src}; refusing to write empty baseline.", file=sys.stderr)
    sys.exit(2)
baseline = {
    "schemaVersion": 1,
    "scenarioId": scen,
    "captureCommit": commit,
    "captureDate": now,
    "captureHost": host,
    "captureRunnerOs": "windows-msys2-ucrt64",
    "rows": rows,
    "hardwareNote": "",
    "perScenarioPolicyOverride": None,
}
with open(dst, "w", encoding="utf-8") as f:
    json.dump(baseline, f, indent=2)
    f.write("\n")
PY
    rm -f "$tmpRun"
    echo "[perf-baseline] wrote $outPath"
}

case "$CMD" in
    list)
        shopt -s nullglob
        files=(docs/perf/baselines/*.json)
        if [[ ${#files[@]} -eq 0 ]]; then
            echo "(no baselines under docs/perf/baselines/)"
            exit 0
        fi
        printf '%-40s %-22s %s\n' "FILE" "SCENARIO.HOST" "CAPTURE-DATE"
        for f in "${files[@]}"; do
            base="$(basename "$f" .json)"
            date="$(python -c "import json,sys; print(json.load(open(sys.argv[1]))['captureDate'])" "$f" 2>/dev/null || echo "?")"
            printf '%-40s %-22s %s\n' "$f" "$base" "$date"
        done
        ;;
    init)
        if [[ -z "$SCENARIO" ]]; then usage; fi
        OUT="$(baseline_path "$SCENARIO" "$HOST")"
        if [[ -f "$OUT" ]]; then
            echo "FAIL: baseline already exists at $OUT — use 'bump' to overwrite." >&2
            exit 1
        fi
        capture "$SCENARIO" "$HOST" "$OUT"
        ;;
    bump)
        if [[ -z "$SCENARIO" ]]; then usage; fi
        OUT="$(baseline_path "$SCENARIO" "$HOST")"
        if [[ ! -f "$OUT" ]]; then
            echo "FAIL: no existing baseline at $OUT — use 'init' to create." >&2
            exit 1
        fi
        if [[ $ASSUME_YES -ne 1 ]]; then
            echo "About to overwrite $OUT. Continue? [y/N]"
            read -r ans
            case "$ans" in
                y|Y|yes|YES) : ;;
                *) echo "aborted."; exit 1 ;;
            esac
        fi
        capture "$SCENARIO" "$HOST" "$OUT"
        ;;
    *)
        usage
        ;;
esac
