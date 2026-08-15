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
# in --bootstrap mode (always-PASS), must NOT trip the guard (status=ok).
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

@test "golden report: rows carry the golden's date + age when a golden exists" {
    write_stub_exe live
    export SMATCHET_GOLDEN_REPORT_FILE="$WORK/golden-report.tsv"
    # First --bootstrap run writes the goldens; the second run diffs against them
    # (SCREENSHOT_DIFF_BIN is the always-rc0 stub), so rows land as `pass` with a
    # real mtime/age rather than the bootstrap placeholder.
    run bash "$DRIVER" --bootstrap
    [ "$status" -eq 0 ]
    : > "$SMATCHET_GOLDEN_REPORT_FILE"
    run bash "$DRIVER"
    [ "$status" -eq 0 ]
    # grep -P is not portable (BSD/busybox); match the literal tab-delimited field.
    grep -q "$(printf '\tpass\t')" "$SMATCHET_GOLDEN_REPORT_FILE"
    # Every pass row's date column must be an ISO date and age a number — that is
    # the staleness signal the job summary renders.
    while IFS=$'\t' read -r scen verdict linf tol mtime age; do
        [ "$verdict" = "pass" ] || continue
        [[ "$mtime" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]
        [[ "$age" =~ ^[0-9]+$ ]]
    done < "$SMATCHET_GOLDEN_REPORT_FILE"
}

@test "golden report: unset SMATCHET_GOLDEN_REPORT_FILE writes nothing (opt-in)" {
    write_stub_exe dead
    unset SMATCHET_GOLDEN_REPORT_FILE
    run bash "$DRIVER"
    [ "$status" -eq 3 ]
    [ ! -e "$WORK/golden-report.tsv" ]
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
