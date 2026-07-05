# Chromebook port — investigation & feasibility

> **Status**: research findings, not shipped code. Code grounded against `develop` HEAD on **2026-06-20**.
> No Chromebook, Android NDK/SDK, or X11 dev headers were available in the authoring environment, so
> **nothing here was compile- or device-verified** — see § Validation gaps. The intent is that a future
> execution slice starts from facts, not a blank page.

## Why this is research-only

A Chromebook port is **not a single mechanical change** — it is a target-selection decision (ChromeOS
exposes two viable runtimes) followed by a port to whichever is chosen. Both candidate paths are blocked
from validation *in this environment*: the Android path needs an NDK + an ARC-capable device, and the
Linux path needs the desktop app to build on Linux for the first time (the cloud authoring box has no
X11 dev headers — a `find_package(X11 REQUIRED)` configure failure, the same blocker noted in
[`mobile-app-fuller-integration.md`](../plans/shipped/mobile-app-fuller-integration.md) § Validation
blocker). Per [`AI_POLICY.md`](../../AI_POLICY.md) *escalate-when-unvalidatable*, the deliverable is a
findings doc + a backlog entry, not blind-shipped code.

## ChromeOS app-delivery surfaces

A modern Chromebook can run an app three ways. Smatchet already has a runnable artifact for one of them.

| Surface | What it is | Smatchet artifact | Verdict |
|---|---|---|---|
| **ARC** (Android Runtime for Chrome) | Android apps in the ChromeOS Android runtime (ARC++ **container** on older devices; **ARCVM** on newer, ~2022+); on by default on most consumer Chromebooks | **`SmatchetMobile` APK already builds** (`arm64-v8a` + `x86_64`) — [`ANDROID_BUILD.md`](ANDROID_BUILD.md) | ✅ **Viable — fastest path** |
| **Crostini** (Linux dev environment) | Debian VM; runs native Linux GUI apps over Wayland (Sommelier) + Xwayland | The `SmatchetStandalone` GLFW/OpenGL3 host — **never built on Linux yet** (all Linux presets are headless, `SMATCHET_BUILD_APP=OFF`) | ✅ **Viable — best desktop UX, more work** |
| **PWA / Web** | Browser/WASM | none | ❌ Non-goal (see § Path C) |

Crucially, **both Chromebook CPU families are already covered by the APK**: most Chromebooks are x86_64
(Intel/AMD), and the growing ARM segment (MediaTek/Qualcomm) is `arm64-v8a` — exactly the two `abiFilters`
in [`Source/Mobile/AndroidApp/app/build.gradle`](../../Source/Mobile/AndroidApp/app/build.gradle).

---

## Path A — Android app on ARC (fastest to first light)

ChromeOS runs the existing APK with **no new build target**. The work is *input/UX ergonomics*, because
ARC on a Chromebook is a keyboard + trackpad + large-resizable-window environment, while the Android host
and mobile shell were authored for a single-touch phone. All gaps live in the thin Android shim
(`Source/Mobile/Android/*`, the Java activity, the manifest) and the mobile-shell layer — **`Source/Core`
is untouched**.

### What already works on ARC

- **Rendering**: EGL + OpenGL ES 3.0 (`SmatchetAndroidEgl.cpp`) is fully supported by ARC.
- **Lifecycle**: focus/pause/resume, surface teardown/rebuild on `APP_CMD_TERM_WINDOW`/`INIT_WINDOW`, and
  density re-resolve + atlas rebuild on `APP_CMD_CONFIG_CHANGED` (`android_main.cpp` `OnAppCmd`) — the same
  machinery ChromeOS drives on window resize/snap/maximize.
- **Secrets**: hardware-backed Android Keystore (AES-GCM) via `SmatchetAndroidSecretBridge.*` (P1.0,
  shipped) — already correct on ChromeOS.
- **Text entry**: soft-IME + hardware-key *characters* via the JNI bridge (`SmatchetAndroidImeBridge.*`,
  `SmatchetActivity.java` `dispatchKeyEvent`).
- **Density / safe-area**: density scaling + work-area insets are already polled and applied per frame.

### Gaps for a keyboard + trackpad Chromebook

| # | Gap | Where | Fix sketch |
|---|---|---|---|
| A1 | **No right-click / secondary button** — only `ImGuiMouseButton_Left` is forwarded | `imgui_impl_android` path via `OnInputEvent` (`android_main.cpp`); mobile-shell click check (`SmatchetMobileShellUi.cpp`) | Extend the Android input backend to map `AMOTION_EVENT_BUTTON_SECONDARY` → `io.AddMouseButtonEvent(1,…)` |
| A2 | **No scroll wheel** — and mobile shell sets `ImGuiWindowFlags_NoScrollWithMouse` | `SmatchetMobileShellUi.cpp` (NoScrollWithMouse flag) | Forward `AMOTION_EVENT_AXIS_VSCROLL`→`io.AddMouseWheelEvent`; drop/condition the flag on desktop mode |
| A3 | **Hardware shortcuts lost** (Ctrl+S/Z/F) — `dispatchKeyEvent` extracts only `getUnicodeChar`, not key codes + modifiers | `SmatchetActivity.java` `dispatchKeyEvent` | Add a key-event channel (parallel to the IME char queue) that maps Android keycodes+meta → `io.AddKeyEvent` |
| A4 | **Imprecise pointer / no hover tooltips** — single touch-contact point only | Android input backend | Feed precise `io.MousePos` from `ACTION_HOVER_MOVE` / pointer coords |
| A5 | **Cursor hidden** — `ImGuiConfigFlags_NoMouseCursorChange` is set (touch host) | `android_main.cpp` `InitImGuiFirstTime` | Clear the flag when a pointer device / Chromebook is detected |
| A6 | **Touch density too large for a trackpad** — mobile shell defaults to `Comfortable` (1.6×) and a width‑hysteresis auto-switch | `SmatchetMobileShellUi.cpp`, `SmatchetUiModeIds.h` | Default to **Desktop UI mode** (1.0×) on ChromeOS; the desktop dockspace UI is the better fit |
| A7 | **Freeform/snapped resize** must be confirmed to drive surface recreation | `OnAppCmd` (`INIT_WINDOW`/`CONFIG_CHANGED`) | Verify on-device that ChromeOS window resize fires the surface/config events; handle if not |
| A8 | **Play-Store / no-touch filtering** — manifest declares no `android.hardware.touchscreen required="false"`, so the app may be hidden from non-touch Chromebooks | [`AndroidManifest.xml`](../../Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml) | Add `<uses-feature android:name="android.hardware.touchscreen" android:required="false"/>` (+ `faketouch`); keep `resizeableActivity` true |

> **Manifest gate note:** any manifest edit (A8) **must keep `android:allowBackup="false"`** (line 7) —
> the **Android security gate** (`mobile-security.yml` → `test-mobile-security.sh`) is a non-required-but
> **blocking** merge check that fails on an `allowBackup` regression and on incomplete per-ABI OpenSSL.
>
> **ChromeOS detection (A4–A6):** gate these desktop-input defaults on the standard ARC system feature —
> `getPackageManager().hasSystemFeature("org.chromium.arc")` (Java) — and pass the result to the native
> shell so it can enable the cursor (A5), default to **Desktop UI mode** (A6), and accept precise-pointer
> input (A4) only on ChromeOS, leaving phone/tablet behaviour untouched.

**Effort:** ~1–2 focused weeks, almost entirely in the Android input backend + a small key-event bridge +
a one-line manifest `uses-feature` + a Chromebook-detection → Desktop-mode default. No `Source/Core`
changes, so the dual-target (DX12/Unreal) purity and the strict-zone lint contract are unaffected.

**Distribution:** sideload via `adb install` for testing; Google Play (with the no-touch `uses-feature`)
or an unlisted/internal track for release.

---

## Path B — Linux desktop app via Crostini (best desktop UX)

This builds the **existing desktop `SmatchetStandalone`** (GLFW + OpenGL3) for Linux and runs it in
Crostini. The payoff is the full keyboard/mouse/resizable-window desktop UI the app was *designed* for —
no mobile shell, no touch compromises — plus, as a side benefit, a **general Linux desktop build** usable
well beyond Chromebooks. The cost is that the desktop app **has never been built on Linux**.

### What's already ready (the core is POSIX-clean)

The shared core compiles cleanly through the non-Windows side of every platform `#ifdef` — there is a CI
gate that proves exactly this:

- **`posix-core-check`** preset (`CMakePresets.json`): *"compiles `CORE_SOURCES` with host Linux clang
  through the non-`_WIN32` (#else) side of every platform `#ifdef`"* (advisory job). Companion Linux
  presets exist for sanitizers/fuzzing (`ninja-tsan-linux`, `ninja-fuzzer-linux`, base
  `_smatchet-clang-linux-base`).
- **Config/data paths**: XDG already implemented — `$XDG_CONFIG_HOME/Smatchet` ↦ `~/.config/Smatchet`
  (`ConfigManager_PathUtils.cpp`).
- **Subprocess**: full POSIX `fork`/`execvp` + `select` path (`SubprocessCapture.cpp` `RunPosix`).
- **Open URL**: `xdg-open` branch already wired (`AppController.cpp` `OpenUrl`).
- **Crash handling**: POSIX signal handlers (`SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL`) +
  `std::set_terminate` + platform-agnostic breadcrumbs (`SmatchetCrashHandler.cpp`, `CrashSink.cpp`).
- **Clipboard**: delegated to ImGui/GLFW — no direct OS calls.
- **CMake knobs**: the [`generic-cmake-cross-platform`](../plans/shipped/generic-cmake-cross-platform.md)
  work already made MSVC/Clang equal-citizen and added `SMATCHET_BUILD_STANDALONE` /
  `SMATCHET_BUILD_APP` gating + `find_package(OpenGL)`/GLFW wiring — so this is *"turn the app on for
  Linux and fix the residue,"* not greenfield.

### Gaps for a Crostini build

| # | Gap | Where | Fix sketch |
|---|---|---|---|
| B1 | **No Linux *app* build exists** — every Linux preset is `SMATCHET_BUILD_APP=OFF`; GLFW/standalone is never compiled on Linux (the `generic-cmake` plan listed Linux app packaging as out of scope) | `CMakePresets.json` | Add a `SMATCHET_BUILD_APP=ON` Linux preset; install GLFW's X11/GL dev set (`xorg-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev`); fix any first-build compile/link residue |
| B2 | **File picker is Windows-only** — non-Win32 returns `false` (no dialog) | `Win32PickFiles.cpp` (`#else` stub), host seam `HostCallbacks::OpenFilePaths` | Inject a Crostini picker via the existing host callback — `zenity --file-selection` / `kdialog` through the POSIX `SubprocessCapture`, or an in-app browser |
| B3 | **Secrets are plaintext on Linux** (mode 0600 only) | `ConfigManager_PathUtils.cpp` POSIX branch | Optional libsecret / Secret Service seam for Crostini; keep the existing desktop-Linux plaintext warning honest |
| B4 | **P4v launcher Windows-only** | `P4vLaunch.cpp` (`#else` returns false) | Low priority (P4 tooling rarely present in Crostini); stub or `p4v`-if-present |
| B5 | **No minidump on Linux** (signals + breadcrumbs work; no `dbghelp`) | `SmatchetCrashHandler.cpp` | Optional `backtrace()`/`backtrace_symbols()` capture |
| B6 | **Crostini GPU path** — GL runs over Sommelier + virglrenderer; OpenGL3 generally works but must be perf-checked against **Pillar 1** (≤ 6.94 ms steady-state) | runtime | On-device perf pass; fall back / tune if virgl is slow |
| B7 | **Packaging** — Crostini surfaces `.desktop` apps in the launcher | release-eng | `.deb` + `.desktop` + icon (separate release-engineering slice) |
| B8 | **Crostini is opt-in** — not enabled by default; absent on some low-end/managed/enterprise Chromebooks | n/a (platform) | Accept; this is the main reason ARC reaches more devices |

**Effort:** larger than Path A — first-ever Linux app build (B1) + the file picker (B2) + packaging (B7)
are the bulk; the rest is optional polish. Most of it is mechanical given the POSIX-clean core and the
CMake knobs already in place. A `SMATCHET_BUILD_APP=ON` Linux CI job (xvfb + mesa, or a real Crostini
runner) would be both the validation gate and a regression guard.

---

## Path C — Web / WASM (non-goal)

Dear ImGui compiles to WASM (Emscripten + WebGL), but Smatchet leans on a stack of native dependencies —
SQLite (`LocalCacheManager`), `cpr`/libcurl + OpenSSL TLS, POSIX subprocess (P4 annotate, scenarios),
and OS file/secret integration — none of which survive a browser sandbox without major rework or a
server-side proxy. **Out of scope.** If a thin web client is ever wanted it should be designed as its own
product, not a port.

---

## Recommendation — phased

| Phase | Target | Why | Effort |
|---|---|---|---|
| **1 — first light** | **ARC** (existing APK + input/UX shim) | The binary already builds for both Chromebook ABIs; remaining work is a contained input-ergonomics shim + a manifest line. Reaches the most devices (ARC is on by default). | ~1–2 wks |
| **2 — best desktop UX + Linux value** | **Crostini** (`SMATCHET_BUILD_APP=ON` Linux desktop) | Full keyboard/mouse/resizable-window fidelity matching the app's desktop design; also yields a general Linux desktop release. | larger |

**Rationale.** Smatchet is a keyboard-and-mouse desktop productivity tool, so the *end-state* with the
best fit is the Crostini desktop build (Path B) — it reuses the real desktop UI and adds a Linux release
for free. But the *shortest path to a Chromebook running Smatchet* is ARC (Path A): the APK already
exists for x86_64 + arm64, and the only thing between it and a usable Chromebook app is the desktop-input
shim. Do Path A first for reach and speed; pursue Path B for the high-fidelity desktop experience. The
two are independent — neither blocks the other, and the input lessons from A inform B.

## Validation gaps (what this could NOT confirm)

- **No Chromebook / ARC device** was available, so every Path-A ergonomics claim (right-click, wheel,
  hardware shortcuts, freeform resize, no-touch filtering) is **code-read inference**, pending an on-device
  pass. The existing `mobile-emulator-smoke` job boots an x86_64 emulator but does not exercise
  ChromeOS-specific freeform-window / trackpad / no-touchscreen behaviour.
- **No Android NDK/SDK** here → no APK rebuild to test manifest/input changes.
- **No X11 dev headers** here → the desktop app could not be built on Linux even once, so Path B's "what
  doesn't compile/link the first time" (B1) is genuinely unknown until attempted on a real Linux box.
- ChromeOS platform behaviours (ARC input mapping, Sommelier/virgl GL, Crostini packaging) are stated from
  established platform documentation, not from a Smatchet run.

**First task of any execution slice:** obtain a Chromebook with both ARC and Crostini enabled, plus the
Android toolchain ([`ANDROID_BUILD.md`](ANDROID_BUILD.md)) and a Linux desktop build box.

## Backlog linkage & next steps

- This investigation should be tracked as a product-debt / roadmap entry under
  [`docs/self-improvement/categories/`](../self-improvement/categories/) (e.g. a `debt/` entry, mirroring
  the Pillar-4 mobile a11y entry) and, if a port is greenlit, promoted to a `docs/plans/active/` execution
  plan per [`AGENTS.md`](../../AGENTS.md) § Process rules.
- Path A overlaps the Android roadmap
  ([`mobile-app-fuller-integration.md`](../plans/shipped/mobile-app-fuller-integration.md)) — the
  desktop-input shim (A1–A5) is reusable by any external-keyboard Android user, not just Chromebooks, so
  it could ride that plan rather than a separate one.
- Path B overlaps the (shipped) cross-platform CMake work
  ([`generic-cmake-cross-platform.md`](../plans/shipped/generic-cmake-cross-platform.md)), whose explicit
  out-of-scope list ("Linux/macOS app packaging") is exactly Path B's scope.

## References

- ChromeOS — ARC (Android apps), Crostini (Linux dev environment, Sommelier/Wayland + Xwayland),
  app-window resize/freeform model; Android `uses-feature android.hardware.touchscreen required=false`
  for non-touch Chromebooks; Play large-screen / ChromeOS app-quality guidance.
- Smatchet Android host — [`ANDROID_BUILD.md`](ANDROID_BUILD.md);
  [`Source/Mobile/Android/`](../../Source/Mobile/Android/) (`android_main.cpp`, `SmatchetAndroidEgl.*`,
  `SmatchetAndroidImeBridge.*`); [`SmatchetActivity.java`](../../Source/Mobile/AndroidApp/app/src/main/java/com/smatchet/mobile/SmatchetActivity.java);
  [`AndroidManifest.xml`](../../Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml).
- Smatchet POSIX seams — `Source/Core/src/Config/ConfigManager_PathUtils.cpp` (XDG paths, POSIX secrets),
  `Source/Core/src/SubprocessCapture.cpp` (`RunPosix`), `Source/Core/src/AppController.cpp` (`OpenUrl`
  `xdg-open`), `Source/Standalone/SmatchetCrashHandler.cpp` (POSIX signals),
  `Source/Core/src/Win32PickFiles.cpp` (`#else` stub), `Source/Core/src/Ui/P4vLaunch.cpp` (`#else` stub).
- Build — `CMakePresets.json` (`posix-core-check`, `_smatchet-clang-linux-base`, `ninja-tsan-linux`,
  `ninja-fuzzer-linux`, `android-ndk-arm64`); merge gate `mobile-security.yml` (Android security gate).
- Mobile shell / UI — `Source/Core/src/Ui/SmatchetMobileShellUi.cpp`,
  `Source/Core/include/Ui/SmatchetUiModeIds.h`.
