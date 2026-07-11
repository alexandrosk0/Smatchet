#!/usr/bin/env bash
# coverage-perfile-gate.sh — per-file >=90% line-coverage floor on a NAMED set of
# high-risk units (security / trust-boundary / parsing). Sibling of coverage.sh
# (global aggregate floor) and coverage-delta-gate.sh (test-delta gate).
#
# WHY a per-file gate on top of the global aggregate floor: the aggregate can clear
# its floor while a single security-critical unit (an SSRF endpoint sanitizer, a
# credential-redaction sweep, a JQL injection escaper) silently rots below it —
# the high blast-radius units are exactly the ones a single uncovered branch can
# turn into a live vulnerability. This gate pins a hard 90% floor on each named
# unit so a regression on a trust-boundary classifier cannot hide inside a healthy
# global number.
#
# UNIT SELECTION (initial high-risk set): every named unit is BOTH high-risk AND
# already near/above 90% on HEAD (each has a dedicated, comprehensive doctest), so
# the gate is GREEN on HEAD by construction — it ratchets the floor, it does not
# demand new coverage to pass today.
#
# INPUT: the Cobertura XML emitted by coverage.sh (coverage/coverage.xml by
# default). OpenCppCoverage's Cobertura export carries one <class filename="...">
# per source file with per-<line hits="N"> rows; we match each named unit's source
# basename against the class filenames (separator-agnostic) and compute
# covered-lines / total-lines across every matching class.
#
# FAIL-OPEN on a missing XML or a unit absent from the XML (a unit that the
# coverage build did not compile — e.g. a feature-gated TU): the gate is a
# ratchet, not a tripwire that wedges CI when coverage did not run or a unit was
# configured out. A unit PRESENT in the XML but below 90% (and not escaped) FAILS.
#
# PER-UNIT ESCAPE (parallel to the *-out-of-band label pattern): set
#   SMATCHET_PERFILE_OOB="unit1,unit2"
# to downgrade a named unit's below-floor failure to a WARN (the CI workflow maps
# a `coverage-perfile-<unit>-out-of-band` PR label onto this env var, mirroring how
# coverage.yml maps `coverage-out-of-band`). Queue a coverage-recovery follow-up
# before relying on an escape.
#
# Usage:
#   bash scripts/dev/coverage-perfile-gate.sh                 # read coverage/coverage.xml, enforce 90%
#   bash scripts/dev/coverage-perfile-gate.sh --xml PATH      # explicit XML path
#   bash scripts/dev/coverage-perfile-gate.sh --floor 85      # override the per-file floor (default 90)
#   SMATCHET_PERFILE_OOB="JqlEscape" bash scripts/dev/coverage-perfile-gate.sh
#
# Exit codes:
#   0 — every named unit at/above the floor, escaped, or fail-open (missing XML/unit)
#   1 — at least one named unit below the floor (and not escaped) / --selftest failure
#   2 — bad arguments / infra error
#
# Self-test (hermetic; no build / exe / OpenCppCoverage needed):
#   bash scripts/dev/coverage-perfile-gate.sh --selftest

set -euo pipefail

# ---------------------------------------------------------------------------
# Named high-risk unit set. Each entry is the SOURCE BASENAME (no dir, no ext)
# whose <class filename> in the Cobertura XML we match. Keep this list to units
# that are (a) trust-boundary / parsing / security and (b) already comprehensively
# tested on HEAD so the gate stays green. Add a unit only with a test that brings
# it to >=90% in the SAME change.
# ---------------------------------------------------------------------------
HIGH_RISK_UNITS=(
    "AiEndpointSanitize"      # SSRF denylist + scheme/host validation on the AI endpoint URL
    "AiErrorRedact"           # provider-error-body credential redaction (Bearer/Basic/api-key/sk-/ghp_)
    "JqlEscape"               # JQL string-literal escape (injection break-out guard)
    "TrackerHttpPure"         # cleartext-base upgrade + transport-error classification + loopback policy
    # 2026-07-10 ratchet — TEST_COVERAGE_GAP_MAP.md Tier-1 pure units, rates measured
    # under their shipping suites with llvm-cov on Linux (same tests the rig runs):
    "TicketGridDurationSortPure"   # untrusted tracker duration-string parser (audit-#3 loop class) — 97%
    "PlaneQuerySuggestEnginePure"  # per-keystroke omnibox parse vs server-sourced catalog — 98%
    "TrackerGridFieldDisplayPure"  # untrusted tracker JSON -> render models via ParseBounded — 95%
    "MergeWatchNotifyPure"         # network-facing notify-endpoint request validation — 100%
    "AiPrefsTestConnectionPure"    # credential-slot routing + endpoint-sanitizer gate — 100% (AI-gated: absent => fail-open)
    "PlaneCustomPropertyPure"      # custom-property serialization from grid strings (C4) — 97%
    # NOT ratcheted: JqlSuggestEnginePure measured 82% under its suite — needs a test
    # top-up to clear the floor before it can join.
)

DEFAULT_FLOOR=90

# ---------------------------------------------------------------------------
# Cobertura per-file coverage extractor.
# perfile_rate <xml-path> <unit-basename>  ->  echoes one of:
#   "MISSING"          unit not present in the XML (fail-open for this unit)
#   "<pct> <cov> <tot>"  integer percent + covered/total line counts
# The Python reads the path + unit via os.environ (never string-interpolated into
# the source) so a hostile path cannot break the program under set -euo pipefail.
# Aggregates <line hits> across EVERY <class> whose filename contains the unit
# basename followed by a C/C++ source extension (.cpp/.cc/.cxx) — separator- and
# case-agnostic on the path, exact on the basename token so "JqlEscape" never
# matches "JqlEscapeExtra".
# ---------------------------------------------------------------------------
perfile_rate() {
    # Feed the Python via a heredoc (NOT `python -c '...'`) so the script's source
    # can freely use single + double quotes in the regexes without clashing with a
    # bash single-quote wrapper. Path + unit cross the boundary via os.environ.
    XML_PATH="$1" UNIT="$2" python - <<'PY'
import os, re, sys
xml = os.environ["XML_PATH"]
unit = os.environ["UNIT"]
try:
    with open(xml, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
except OSError:
    print("MISSING")
    sys.exit(0)

# OpenCppCoverage Cobertura: <class name="..." filename="...\path\Unit.cpp" ...>.
# Match the basename token + a C/C++ extension, separator- and case-agnostic, with
# a word boundary after the ext so "JqlEscape" never matches "JqlEscapeExtra".
# The leading separator is OPTIONAL ((^|[\\/])) so a basename-only filename in the
# XML (e.g. "JqlEscape.cpp" with no path) matches as well as "src/foo/JqlEscape.cpp";
# anchoring to start-or-separator still prevents "ExtraJqlEscape.cpp" from matching.
unit_re = re.compile(r"(?:^|[\\/])" + re.escape(unit) + r"\.(?:cpp|cc|cxx)\b", re.IGNORECASE)
covered = 0
total = 0
matched = False
for m in re.finditer(r"<class\b([^>]*)>(.*?)</class>", text, re.DOTALL):
    attrs, body = m.group(1), m.group(2)
    fn = re.search(r"filename=\"([^\"]*)\"", attrs)
    if not fn:
        continue
    if not unit_re.search(fn.group(1)):
        continue
    matched = True
    for lm in re.finditer(r"<line\b[^>]*\bhits=\"(\d+)\"", body):
        total += 1
        if int(lm.group(1)) > 0:
            covered += 1

if not matched or total == 0:
    print("MISSING")
    sys.exit(0)
pct = int((covered * 100) // total)  # floor — conservative against the threshold
print("%d %d %d" % (pct, covered, total))
PY
}

# is_escaped <unit> — true if the unit appears in the comma-separated SMATCHET_PERFILE_OOB.
is_escaped() {
    local unit="$1" oob="${SMATCHET_PERFILE_OOB:-}" item
    [ -n "$oob" ] || return 1
    local IFS=','
    for item in $oob; do
        # Trim surrounding whitespace.
        item="${item#"${item%%[![:space:]]*}"}"
        item="${item%"${item##*[![:space:]]}"}"
        [ "$item" = "$unit" ] && return 0
    done
    return 1
}

# enforce <xml-path> <floor> — run the gate over HIGH_RISK_UNITS. Exit 0/1.
enforce() {
    local xml="$1" floor="$2"
    if [ ! -s "$xml" ]; then
        echo "[coverage-perfile-gate] fail-open: coverage XML '$xml' missing/empty —"
        echo "[coverage-perfile-gate]   no per-file enforcement (coverage did not run)."
        return 0
    fi
    local failed=0 unit res pct cov tot
    for unit in "${HIGH_RISK_UNITS[@]}"; do
        res="$(perfile_rate "$xml" "$unit")"
        if [ "$res" = "MISSING" ]; then
            echo "[coverage-perfile-gate] $unit: fail-open (absent from XML — not compiled into the coverage build)."
            continue
        fi
        # shellcheck disable=SC2086  # res is a controlled "pct cov tot" triple
        set -- $res
        pct="$1"; cov="$2"; tot="$3"
        if [ "$pct" -ge "$floor" ]; then
            echo "[coverage-perfile-gate] $unit: PASS ${pct}% (${cov}/${tot} lines, floor ${floor}%)"
        elif is_escaped "$unit"; then
            echo "::warning::[coverage-perfile-gate] $unit: ${pct}% < ${floor}% — downgraded to WARN by"
            echo "  SMATCHET_PERFILE_OOB (coverage-perfile-${unit}-out-of-band). Queue a coverage-recovery follow-up."
        else
            echo "[coverage-perfile-gate] $unit: FAIL ${pct}% < floor ${floor}% (${cov}/${tot} lines)" >&2
            failed=1
        fi
    done
    if [ "$failed" -ne 0 ]; then
        echo >&2
        echo "FAIL: one or more high-risk units fell below the ${floor}% per-file coverage floor." >&2
        echo "Add tests to lift the unit back to >=${floor}%, or apply the" >&2
        echo "'coverage-perfile-<unit>-out-of-band' PR label (and queue a recovery follow-up)." >&2
        return 1
    fi
    echo "[coverage-perfile-gate] PASS — all ${#HIGH_RISK_UNITS[@]} high-risk units at/above the ${floor}% floor."
    return 0
}

# ---------------------------------------------------------------------------
# --selftest — hermetic, both-direction fixtures (no build / exe / network).
# selftest: asserts-failure — feeds a synthetic Cobertura XML with one unit BELOW
# the floor and asserts the gate FAILS (exit 1); then asserts the per-unit escape
# downgrades that to PASS; then asserts an all-above-floor XML PASSES; and that a
# missing XML and a missing-unit fail OPEN (exit 0).
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--selftest" ]; then
    st_tmp="$(mktemp -d)" || { echo "coverage-perfile-gate selftest: mktemp failed" >&2; exit 2; }
    trap 'rm -rf "$st_tmp"' EXIT
    st_fail=0

    # A class block with `good`/`bad` mix: count(hits>0)/count(line)=pct.
    # AiEndpointSanitize -> 5/5 = 100% ; JqlEscape -> 1/5 = 20% (below floor).
    write_xml() {
        # $1 = JqlEscape hits pattern (space-separated 0/1 per line)
        local jql_hits="$1"
        {
            echo '<?xml version="1.0"?>'
            echo '<coverage line-rate="0.8">'
            echo ' <packages><package><classes>'
            echo '  <class name="AiEndpointSanitize" filename="C:\src\Source\Core\src\AiEndpointSanitize.cpp"><lines>'
            local i
            for i in 1 2 3 4 5; do echo "   <line number=\"$i\" hits=\"3\"/>"; done
            echo '  </lines></class>'
            echo '  <class name="AiErrorRedact" filename="C:\src\Source\Core\src\AiErrorRedact.cpp"><lines>'
            for i in 1 2 3 4 5 6 7 8 9 10; do echo "   <line number=\"$i\" hits=\"2\"/>"; done
            echo '  </lines></class>'
            echo '  <class name="TrackerHttpPure" filename="C:\src\Source\Core\src\Tracker\TrackerHttpPure.cpp"><lines>'
            for i in 1 2 3 4 5 6 7 8 9 10; do echo "   <line number=\"$i\" hits=\"4\"/>"; done
            echo '  </lines></class>'
            echo '  <class name="JqlEscape" filename="C:\src\Source\Core\src\Tracker\JqlEscape.cpp"><lines>'
            local n=1 h
            for h in $jql_hits; do echo "   <line number=\"$n\" hits=\"$h\"/>"; n=$((n+1)); done
            echo '  </lines></class>'
            echo ' </classes></package></packages>'
            echo '</coverage>'
        }
    }

    set +e
    # (1) JqlEscape 1/5 = 20% -> below floor -> gate FAILS (no escape).
    write_xml "1 0 0 0 0" > "$st_tmp/low.xml"
    ( SMATCHET_PERFILE_OOB="" enforce "$st_tmp/low.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 1 ] || { echo "selftest FAIL: below-floor unit did not FAIL (rc=$rc)" >&2; st_fail=1; }

    # (2) Same XML, but JqlEscape escaped -> downgraded to WARN -> gate PASSES.
    ( SMATCHET_PERFILE_OOB="JqlEscape" enforce "$st_tmp/low.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "selftest FAIL: escaped below-floor unit did not PASS (rc=$rc)" >&2; st_fail=1; }

    # (3) All units >=90% -> gate PASSES.
    write_xml "1 1 1 1 1" > "$st_tmp/high.xml"
    ( SMATCHET_PERFILE_OOB="" enforce "$st_tmp/high.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "selftest FAIL: all-above-floor XML did not PASS (rc=$rc)" >&2; st_fail=1; }

    # (4) Missing XML -> fail-open -> PASSES.
    ( enforce "$st_tmp/does-not-exist.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "selftest FAIL: missing XML did not fail-open (rc=$rc)" >&2; st_fail=1; }

    # (5) A unit absent from an otherwise-valid XML fails OPEN for that unit (overall PASS).
    {
        echo '<?xml version="1.0"?><coverage><packages><package><classes>'
        echo '<class filename="C:\src\Source\Core\src\Unrelated.cpp"><lines><line number="1" hits="1"/></lines></class>'
        echo '</classes></package></packages></coverage>'
    } > "$st_tmp/absent.xml"
    ( enforce "$st_tmp/absent.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "selftest FAIL: all-units-absent XML did not fail-open (rc=$rc)" >&2; st_fail=1; }

    # (6) Exactly-at-floor (9/10 = 90%) must PASS (>= boundary).
    {
        echo '<?xml version="1.0"?><coverage><packages><package><classes>'
        echo '<class filename="...\Source\Core\src\Tracker\JqlEscape.cpp"><lines>'
        for i in 1 2 3 4 5 6 7 8 9; do echo "<line number=\"$i\" hits=\"1\"/>"; done
        echo '<line number="10" hits="0"/>'
        echo '</lines></class>'
        echo '</classes></package></packages></coverage>'
    } > "$st_tmp/boundary.xml"
    # Only JqlEscape present here; the other three fail-open. 90% must PASS.
    ( SMATCHET_PERFILE_OOB="" enforce "$st_tmp/boundary.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 0 ] || { echo "selftest FAIL: exactly-90% boundary did not PASS (rc=$rc)" >&2; st_fail=1; }

    # (7) BASENAME-ONLY filename (no path separator) must still MATCH the unit and be
    # enforced — not mis-classified as MISSING. JqlEscape 1/5 = 20% with a bare
    # "JqlEscape.cpp" filename must FAIL (proving the leading-separator-optional regex
    # matched; a MISSING classification would fail-OPEN to PASS and hide the rot).
    {
        echo '<?xml version="1.0"?><coverage><packages><package><classes>'
        echo '<class filename="JqlEscape.cpp"><lines>'
        echo '<line number="1" hits="1"/>'
        for i in 2 3 4 5; do echo "<line number=\"$i\" hits=\"0\"/>"; done
        echo '</lines></class>'
        echo '</classes></package></packages></coverage>'
    } > "$st_tmp/basename.xml"
    ( SMATCHET_PERFILE_OOB="" enforce "$st_tmp/basename.xml" "$DEFAULT_FLOOR" ) >/dev/null 2>&1
    rc=$?
    [ "$rc" -eq 1 ] || { echo "selftest FAIL: basename-only filename did not MATCH+FAIL (rc=$rc, expected 1 — likely mis-classified MISSING)" >&2; st_fail=1; }
    set -e

    if [ "$st_fail" -eq 0 ]; then
        echo "coverage-perfile-gate --selftest: PASS — below-floor fails, escape + at/above-floor + fail-open pass."
        exit 0
    fi
    echo "coverage-perfile-gate --selftest: FAIL"
    exit 1
fi

# ---------------------------------------------------------------------------
# Normal run.
# ---------------------------------------------------------------------------
cd "$(dirname "$0")/../.."

XML_PATH="coverage/coverage.xml"
FLOOR="$DEFAULT_FLOOR"
while [ $# -gt 0 ]; do
    case "$1" in
        --xml)
            [ $# -ge 2 ] || { echo "FAIL: --xml requires a path" >&2; exit 2; }
            XML_PATH="$2"; shift 2 ;;
        --xml=*) XML_PATH="${1#--xml=}"; shift ;;
        --floor)
            [ $# -ge 2 ] || { echo "FAIL: --floor requires an integer" >&2; exit 2; }
            FLOOR="$2"; shift 2 ;;
        --floor=*) FLOOR="${1#--floor=}"; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if ! [[ "$FLOOR" =~ ^[0-9]+$ ]]; then
    echo "FAIL: --floor must be an integer (got: $FLOOR)" >&2
    exit 2
fi

enforce "$XML_PATH" "$FLOOR"
