#!/usr/bin/env bash
# mutation-smoke.sh — assertion-strength gate (testing-surface-roadmap.md Slice F).
#
# Drives a JSON corpus of single-point production mutants against an
# assertion-based test rig (default: SmatchetTsanTests / ninja-tsan-linux, the
# only Linux-runnable doctest oracle — see MUTATION_PILOT.md § Phase 0). For each
# mutant: assert the tree is clean -> confirm the search text occurs exactly once
# -> apply the exact edit -> incrementally rebuild -> run the rig -> classify
# KILLED (rig failed) / SURVIVED (rig passed) / BUILD_FAIL (discarded) -> revert
# -> re-assert clean. NO mutant is ever left in the tree: a clean-tree assertion
# brackets every mutant and an EXIT trap reverts on any interruption.
#
# Equivalent mutants (expect=equivalent) change no observable behaviour, so no
# test can kill them; they are applied-and-checked for honesty (a KILLED
# equivalent means the corpus ruling is wrong) but excluded from the kill-rate
# denominator so the floor can't be gamed by unkillable mutants.
#
# Modes:
#   --list                 print the corpus (id/file/expect) and exit; no build
#   --dry-run              exercise apply/revert + single-occurrence checks for
#                          every mutant WITHOUT building/running (mechanics proof;
#                          works on any host, no TSan runtime needed)
#   (default)             full sweep; with --gate, exit non-zero below the floor
#   --gate                 fail (exit 1) when the adjusted kill rate < floor
#   --floor N              kill-rate floor percent (default 80)
#   --corpus PATH          corpus JSON (default scripts/dev/mutation-smoke-corpus.json)
#   --preset NAME          cmake build preset (default ninja-tsan-linux)
#   --exe PATH             test exe (default build/<preset>/tests/SmatchetTsanTests)
#   --id ID[,ID...]        run only these corpus ids
#
# Bypass (CI): SMATCHET_SKIP_MUTATION_SMOKE=1 (logged).
#
# Exit codes: 0 ok (or floor met) · 1 floor not met (--gate) or fatal · 2 usage
#             · 3 corpus/tree integrity error (dirty tree, ambiguous search)
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

CORPUS="scripts/dev/mutation-smoke-corpus.json"
PRESET="ninja-tsan-linux"
EXE=""
FLOOR=80
MODE="run"      # run | list | dry
GATE=0
ONLY_IDS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --list) MODE="list" ;;
        --dry-run) MODE="dry" ;;
        --gate) GATE=1 ;;
        --floor) FLOOR="${2:?}"; shift ;;
        --floor=*) FLOOR="${1#*=}" ;;
        --corpus) CORPUS="${2:?}"; shift ;;
        --corpus=*) CORPUS="${1#*=}" ;;
        --preset) PRESET="${2:?}"; shift ;;
        --preset=*) PRESET="${1#*=}" ;;
        --exe) EXE="${2:?}"; shift ;;
        --exe=*) EXE="${1#*=}" ;;
        --id) ONLY_IDS="${2:?}"; shift ;;
        --id=*) ONLY_IDS="${1#*=}" ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "mutation-smoke: unknown arg '$1'" >&2; exit 2 ;;
    esac
    shift
done

if [ "${SMATCHET_SKIP_MUTATION_SMOKE:-}" = "1" ]; then
    echo "mutation-smoke: SKIPPED (SMATCHET_SKIP_MUTATION_SMOKE=1)"
    exit 0
fi

[ -f "$CORPUS" ] || { echo "mutation-smoke: corpus not found: $CORPUS" >&2; exit 3; }
command -v python3 >/dev/null || { echo "mutation-smoke: python3 required" >&2; exit 2; }
[ -n "$EXE" ] || EXE="build/${PRESET}/tests/SmatchetTsanTests"

# --- tree-clean guard: refuse to run on a dirty tree; revert on interruption ---
assert_clean() {
    if ! git diff --quiet --ignore-submodules -- . 2>/dev/null; then
        echo "mutation-smoke: FATAL — working tree not clean (a mutant may be un-reverted)" >&2
        git status --porcelain -- . | head >&2
        exit 3
    fi
}
CUR_FILE=""
revert_current() {
    if [ -n "$CUR_FILE" ]; then
        git checkout -- "$CUR_FILE" 2>/dev/null || true
        CUR_FILE=""
    fi
}
trap 'revert_current' EXIT INT TERM

assert_clean

# --- read corpus into parallel arrays via python (jq may be absent) ---------
mapfile -t ROWS < <(python3 - "$CORPUS" "$ONLY_IDS" <<'PY'
import json, sys
corpus = json.load(open(sys.argv[1]))
only = set(x for x in sys.argv[2].split(',') if x) if len(sys.argv) > 2 else set()
for m in corpus:
    if only and m['id'] not in only:
        continue
    # tab-separated; search/replace base64 to survive newlines/quotes
    import base64
    def b(s): return base64.b64encode(s.encode()).decode()
    print('\t'.join([m['id'], m['file'], m.get('expect','killed'),
                     b(m['search']), b(m['replace']), m.get('note','').replace('\t',' ')]))
PY
)

[ "${#ROWS[@]}" -gt 0 ] || { echo "mutation-smoke: no mutants selected"; exit 0; }

if [ "$MODE" = "list" ]; then
    printf '%-14s %-8s %s\n' "ID" "EXPECT" "FILE"
    for r in "${ROWS[@]}"; do IFS=$'\t' read -r id file expect _s _r _n <<<"$r"
        printf '%-14s %-8s %s\n' "$id" "$expect" "$file"; done
    echo "(${#ROWS[@]} mutants)"
    exit 0
fi

# apply exactly one occurrence of search->replace in file (fail if 0 or >1)
apply_mutant() {
    local file="$1" search_b64="$2" replace_b64="$3"
    python3 - "$file" "$search_b64" "$replace_b64" <<'PY'
import sys, base64
path, sb, rb = sys.argv[1], sys.argv[2], sys.argv[3]
search = base64.b64decode(sb).decode()
replace = base64.b64decode(rb).decode()
src = open(path).read()
n = src.count(search)
if n != 1:
    sys.stderr.write(f"AMBIGUOUS: search text occurs {n}x (need exactly 1) in {path}\n")
    sys.exit(3)
open(path, 'w').write(src.replace(search, replace, 1))
PY
}

killed=0; survived=0; buildfail=0; equiv_checked=0; equiv_bad=0
declare -a SURV_IDS=()

for r in "${ROWS[@]}"; do
    IFS=$'\t' read -r id file expect s_b64 r_b64 note <<<"$r"
    [ -f "$file" ] || { echo "  $id: FILE-MISSING ($file) — skipped" >&2; continue; }
    assert_clean
    CUR_FILE="$file"
    if ! apply_mutant "$file" "$s_b64" "$r_b64"; then
        revert_current; echo "  $id: SPEC-ERROR (search not unique) — skipped" >&2; continue
    fi

    if [ "$MODE" = "dry" ]; then
        # mechanics only: confirm the edit changed the file, then revert
        if git diff --quiet -- "$file"; then
            echo "  $id: DRY-FAIL (edit was a no-op)" >&2
        else
            echo "  $id: dry-ok (applied+reverting) [$expect]"
        fi
        revert_current
        continue
    fi

    # incremental build (1 TU + relink); build failure => discard the mutant
    if ! cmake --build --preset "$PRESET" >/dev/null 2>&1; then
        buildfail=$((buildfail+1)); echo "  $id: BUILD_FAIL (discarded)"
        revert_current; continue
    fi
    # run the rig: exit 0 => mutant SURVIVED; non-zero => KILLED
    if "$EXE" >/dev/null 2>&1; then
        verdict="SURVIVED"
    else
        verdict="KILLED"
    fi
    revert_current

    if [ "$expect" = "equivalent" ]; then
        equiv_checked=$((equiv_checked+1))
        [ "$verdict" = "KILLED" ] && { equiv_bad=$((equiv_bad+1)); echo "  $id: $verdict — WARNING: corpus marks this equivalent but a test killed it (re-classify)"; } \
                                  || echo "  $id: $verdict (equivalent, excluded from floor)"
        continue
    fi
    if [ "$expect" = "survived" ]; then
        # Known assertion gap (backlogged). Informational, excluded from the floor.
        # When its backlog fix lands the rig KILLS it — nudge to flip expect=killed
        # so it becomes a regression guard.
        [ "$verdict" = "KILLED" ] && echo "  $id: $verdict — a backlogged gap is now covered; flip corpus expect -> killed ($note)" \
                                  || echo "  $id: $verdict (known gap, excluded from floor: $note)"
        continue
    fi
    if [ "$verdict" = "KILLED" ]; then killed=$((killed+1)); echo "  $id: KILLED"
    else survived=$((survived+1)); SURV_IDS+=("$id"); echo "  $id: SURVIVED  <- weak assertion ($note)"; fi
done

assert_clean
trap - EXIT INT TERM

if [ "$MODE" = "dry" ]; then
    echo "mutation-smoke: dry-run complete — apply/revert mechanics OK for ${#ROWS[@]} mutants; tree clean."
    exit 0
fi

scored=$((killed+survived))
echo "----------------------------------------------------------------"
echo "mutation-smoke: killed=$killed survived=$survived build_fail=$buildfail  (equivalents: $equiv_checked checked, $equiv_bad mis-ruled)"
if [ "$scored" -eq 0 ]; then
    echo "mutation-smoke: no scorable (non-equivalent) mutants ran — nothing to gate."
    [ "$equiv_bad" -gt 0 ] && exit 1 || exit 0
fi
rate=$(( killed * 100 / scored ))
echo "mutation-smoke: adjusted kill rate = ${rate}% (floor ${FLOOR}%)"
[ "${#SURV_IDS[@]}" -gt 0 ] && echo "mutation-smoke: survivors: ${SURV_IDS[*]}"

if [ "$equiv_bad" -gt 0 ]; then
    echo "mutation-smoke: FAIL — an equivalent-ruled mutant was killed; the corpus ruling is stale."
    [ "$GATE" -eq 1 ] && exit 1
fi
if [ "$rate" -lt "$FLOOR" ]; then
    echo "mutation-smoke: BELOW FLOOR."
    [ "$GATE" -eq 1 ] && exit 1
fi
echo "mutation-smoke: OK."
exit 0
