#!/usr/bin/env bash
# mobile-emulator-smoke.sh — WS5 item 18 boot smoke for the Android debug APK.
# (docs/plans/mobile-mvp-completion.md.)
#
# Runs INSIDE reactivecircus/android-emulator-runner (the emulator is already
# booted, adb is on PATH, and the repo is checked out). It installs the debug
# APK, launches the NativeActivity, and asserts the native shell reached
# first-frame readiness by polling logcat for the "ImGui ready" marker emitted
# by android_main.cpp (SLOG, tag "Smatchet"). It also notes the window-independent
# "Core booted" marker and fails if the process dies right after first frame.
#
# Advisory — this job is NOT on the merge-gate allow-list and never blocks merge;
# it is an early-warning that the APK boots a real frame on a real (emulated) GPU,
# which neither the .so-link nor the assembleDebug job can prove.
#
# Inputs (env):
#   SMATCHET_APK_PATH        path to the built debug APK (required)
#   SMATCHET_SMOKE_TIMEOUT_S logcat poll budget in seconds (default 150)
set -euo pipefail

PKG="com.smatchet.mobile"
ACT="${PKG}/.SmatchetActivity"
MARKER="ImGui ready"
BOOT_MARKER="Core booted"
TIMEOUT_S="${SMATCHET_SMOKE_TIMEOUT_S:-150}"
APK="${SMATCHET_APK_PATH:?SMATCHET_APK_PATH must point at the built debug APK}"

echo "==> waiting for device + framework boot_completed"
adb wait-for-device
until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
    sleep 2
done

echo "==> uninstalling any stale $PKG (cached-AVD userdata may carry an older install)"
# The cached AVD's userdata can contain a previous run's install signed with THAT
# runner's auto-generated debug keystore — a different key than this run's APK, so
# a bare `install -r` fails INSTALL_FAILED_UPDATE_INCOMPATIBLE. Quick-boot used to
# hide this by rewinding the disk to the pre-install snapshot; the deterministic
# cold boot (-no-snapshot-load) exposes it. Uninstall-if-present keeps the install
# hermetic regardless of cache state; `|| true` — absent package is the clean case.
adb uninstall "$PKG" >/dev/null 2>&1 || true

echo "==> installing $APK"
# -t: debug APKs are stamped android:testOnly="true" by AGP, and adb rejects a
# testOnly package without it (INSTALL_FAILED_TEST_ONLY). Benign for a debug
# build; -t is the standard accept flag.
adb install -r -g -t "$APK"

echo "==> clearing logcat + launching $ACT"
adb logcat -c
adb shell am start -n "$ACT"

echo "==> polling logcat (tag Smatchet) up to ${TIMEOUT_S}s for first-frame marker: '$MARKER'"
deadline=$((SECONDS + TIMEOUT_S))
saw_boot=0
while [ "$SECONDS" -lt "$deadline" ]; do
    dump="$(adb logcat -d -s Smatchet:I 2>/dev/null | tr -d '\r')"
    if printf '%s\n' "$dump" | grep -qF "$BOOT_MARKER"; then
        saw_boot=1
    fi
    if printf '%s\n' "$dump" | grep -qF "$MARKER"; then
        echo "==> saw first-frame marker: '$MARKER'"
        [ "$saw_boot" = 1 ] && echo "==> window-independent '$BOOT_MARKER' marker also present"
        pid="$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')"
        if [ -n "$pid" ]; then
            echo "==> PASS: $PKG alive (pid $pid) and reached '$MARKER'"
            exit 0
        fi
        echo "::error::reached '$MARKER' but $PKG is no longer running (crash after first frame)"
        printf '%s\n' "$dump" | tail -50
        exit 1
    fi
    sleep 3
done

echo "::error::timed out after ${TIMEOUT_S}s without seeing '$MARKER'"
echo "---- last Smatchet logcat ----"
adb logcat -d -s Smatchet:I 2>/dev/null | tr -d '\r' | tail -80 || true
echo "---- recent crash buffer ----"
adb logcat -d -b crash 2>/dev/null | tr -d '\r' | tail -40 || true
exit 1
