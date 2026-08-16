#!/usr/bin/env bats
# tests/bats/bucket_lane_launch_smoke.bats
# ----------------------------------------------------------------------------
# Bats tests for the bucket-lane broken-lane guard in
# scripts/dev/test-screenshot-diff.sh (Gate 2(b) driver-side hard-exit).
#
# Stubs SMATCHET_EXE on PATH-free (via env) so the driver runs without a real
# build. A stub that emits NO valid capture envelope makes every scenario FAIL
# -> Passed:0 Failed:N -> the broken-lane guard must hard-exit 3 + write
# status=broken to the sentinel. A stub that emits a valid envelope + PNG, run
# in --bootstrap mode (which skips the DIFF, so nothing fails on pixels), must
# NOT trip the guard (status=ok). Bootstrap is not unconditionally green: the
# golden-independent assertions still run, and a failed one (the dock-gap pink
# scan) fails the run and reports `fail-assert` — see the mixed-outcome test.
#
# Also exercises the negative-test fixture (build-log-regex lesson): the broken
# case MUST exit nonzero, asserted explicitly so a regression to pass-through is
# caught at authoring time.
#
# Requires: bash, python (the driver's `extract` helper), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export DRIVER="$REPO_ROOT/scripts/dev/test-screenshot-diff.sh"
    WORK="$(mktemp -d)"
    export WORK
    export SMATCHET_LANE_STATUS_FILE="$WORK/lane-status"
    # Isolate goldens to a scratch dir so the --bootstrap (live) case never
    # clobbers the committed tests/golden/ PNGs.
    export SMATCHET_GOLDEN_DIR="$WORK/golden"
    mkdir -p "$SMATCHET_GOLDEN_DIR"
    # Skip g++ compile of the diff helper — point at a no-op prebuilt that always
    # reports "within tolerance" (rc 0). The broken case never reaches it (the
    # capture envelope is missing first); the ok case runs in --bootstrap (no diff).
    DIFFBIN="$WORK/diffbin"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$DIFFBIN"
    chmod +x "$DIFFBIN"
    export SCREENSHOT_DIFF_BIN="$DIFFBIN"
}

teardown() {
    rm -rf "$WORK"
}

# Write a stub Smatchet.exe. $1 = mode: "dead" (no envelope, scenarios FAIL) or
# "live" (valid envelope + flushes a PNG so the scenario captures).
write_stub_exe() {
    local mode="$1"
    local exe="$WORK/Smatchet.exe"
    if [ "$mode" = "dead" ]; then
        # Emits nothing useful — captureRequested is absent -> every scenario FAILs.
        printf '#!/usr/bin/env bash\necho "{}"\nexit 0\n' > "$exe"
    else
        # Emits a valid envelope AND writes the captured PNG the driver polls for.
        # The PNG must be a REAL decodable image (1x1 black), not placeholder
        # bytes: the dock-gap-sentinel pink-pixel scan stb_image-loads the
        # capture even in --bootstrap mode.
        cat > "$exe" <<'STUB'
#!/usr/bin/env bash
# Find --screenshotPath=<file> and write a tiny PNG there so the driver's
# non-empty-file poll succeeds; print an envelope with captureRequested:true.
out=""
for a in "$@"; do
    case "$a" in --screenshotPath=*) out="${a#--screenshotPath=}" ;; esac
done
[ -n "$out" ] && base64 -d > "$out" <<'PNG'
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGNgYGAAAAAEAAH2FzhVAAAAAElFTkSuQmCC
PNG
echo '{"data":{"captureRequested":true}}'
exit 0
STUB
    fi
    chmod +x "$exe"
    export SMATCHET_EXE="$exe"
}

@test "broken lane: Passed==0 && Failed>0 -> exit 3 + status=broken sentinel" {
    write_stub_exe dead
    run bash "$DRIVER"
    [ "$status" -eq 3 ]
    [[ "$output" == *"Passed: 0"* ]]
    [[ "$output" == *"BROKEN"* ]]
    grep -q '^status=broken' "$SMATCHET_LANE_STATUS_FILE"
}

@test "negative-test: the broken lane does NOT silently exit 0" {
    write_stub_exe dead
    run bash "$DRIVER"
    [ "$status" -ne 0 ]
}

@test "healthy lane: Passed>0 (bootstrap) does NOT trip the broken guard" {
    write_stub_exe live
    run bash "$DRIVER" --bootstrap
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: "* ]]
    grep -q '^status=ok' "$SMATCHET_LANE_STATUS_FILE"
}

# --- masked-step verdict reporting (tooling 2026-08-06 bucket-c-golden-mask) ---
# The CI step that runs this driver is continue-on-error, so a stale golden used
# to be indistinguishable from a clean run. The driver must therefore emit a
# per-scenario verdict row REGARDLESS of outcome — these pin that the rows exist
# on the failing path (the one the mask swallows) and carry the golden's age.

@test "golden report: a FAILING scenario still emits a verdict row" {
    write_stub_exe dead
    export SMATCHET_GOLDEN_REPORT_FILE="$WORK/golden-report.tsv"
    run bash "$DRIVER"
    [ "$status" -eq 3 ]   # broken lane — the masked case
    [ -s "$SMATCHET_GOLDEN_REPORT_FILE" ]
    # One row per registered scenario, each a non-pass verdict.
    n="$(awk '/^SCENARIOS=\(/{f=1;next} f&&/^\)/{exit} f&&NF{c++} END{print c+0}' "$DRIVER")"
    [ "$(wc -l < "$SMATCHET_GOLDEN_REPORT_FILE")" -eq "$n" ]
    # 6 tab-separated columns: scenario, verdict, linf, tol, mtime, age.
    while IFS=$'\t' read -r scen verdict linf tol mtime age; do
        [ -n "$scen" ]
        [ "$verdict" != "pass" ]
        [ -n "$tol" ]
        [ -n "$age" ]
    done < "$SMATCHET_GOLDEN_REPORT_FILE"
}

@test "golden report: rows date the golden from git, not the filesystem mtime" {
    write_stub_exe live
    export SMATCHET_GOLDEN_REPORT_FILE="$WORK/golden-report.tsv"
    # First --bootstrap run writes the goldens; the second diffs against them
    # (SCREENSHOT_DIFF_BIN is the always-rc0 stub), so rows land as `pass`.
    run bash "$DRIVER" --bootstrap
    [ "$status" -eq 0 ]

    # Commit the goldens at a KNOWN old date. mtime is "just now" either way, so
    # a row reporting that old date proves the date came from git — the whole
    # point: on a CI runner every file's mtime is the checkout time, which would
    # report a months-old golden as 0 days old.
    git -C "$SMATCHET_GOLDEN_DIR" init -q
    git -C "$SMATCHET_GOLDEN_DIR" config user.email t@example.com
    git -C "$SMATCHET_GOLDEN_DIR" config user.name t
    git -C "$SMATCHET_GOLDEN_DIR" add -A
    GIT_AUTHOR_DATE="2026-01-02T00:00:00Z" GIT_COMMITTER_DATE="2026-01-02T00:00:00Z" \
        git -C "$SMATCHET_GOLDEN_DIR" commit -q -m goldens

    : > "$SMATCHET_GOLDEN_REPORT_FILE"
    run bash "$DRIVER"
    [ "$status" -eq 0 ]
    # grep -P is not portable (BSD/busybox); match the literal tab-delimited field.
    grep -q "$(printf '\tpass\t')" "$SMATCHET_GOLDEN_REPORT_FILE"
    while IFS=$'\t' read -r scen verdict linf tol gdate age; do
        [ "$verdict" = "pass" ] || continue
        [ "$gdate" = "2026-01-02" ]   # git's commit date, NOT today's mtime
        [[ "$age" =~ ^[0-9]+$ ]]
        [ "$age" -gt 0 ]
    done < "$SMATCHET_GOLDEN_REPORT_FILE"
}

@test "golden report: an undatable golden reports '-', never a fresh-looking 0" {
    write_stub_exe live
    export SMATCHET_GOLDEN_REPORT_FILE="$WORK/golden-report.tsv"
    # Goldens exist but live in no git repo — the date is unknowable. It must
    # read '-' rather than falling back to the mtime (which is "now" on every CI
    # checkout and would report every stale golden as brand new).
    run bash "$DRIVER" --bootstrap
    [ "$status" -eq 0 ]
    : > "$SMATCHET_GOLDEN_REPORT_FILE"
    run bash "$DRIVER"
    [ "$status" -eq 0 ]
    while IFS=$'\t' read -r scen verdict linf tol gdate age; do
        [ "$verdict" = "pass" ] || continue
        [ "$gdate" = "-" ]
        [ "$age" = "-" ]
    done < "$SMATCHET_GOLDEN_REPORT_FILE"
}

@test "golden report: unset SMATCHET_GOLDEN_REPORT_FILE writes nothing (opt-in)" {
    write_stub_exe dead
    unset SMATCHET_GOLDEN_REPORT_FILE
    run bash "$DRIVER"
    [ "$status" -eq 3 ]
    [ ! -e "$WORK/golden-report.tsv" ]
}

@test "golden report: a failed NON-diff assertion never reports as pass" {
    write_stub_exe live
    export SMATCHET_GOLDEN_REPORT_FILE="$WORK/golden-report.tsv"
    # Bootstrap the goldens with a PASSING pink stub so the diff run has something
    # to compare against.
    PINKBIN="$WORK/pinkbin"
    printf '#!/usr/bin/env bash\nexit 0\n' > "$PINKBIN"
    chmod +x "$PINKBIN"
    export PINK_PIXEL_COUNT_BIN="$PINKBIN"
    run bash "$DRIVER" --bootstrap
    [ "$status" -eq 0 ]

    # Now fail ONLY the dock-gap pink scan. The image diff still passes (the diff
    # stub always returns 0), so without propagation the row would read `pass`
    # while the driver counted the scenario failed — the report contradicting the
    # run, which is the failure mode this report exists to prevent.
    printf '#!/usr/bin/env bash\necho "pink pixels: 42"\nexit 1\n' > "$PINKBIN"
    : > "$SMATCHET_GOLDEN_REPORT_FILE"
    run bash "$DRIVER"
    [ "$status" -ne 0 ]   # the driver itself must still fail

    verdict="$(awk -F'\t' '$1=="dock-gap-sentinel"{print $2}' "$SMATCHET_GOLDEN_REPORT_FILE")"
    [ -n "$verdict" ]
    [ "$verdict" != "pass" ]
    [ "$verdict" = "fail-assert" ]
    # Scenarios with no failing assertion are unaffected.
    other="$(awk -F'\t' '$1=="command-palette-fuzzy"{print $2}' "$SMATCHET_GOLDEN_REPORT_FILE")"
    [ "$other" = "pass" ]
}

# --- lane-shape invariant: only lane-integrity may block ------------------------
# The bucket-C lane is advisory for its screenshot-diff purpose; its ONLY teeth are
# the lane-integrity step. Every step after it is pure reporting/observability, and
# each must be continue-on-error — the Actions bash wrapper runs with -e (a body's
# `set -uo pipefail` does not clear it) and `if-no-files-found: ignore` covers only
# an ABSENT file, so an unmasked mkdir/cp/cat or a transient upload error would red
# the lane after the teeth already passed. Asserted here so the property is
# enforced rather than re-argued in review (CodeRabbit flagged three such steps
# across two rounds on PR #2023).
@test "bucket-C workflow: lane-integrity is the lane's only blocking step" {
    python3 -c 'import yaml' 2>/dev/null || skip "PyYAML unavailable"
    run python3 - "$REPO_ROOT/.github/workflows/build-and-test.yml" <<'PY'
import sys, yaml
jobs = yaml.safe_load(open(sys.argv[1]))["jobs"]
job = next(j for j in jobs.values() if "Bucket-C" in str(j.get("name", "")))
steps = job["steps"]
teeth = [i for i, s in enumerate(steps) if "Lane-integrity" in str(s.get("name", ""))]
assert len(teeth) == 1, "expected exactly one lane-integrity step, got %d" % len(teeth)
i = teeth[0]
assert not steps[i].get("continue-on-error", False), "lane-integrity must keep its teeth"
bad = [s.get("name", "?") for s in steps[i + 1:] if not s.get("continue-on-error", False)]
assert not bad, "reporting steps after lane-integrity must be continue-on-error: %s" % bad
# The report's own history requirement: dating a golden needs real history.
checkout = next(s for s in steps if "checkout" in str(s.get("uses", "")))
assert checkout.get("with", {}).get("fetch-depth") == 0, "golden dating needs fetch-depth: 0"
print("OK")
PY
    [ "$status" -eq 0 ] || { echo "$output"; return 1; }
    [[ "$output" == *"OK"* ]]
}

@test "sentinel records exact passed/failed counts" {
    write_stub_exe dead
    run bash "$DRIVER"
    # Every registered scenario FAILs on the missing envelope, so the sentinel
    # must record passed=0 and failed==<scenario count>. Derive the expected
    # count from the driver's own SCENARIOS=( ... ) registry rather than pinning
    # a literal (the count grew 3 -> 7 as scenarios were added and the old
    # literal silently rotted); this asserts the exact-count contract drift-proof.
    n="$(awk '/^SCENARIOS=\(/{f=1;next} f&&/^\)/{exit} f&&NF{c++} END{print c+0}' "$DRIVER")"
    [ "$n" -gt 0 ]
    grep -q "^status=broken passed=0 failed=${n}\$" "$SMATCHET_LANE_STATUS_FILE"
}
