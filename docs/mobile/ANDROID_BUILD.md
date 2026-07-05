# Building the Smatchet Android App (`SmatchetMobile` `.so` → APK)

> First-time, bare-machine build guide for the Android target. Every version, path, flag, and command below is concrete and copy-pasteable. Where a value is machine-specific, a **Reference machine (known-good)** callout shows the exact example from a confirmed working build; the surrounding prose states the general requirement.

---

## 1. Overview

Smatchet is built from one shared, engine-agnostic `Source/Core` against **three targets**:

| Target | Kind | Notes |
|---|---|---|
| `SmatchetStandalone` | Desktop executable | GLFW + desktop GL. Build instructions: [`docs/agent-rules/build.md`](../agent-rules/build.md). |
| `SmatchetCore_DX12` | Unreal static lib | DX12 dual-target. |
| **`SmatchetMobile`** | **Android shared library** | `libSmatchetMobile.so`, packaged into an APK — **this document**. |

`SmatchetMobile` is the Android sibling of `SmatchetStandalone`: it boots the same `AppController` + `PluginHost` + `SmatchetUI` stack, but over **EGL/GLES3 + `imgui_impl_android` + `imgui_impl_opengl3`** instead of GLFW/desktop-GL. It is defined only inside `if(ANDROID)` in the repo-root `CMakeLists.txt` and links against `native_app_glue log android EGL GLESv3`.

**Toolchain chain (what calls what):**

```
./gradlew assembleDebug
   └─ AGP (com.android.application 8.5.2)
        └─ externalNativeBuild → CMake (>= 3.24, from cmake.dir — NOT the SDK's bundled 3.22.1)
             └─ Android NDK clang (26.3.11579264, toolchain android.toolchain.cmake)
                  └─ compiles CORE_SOURCES + Source/Mobile/Android/*.cpp
                       └─ links static OpenSSL (per-ABI libssl.a/libcrypto.a) via cpr
                            └─ libSmatchetMobile.so
                                 └─ packaged → app-debug.apk
```

> The single biggest first-time trap is the **CMake version conflict** (the SDK ships only CMake 3.22.1, the repo needs ≥ 3.24). It is solved in two coordinated places — see [§3.5](#35-install-external-cmake--324) and [§7](#7-troubleshooting). Read that before your first build.

**Terminology:** `<repo>` below means your local checkout root (the directory containing the top-level `CMakeLists.txt`). On the reference machine that is `C:/Dev/trees/mobile-phase0-slice3` (a worktree); a fresh clone is whatever you pass to `git clone` ([§3.1](#31-clone-the-repo)).

---

## 2. Prerequisites

### Toolchain matrix

| Tool | Required version | Notes |
|---|---|---|
| **JDK** | **17** (any vendor) | AGP 8.5 requires JDK 17. `JAVA_HOME` must point here. |
| **Android SDK** | with the components below | Bootstrapped from `cmdline-tools;latest`, then filled in via `sdkmanager` ([§3.3](#33-bootstrap-the-android-sdk--components)). |
| &nbsp;&nbsp;`cmdline-tools` | `latest` | Provides `sdkmanager` / `avdmanager`. |
| &nbsp;&nbsp;`platform-tools` | latest | Provides `adb`. |
| &nbsp;&nbsp;`platforms;android-34` | **android-34** | `compileSdk = 34`, `targetSdk = 34`. |
| &nbsp;&nbsp;`build-tools;34.0.0` | **34.0.0** | |
| &nbsp;&nbsp;`ndk;26.3.11579264` | **26.3.11579264** | Pinned in `app/build.gradle` (`ndkVersion`) and mirrored by CI. Do not substitute. |
| &nbsp;&nbsp;`emulator` + `system-images;android-34;google_apis;x86_64` | for emulator runs | Only needed to run the APK on an emulator ([§6](#6-run-on-emulator--device)). |
| **External CMake** | **≥ 3.24** | **Required separately from the SDK.** The SDK's bundled CMake is only **3.22.1** (the newest `sdkmanager` offers) and is **too old** — root `CMakeLists.txt` line 1 is `cmake_minimum_required(VERSION 3.24)`. Install a standalone CMake ≥ 3.24 and point `cmake.dir` at it ([§3.5](#35-install-external-cmake--324)). |
| **Ninja** | recent | The Android CMake generator is Ninja. **Not bundled with the standalone CMake on Windows** — install it separately and put it on `PATH` ([§3.6](#36-install-ninja)). |
| **make + perl** | recent | Needed **only to build OpenSSL** ([§3.7](#37-build-static-openssl-for-both-abis)). `perl` ships with Git for Windows (the repo vendors small first-party shims for the few `Configure` modules Git's perl is stripped of — **no MSYS2 / Strawberry Perl needed**); **`make` does not** — install `mingw32-make` ([§3.7](#37-build-static-openssl-for-both-abis)). |
| **OpenSSL** | **3.5.6** | Built as **static, per-ABI** libs (`libssl.a` + `libcrypto.a`). SHA-256 of the source tarball: `deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736`. **Hard link requirement** — see [§3.7](#37-build-static-openssl-for-both-abis). |
| **Android Gradle Plugin (AGP)** | **8.5.2** | Declared `apply false` in `build.gradle`; applied without a version in `app/build.gradle`. |
| **Gradle** | **8.7** (`-bin`) | Supplied by the committed wrapper (`distributionUrl = …/gradle-8.7-bin.zip`). Use `./gradlew` — do not install Gradle separately. |
| **min / target SDK** | minSdk **24**, targetSdk **34** | `ANDROID_PLATFORM = android-24`; this must equal `__ANDROID_API__` used to build OpenSSL. |
| **ABIs** | `arm64-v8a`, `x86_64` | `abiFilters` lists both. `x86_64` = emulator at native speed; `arm64-v8a` = physical devices + CI parity. A full release APK needs **both**; `x86_64`-only is the fast inner loop. |

> **Reference machine (known-good — example config, this session):**
> - **OS:** Windows 11. Builds run from **Git Bash** (carries the full Windows environment — see [§7 TRAP 3](#7-troubleshooting)).
> - **JDK:** Microsoft OpenJDK 17, `JAVA_HOME = C:/Program Files/Microsoft/jdk-17.0.19.10-hotspot`.
> - **Android SDK root:** `C:/Android/sdk`, with `cmdline-tools;latest`, `platform-tools`, `platforms;android-34`, `build-tools;34.0.0`, `ndk;26.3.11579264`.
> - **External CMake:** `C:/Program Files/CMake` (**CMake 4.3.0-rc3** — satisfies `≥ 3.24`).
> - **Ninja:** installed separately via `winget install Ninja-build.Ninja`, resolved on `PATH` (**not** co-located with CMake).
> - **make:** `mingw32-make` (from a WinLibs / CLion MinGW install on `PATH`); `perl` from Git for Windows (with the repo's [`scripts/mobile/openssl/perl-shim/`](../../scripts/mobile/openssl/perl-shim/) on `PERL5LIB` — **no MSYS2**).
> - **OpenSSL:** static, per-ABI, at `C:/Android/openssl-android/{x86_64,arm64-v8a}`. This example machine has `x86_64` built; CI builds `arm64-v8a` (the `mobile-android-ndk` / `-apk` jobs) and `x86_64` (the emulator-smoke job), and a release APK needs **both** ABIs.

---

## 3. One-time setup

Do these once per machine, in order. After this, the only repeated command is `./gradlew assembleDebug` ([§4](#4-building-the-apk)).

### 3.0 Set environment variables (do this FIRST, every shell)

These must be present for **every** Gradle invocation — including the wrapper bootstrap — because any Gradle task configures the `:app` module, which loads AGP, which needs the SDK location. Missing them yields `SDK location not found` or `JAVA_HOME is not set` ([TRAP 2](#7-troubleshooting)). They are also read by the SDK bootstrap ([§3.3](#33-bootstrap-the-android-sdk--components)) and the OpenSSL build ([§3.7](#37-build-static-openssl-for-both-abis)), so set them before anything else.

```bash
# --- JDK 17 ---
export JAVA_HOME="/c/Program Files/Microsoft/jdk-17.0.19.10-hotspot"
export PATH="$JAVA_HOME/bin:$PATH"

# --- Android SDK (set BOTH; different tools read different vars) ---
export ANDROID_SDK_ROOT="C:/Android/sdk"
export ANDROID_HOME="C:/Android/sdk"

# --- NDK (used by the OpenSSL build and the CMake toolchain file) ---
export ANDROID_NDK_ROOT="C:/Android/sdk/ndk/26.3.11579264"

# --- OpenSSL base — its /${ANDROID_ABI} subdir is derived per ABI by smatchet_prepare_cpr() ---
export SMATCHET_ANDROID_OPENSSL_BASE="C:/Android/openssl-android"

# --- Put the SDK tools + Ninja on PATH ---
export PATH="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin:$PATH"   # sdkmanager, avdmanager
export PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"             # adb
export PATH="$ANDROID_SDK_ROOT/emulator:$PATH"                   # emulator (if installed)
```

Notes:
- **Forward slashes** (`C:/Android/sdk`) work everywhere a shell consumes the value and avoid backslash-escaping headaches. The one place that needs Windows-style escaping is `local.properties` ([§3.4](#34-create-localproperties)).
- `SMATCHET_ANDROID_OPENSSL_BASE` is consumed by `app/build.gradle`, which appends `-DSMATCHET_ANDROID_OPENSSL_BASE=<base>` to the CMake arguments. `smatchet_prepare_cpr()` then derives `<base>/${ANDROID_ABI}` when `OPENSSL_ROOT_DIR` is unset. You can equivalently pass it as a Gradle property: `-PsmatchetOpensslBase=<base>`.
- Run builds from a shell that carries the **full** Windows environment (**Git Bash** or a normal terminal). An MSYS2 `-lc` login shell strips `USERPROFILE`/`LOCALAPPDATA` and breaks ccache ([TRAP 3](#7-troubleshooting)).

### 3.1 Clone the repo

The Android tree (`Source/Mobile/` + the Gradle project) is on `develop`:

```bash
git clone https://github.com/alexandrosk0/Smatchet.git <repo>
cd <repo>
```

### 3.2 Install JDK 17

Install any JDK 17 distribution and capture its install path for `JAVA_HOME` ([§3.0](#30-set-environment-variables-do-this-first-every-shell)).

```powershell
# Windows (PowerShell), reference-machine choice:
winget install Microsoft.OpenJDK.17
```

> **Reference machine:** Microsoft OpenJDK 17 at `C:/Program Files/Microsoft/jdk-17.0.19.10-hotspot`. Any JDK 17 works — AGP 8.5 simply requires major version 17.

Verify:

```bash
"$JAVA_HOME/bin/java" -version   # must report a 17.x VM
```

### 3.3 Bootstrap the Android SDK + components

The SDK is bootstrapped from the **command-line tools** zip, then everything else is installed with `sdkmanager`.

**(a) Download + unzip `cmdline-tools`, then fix the nested layout.** `sdkmanager` insists the tools live at `<sdk>/cmdline-tools/latest/...`. The official zip unpacks to a top-level `cmdline-tools/` folder, so you must move it into a `latest/` subdir:

```bash
# ANDROID_SDK_ROOT must already be set (§3.0)
mkdir -p "$ANDROID_SDK_ROOT/cmdline-tools"
cd "$ANDROID_SDK_ROOT/cmdline-tools"

# Grab the current command-line tools zip from https://developer.android.com/studio
# (the "Command line tools only" download). Example file name shown:
curl -fsSL -o cmdline-tools.zip \
  "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"
jar xf cmdline-tools.zip        # 'jar' ships with the JDK; no separate unzip needed

# The zip unpacks to ./cmdline-tools — rename it to ./latest (the layout sdkmanager wants)
mv cmdline-tools latest
# Result: $ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager(.bat)
```

> On Windows the SDK download is a `commandlinetools-win-*.zip`; on Linux/CI use `commandlinetools-linux-*.zip`. The unzip-then-`mv-to-latest` dance is identical. (`jar xf` is used instead of `unzip` because the JDK is already installed and Git Bash has no `unzip`.)

**(b) Accept licenses + install the component set:**

```bash
SDKMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"

# Accept all licenses (idempotent)
yes 2>/dev/null | "$SDKMANAGER" --licenses >/dev/null 2>&1 || true

# Install the exact component set (add the emulator packages only if you'll run an AVD)
"$SDKMANAGER" \
  "cmdline-tools;latest" \
  "platform-tools" \
  "platforms;android-34" \
  "build-tools;34.0.0" \
  "ndk;26.3.11579264" \
  "emulator" \
  "system-images;android-34;google_apis;x86_64"
```

After this the NDK is at `$ANDROID_SDK_ROOT/ndk/26.3.11579264` — already pointed at by `ANDROID_NDK_ROOT` ([§3.0](#30-set-environment-variables-do-this-first-every-shell)).

> **Do NOT** install the SDK-bundled `cmake;3.22.1` package and expect it to be used — it is too old, and even if installed, AGP is deliberately steered away from it ([§3.5](#35-install-external-cmake--324)).

### 3.4 Create `local.properties`

`local.properties` is **machine-local and gitignored** ([TRAP 8](#7-troubleshooting)) — never commit it. Create it at `Source/Mobile/AndroidApp/local.properties` with **two keys**:

```properties
# Java .properties escaping: backslashes are doubled, the drive colon is escaped as \:
sdk.dir=C\:\\Android\\sdk
cmake.dir=C\:\\Program Files\\CMake
```

- `sdk.dir` — the Android SDK root (must contain NDK `26.3.11579264`, `platforms;android-34`, `build-tools;34.0.0`).
- `cmake.dir` — the **external CMake ≥ 3.24** install ([§3.5](#35-install-external-cmake--324)); the SDK ships only 3.22.1.

> **Reference machine logical values:** `sdk.dir = C:\Android\sdk`, `cmake.dir = C:\Program Files\CMake`. In a `.properties` file each `\` is written `\\` and the drive `:` is written `\:`, hence the escaped form above. (Forward slashes — `sdk.dir=C:/Android/sdk` — also work and avoid escaping entirely.)

### 3.5 Install external CMake ≥ 3.24

**Why this is mandatory and separate.** Root `CMakeLists.txt` begins with `cmake_minimum_required(VERSION 3.24)` (and `CMakePresets.json` declares `cmakeMinimumRequired 3.24.0`). The Android SDK only offers **CMake 3.22.1**. So you must install a standalone CMake ≥ 3.24 and make AGP use it.

```powershell
# Windows (PowerShell), reference-machine choice:
winget install Kitware.CMake
```

**Two coordinated settings are required** (both, or it silently falls back to the SDK's 3.22.1):

1. **`local.properties` → `cmake.dir`** points at the external CMake install ([§3.4](#34-create-localproperties)).
2. **`app/build.gradle` → `externalNativeBuild.cmake version '3.24.0+'`** (already committed). The trailing **`+`** ("this version or higher") is essential: if the requested version *exactly* matches an SDK-installed package (e.g. `'3.22.1'`), AGP uses the SDK copy and **ignores `cmake.dir`**. Requesting a version the SDK does not ship (here, ≥ 3.24 via `3.24.0+`) forces AGP to resolve CMake from `cmake.dir` / `PATH` instead.

Verified end-to-end: with `cmake.dir` set and `'3.24.0+'` requested, AGP picks up the external CMake and configure succeeds. (CI is unaffected — Ubuntu's CMake ≥ 3.28 already satisfies `3.24.0+`.)

> **Reference machine:** external CMake at `C:/Program Files/CMake` (**CMake 4.3.0-rc3**).

### 3.6 Install Ninja

The Android CMake generator is **Ninja**, and on Windows it is **not** bundled with the standalone CMake install — install it separately and make sure it is on `PATH`:

```powershell
# Windows (PowerShell), reference-machine choice:
winget install Ninja-build.Ninja
```

```bash
ninja --version   # confirm it resolves on PATH
```

> **Reference machine:** Ninja installed via WinGet (`Ninja-build.Ninja`), resolved through `PATH` — there is no `ninja.exe` under `C:/Program Files/CMake/bin`. On Linux/CI install via the package manager (`apt-get install -y ninja-build`).

### 3.7 Build static OpenSSL for BOTH ABIs

**Hard requirement to link.** cpr **forces the OpenSSL TLS backend on Android** (`smatchet_prepare_cpr()` in `cmake/SmatchetThirdParty.cmake` FORCE-sets `CPR_FORCE_OPENSSL_BACKEND ON` + `OPENSSL_USE_STATIC_LIBS TRUE` inside `if(ANDROID)`). `libSmatchetMobile.so` **cannot link** without static `libssl.a` + `libcrypto.a` for the **target ABI**. Build OpenSSL **per ABI before** building the app.

**Pins (mirror CI):** OpenSSL **3.5.6**, SHA-256 `deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736`, NDK **26.3.11579264**, `__ANDROID_API__` = **24** (must equal `ANDROID_PLATFORM = android-24`).

**Prereqs — `make` + `perl` (NO MSYS2):**
- **`perl`** ships with **Git for Windows** (Git Bash) — already present. Git's perl is *stripped* of three pure-perl modules `./Configure` pulls in; the repo vendors first-party shims for exactly those at [`scripts/mobile/openssl/perl-shim/`](../../scripts/mobile/openssl/perl-shim/) and puts them on `PERL5LIB` — so **no MSYS2 or Strawberry Perl is required** (the helper script in the next step wires this up for you). The module cascade + why-shims-not-the-17-real-files is in [`perl-shim/README.md`](../../scripts/mobile/openssl/perl-shim/README.md).
- **`make` is NOT in Git Bash.** Install **MinGW `mingw32-make`** (native Win32 — `winget install BrechtSanders.WinLibs.POSIX.UCRT`, or a CLion-bundled MinGW) **or** `choco install make`. The helper script and snippets below invoke it as `mingw32-make`. *(Do **not** use MSYS2's `make` — its `-lc` login shell strips `USERPROFILE`/`LOCALAPPDATA` and breaks ccache; see [TRAP 3](#7-troubleshooting).)*
- On Linux/CI: `sudo apt-get install -y make perl` (CI's perl is not stripped, so no shim is needed there).

**Choose a scratch dir and a base directory.** The base holds one subdir per ABI:

```
<base>/
├── x86_64/      {lib/libssl.a, lib/libcrypto.a, include/openssl/...}
└── arm64-v8a/   {lib/libssl.a, lib/libcrypto.a, include/openssl/...}
```

> **Reference machine:** `<base> = C:/Android/openssl-android` → `C:/Android/openssl-android/x86_64` and `C:/Android/openssl-android/arm64-v8a`. This layout is exactly what `SMATCHET_ANDROID_OPENSSL_BASE` expects ([§3.0](#30-set-environment-variables-do-this-first-every-shell) / [TRAP 5](#7-troubleshooting)).

**One command — the helper script (recommended).** From the **repo root**, with `mingw32-make` on `PATH` (prereqs above):

```bash
export ANDROID_NDK_ROOT="C:/Android/sdk/ndk/26.3.11579264"
export SMATCHET_ANDROID_OPENSSL_BASE="C:/Android/openssl-android"   # <base>
bash scripts/mobile/openssl/build-android-openssl.sh
```

It downloads + SHA-256-verifies OpenSSL 3.5.6, then Configures + builds + installs **both** ABIs into `<base>/{arm64-v8a,x86_64}` — exactly the layout `SMATCHET_ANDROID_OPENSSL_BASE` expects. Under the hood it runs Git's perl with the repo [`perl-shim/`](../../scripts/mobile/openssl/perl-shim/) on `PERL5LIB`, invokes perl as `/usr/bin/perl` (space-free — the space in `C:\Program Files\Git\…` otherwise breaks the generated Makefile's unquoted `$(PERL)` recipes), and finishes with an `llvm-objdump -f` architecture check per ABI (`aarch64` / `x86_64`). **No MSYS2.** Env overrides: `OPENSSL_TARBALL=<path>` (skip the download), `MAKE=<prog>` / `MAKE_JOBS=<n>`, `ANDROID_API=<n>`, `WORK_DIR=<dir>`. ~10 min per ABI (one-time; CI caches it).

<details>
<summary><b>Manual build — what the script does, step by step</b></summary>

**Download + verify the source (once):**

```bash
SCRATCH="C:/Android/scratch"            # any writable working dir
mkdir -p "$SCRATCH" && cd "$SCRATCH"
curl -fsSL \
  "https://github.com/openssl/openssl/releases/download/openssl-3.5.6/openssl-3.5.6.tar.gz" \
  -o openssl.tar.gz
echo "deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736  openssl.tar.gz" | sha256sum -c -
```

**Put the NDK clang toolchain on `PATH`** and the perl-shim on `PERL5LIB`:

```bash
# Windows (Git Bash): windows-x86_64 prebuilt. Linux/CI: swap in linux-x86_64.
export PATH="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/windows-x86_64/bin:$PATH"
# Git's perl is stripped of three Configure modules — the repo shims cover them:
export PERL5LIB="<repo>/scripts/mobile/openssl/perl-shim"
```

**Build each ABI into `<base>/<abi>`** (`--libdir=lib` pins the layout — some hosts default to `lib64`). OpenSSL configures **in-tree**, so extract a *fresh* tree per ABI; copy the shim into it too so `mingw32-make`'s `perl -I.` recipes resolve it; and run perl as `/usr/bin/perl` (space-free):

```bash
SHIM="<repo>/scripts/mobile/openssl/perl-shim"
for pair in "android-arm64:arm64-v8a" "android-x86_64:x86_64"; do
  target="${pair%%:*}"; abi="${pair##*:}"
  rm -rf "$SCRATCH/$target" && mkdir "$SCRATCH/$target" && cd "$SCRATCH/$target"
  tar xzf "$SCRATCH/openssl.tar.gz" && cd "openssl-3.5.6"
  cp -r "$SHIM/." .                       # make recipes find the shim via -I.
  /usr/bin/perl ./Configure "$target" -D__ANDROID_API__=24 \
    no-shared no-tests no-apps no-docs --libdir=lib \
    --prefix="$SMATCHET_ANDROID_OPENSSL_BASE/$abi"
  mingw32-make -j8                        # 'make -j' on Linux/CI
  mingw32-make install_sw
done
```

</details>

**Verify each ABI produced the static libs:**

```bash
test -f "$SMATCHET_ANDROID_OPENSSL_BASE/x86_64/lib/libssl.a"
test -f "$SMATCHET_ANDROID_OPENSSL_BASE/x86_64/lib/libcrypto.a"
test -f "$SMATCHET_ANDROID_OPENSSL_BASE/x86_64/include/openssl/opensslv.h"
# repeat for arm64-v8a
```

---

## 4. Building the APK

All commands run from `<repo>/Source/Mobile/AndroidApp/`. The Gradle wrapper jar is committed, so `./gradlew` works immediately (no separate Gradle install). All environment variables from [§3.0](#30-set-environment-variables-do-this-first-every-shell) must be exported in the shell first.

### Fast inner loop — `x86_64`-only (CONFIRMED known-good, exit 0)

```bash
cd <repo>/Source/Mobile/AndroidApp
# (env vars from §3.0 already exported in this shell)
./gradlew assembleDebug -Pandroid.injected.build.abi=x86_64 --console=plain
```

`-Pandroid.injected.build.abi=x86_64` does two things:
1. **Skips arm64-v8a** (whose OpenSSL you may not have built yet).
2. **Redirects the APK output** to `app/build/intermediates/apk/debug/app-debug.apk` (**not** the usual `outputs/` path — see [§5](#5-output--verification)).

A harmless note prints because `abiFilters` still lists both ABIs:

```
There are no .so files available to package in the APK for arm64-v8a
```

### Full build — both ABIs

```bash
cd <repo>/Source/Mobile/AndroidApp
# (env vars from §3.0 already exported in this shell)
./gradlew assembleDebug --console=plain
```

No `-P` ABI flag → builds **both** `arm64-v8a` and `x86_64`, requires **both** OpenSSL ABIs present, and writes to the standard `app/build/outputs/apk/debug/app-debug.apk`.

**Build-time expectations (reference machine):** first full native build (curl + cpr + core) ~6 min; incremental header-touch rebuild ~1.5 min.

---

## 5. Output & verification

### APK output path depends on the build mode

| Build | APK path |
|---|---|
| `x86_64`-only (`-Pandroid.injected.build.abi=x86_64`) | `app/build/intermediates/apk/debug/app-debug.apk` |
| Full / both-ABI (no `-P` flag) | `app/build/outputs/apk/debug/app-debug.apk` |

### Confirm the embedded native library

```bash
# 'jar tf' lists archive contents using the JDK that's already installed (no 'unzip' needed)
jar tf app/build/intermediates/apk/debug/app-debug.apk | grep libSmatchetMobile
# expect: lib/x86_64/libSmatchetMobile.so   (and lib/arm64-v8a/... on a full build)
```

### Confirmed sizes (reference machine, `x86_64`-only debug)

- `app-debug.apk` = **25.3 MB**
- `lib/x86_64/libSmatchetMobile.so` = **24 MB stripped** (from **166 MB** unstripped debug)

The target name `SmatchetMobile` (CMake `add_library(... SHARED)`) matches the manifest `android.app.lib_name = SmatchetMobile`, so the `NativeActivity` loads exactly this `.so`.

---

## 6. Run on emulator / device

### Emulator (`x86_64`, native speed on an x86 host)

Requires the `emulator` package + an x86_64 system image ([§3.3](#33-bootstrap-the-android-sdk--components)) and the SDK tool dirs on `PATH` ([§3.0](#30-set-environment-variables-do-this-first-every-shell)).

```bash
# 1. Create an x86_64 AVD (API 34). The AVD ABI must match the .so you built (x86_64).
echo "no" | avdmanager create avd \
  -n smatchet_x86_64 \
  -k "system-images;android-34;google_apis;x86_64" \
  --force

# 2. Launch the emulator (background it; -no-snapshot for a clean boot)
emulator -avd smatchet_x86_64 -no-snapshot -gpu auto &

# 3. Wait for the device, then for full boot
adb wait-for-device
while [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != "1" ]; do sleep 2; done

# 4. Install the APK (point at the correct output path from §5)
adb install -r app/build/intermediates/apk/debug/app-debug.apk

# 5. Launch the real activity (from AndroidManifest.xml)
adb shell am start -n com.smatchet.mobile/.SmatchetActivity
```

- Package / launch identity: `applicationId = namespace = com.smatchet.mobile`; launcher activity `com.smatchet.mobile.SmatchetActivity` (declared as `.SmatchetActivity`, `android:exported="true"`, `MAIN` + `LAUNCHER`).
- **Acceleration:** for a usable emulator, enable hardware acceleration — **WHPX** (Windows Hypervisor Platform, the modern path: enable "Windows Hypervisor Platform" in *Turn Windows features on or off*) or, on older setups, **Intel HAXM**. Without it the x86_64 emulator runs in slow software rendering. Verify with `emulator -accel-check`.

### Physical arm64 device

Build with the arm64 ABI present (full both-ABI build, or `-Pandroid.injected.build.abi=arm64-v8a`) — this **requires the `arm64-v8a` OpenSSL** from [§3.7](#37-build-static-openssl-for-both-abis). Then `adb wait-for-device && adb install -r …` and the same `am start` line. (`arm64-v8a` is also the ABI CI validates, for device parity.)

---

## 7. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Configure fails: CMake too old / `cmake_minimum_required(VERSION 3.24)` not satisfied; or AGP keeps using **3.22.1**. | The SDK ships only CMake **3.22.1**; the repo needs **≥ 3.24**. AGP also **ignores `cmake.dir`** if the requested version *exactly* matches an SDK-installed package. | **Both** required: (a) set `cmake.dir` in `local.properties` to an external CMake ≥ 3.24; (b) keep `app/build.gradle` `externalNativeBuild.cmake version '3.24.0+'`. The **`+`** ("or higher" / non-SDK version) forces AGP to resolve CMake from `cmake.dir`/`PATH`. ([§3.5](#35-install-external-cmake--324)) |
| `'ninja' is not recognized` / generator not found. | Ninja is not on `PATH` (it is **not** bundled with the standalone CMake on Windows). | Install Ninja separately (`winget install Ninja-build.Ninja`) and ensure it resolves on `PATH`. ([§3.6](#36-install-ninja)) |
| `SDK location not found` / `JAVA_HOME is not set` — even on `gradle wrapper`. | `JAVA_HOME` and/or `ANDROID_SDK_ROOT`/`ANDROID_HOME` not exported. Any Gradle task configures `:app`, which loads AGP, which needs the SDK location. | Export `JAVA_HOME`, `ANDROID_HOME`, **and** `ANDROID_SDK_ROOT` before *any* Gradle invocation (including the wrapper bootstrap). ([§3.0](#30-set-environment-variables-do-this-first-every-shell)) |
| ccache breaks / user-profile lookups fail mid-build. | An MSYS2 `-lc` login shell strips `USERPROFILE` and `LOCALAPPDATA`. | Run from a shell with the **full** Windows environment — **Git Bash** or a normal terminal (both inherit the complete Windows env). ([§3.0](#30-set-environment-variables-do-this-first-every-shell)) |
| OpenSSL build: `make: command not found`. | `make` is **not** in Git for Windows (only `perl` is). | Install **`mingw32-make`** (WinLibs `winget install BrechtSanders.WinLibs.POSIX.UCRT`, or CLion MinGW) or `choco install make` — **no MSYS2 needed**; the helper script and snippets invoke it as `mingw32-make`. ([§3.7](#37-build-static-openssl-for-both-abis)) |
| OpenSSL `Configure`: `Can't locate Locale/Maketext/Simple.pm` (or `ExtUtils/MakeMaker.pm`, `Pod/Usage.pm`) in `@INC`. | Git-for-Windows perl is stripped of these pure-perl modules. | Put the repo shims on `PERL5LIB`: `export PERL5LIB="<repo>/scripts/mobile/openssl/perl-shim"` — the helper script does this automatically. **No MSYS2 / Strawberry Perl.** ([§3.7](#37-build-static-openssl-for-both-abis)) |
| OpenSSL `make`: `/usr/bin/sh: /c/Program: No such file or directory`. | The space in `C:\Program Files\Git\…\perl` baked into the generated Makefile's unquoted `$(PERL)` recipes. | Invoke perl by its space-free POSIX path: `/usr/bin/perl ./Configure …` (the helper script does this). ([§3.7](#37-build-static-openssl-for-both-abis)) |
| Link failure: `not able to find OpenSSL` (or missing `libssl`/`libcrypto`). | cpr forces the OpenSSL TLS backend on Android; the static libs for the **target ABI** are missing. | Build static OpenSSL **per ABI before** the app, laid out as `<base>/{x86_64,arm64-v8a}`, and hand the base to the build via `SMATCHET_ANDROID_OPENSSL_BASE` / `-PsmatchetOpensslBase`. ([§3.7](#37-build-static-openssl-for-both-abis)) |
| OpenSSL built but still not found. | `OPENSSL_ROOT_DIR` unset and the `<base>/${ANDROID_ABI}/lib/libssl.a` derived path doesn't exist (wrong layout, or wrong `--libdir`). | Ensure the per-ABI subdir name is exactly `x86_64` / `arm64-v8a` and that OpenSSL was configured with `--libdir=lib` (some hosts default to `lib64`). `smatchet_prepare_cpr()` derives `<base>/${ANDROID_ABI}` and looks for `lib/libssl.a`; if missing it only warns and falls back to probing. |
| `There are no .so files available to package in the APK for arm64-v8a`. | Harmless. You used `-Pandroid.injected.build.abi=x86_64` but `abiFilters` still lists both ABIs. | Ignore for `x86_64`-only inner-loop builds. Build both ABIs for a release APK. ([§4](#4-building-the-apk)) |
| `sdkmanager`/`avdmanager` not found, or "Could not determine SDK root". | `cmdline-tools` not in the `latest/` layout, or SDK tool dirs not on `PATH`/`ANDROID_SDK_ROOT` unset. | Ensure the tools sit at `<sdk>/cmdline-tools/latest/bin` (the unzip-then-`mv-to-latest` step) and that `ANDROID_SDK_ROOT` + the PATH entries from §3.0 are exported. ([§3.3](#33-bootstrap-the-android-sdk--components)) |
| Emulator extremely slow / `emulator -accel-check` fails. | No hardware acceleration. | Enable **WHPX** (Windows Hypervisor Platform) or Intel HAXM. ([§6](#6-run-on-emulator--device)) |
| **Android debug-only** link error: `undefined symbol: Foo::kBar` for a class-type `static constexpr` member. | C++14 ODR: a class-type `static constexpr` member ODR-used by reference needs an out-of-line definition. MSVC and optimised Clang fold the constant and never emit the symbol, but a `-O0` Android **debug** build *does* emit the reference. | Convert the member to a `constexpr` member function (implicitly inline, needs no out-of-line definition) — exactly how `MainThreadDispatcher::kDrainBudget` was fixed. (Already resolved in-tree; included only as a diagnosis aid if you add a new such member.) |

---

## 8. CI reference

**Workflow:** [`.github/workflows/build-and-test.yml`](../../.github/workflows/build-and-test.yml).

### Existing job — `mobile-android-ndk` (blocking)

- **Name:** `Mobile — Android NDK arm64-v8a (.so configure+link)`
- **Runner:** `ubuntu-latest` · **timeout:** 45 min · **permissions:** `contents: read`
- **Gating:** `needs: changes` and runs only when `needs.changes.outputs.code == 'true'`. The change-detector treats `*.md`, `docs/*`, `agents/*`, etc. as docs-class (no build); a `Source/Mobile/**` change hits the catch-all and sets `code=true`, so this job runs. (An empty/failed diff defaults `code=true`, fail-safe.)
- **Blocking:** a branch-protection required context on `develop` + poller-blocked under block-on-any-red (see § Mobile CI jobs below).

**Pins (env):**

| Var | Value |
|---|---|
| `OPENSSL_VERSION` | `3.5.6` |
| `OPENSSL_SHA256` | `deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736` |
| `NDK_VERSION` | `26.3.11579264` |
| `ANDROID_API` | `24` |

**What it validates (steps, in order):**
1. Checkout (`actions/checkout@v4`, `persist-credentials: false`).
2. Install OpenSSL prereqs (`make` + `perl`).
3. Install + pin the NDK via `sdkmanager "ndk;26.3.11579264"`; export `ANDROID_NDK_ROOT`, `ANDROID_NDK_HOME`, and `OPENSSL_PREFIX=${RUNNER_TEMP}/openssl-android/arm64-v8a`.
4. Cache prebuilt OpenSSL (`actions/cache@v4`, keyed on OpenSSL/NDK/ABI/API).
5. Build static OpenSSL for `arm64-v8a` with the NDK clang — the same `./Configure android-arm64 -D__ANDROID_API__=24 no-shared no-tests no-apps no-docs --libdir=lib --prefix=…` flow as [§3.7](#37-build-static-openssl-for-both-abis) (cache-miss only).
6. Verify `libssl.a`, `libcrypto.a`, `opensslv.h` present.
7. Cache FetchContent `_deps`.
8. **Configure** (`Slice 1 — TLS backend`):
   ```bash
   cmake --preset android-ndk-arm64 \
     -DOPENSSL_ROOT_DIR="${OPENSSL_PREFIX}" \
     -DOPENSSL_INCLUDE_DIR="${OPENSSL_PREFIX}/include" \
     -DOPENSSL_SSL_LIBRARY="${OPENSSL_PREFIX}/lib/libssl.a" \
     -DOPENSSL_CRYPTO_LIBRARY="${OPENSSL_PREFIX}/lib/libcrypto.a" \
     -DCMAKE_FIND_ROOT_PATH="${OPENSSL_PREFIX}"
   ```
   (CI pins `OPENSSL_ROOT_DIR` explicitly — unlike the local `SMATCHET_ANDROID_OPENSSL_BASE` convenience path. The `${RUNNER_TEMP}` here is the GitHub-runner scratch dir; locally use any writable scratch dir, e.g. `C:/Android/scratch` from [§3.7](#37-build-static-openssl-for-both-abis).)
9. **Compile + link** the `.so` (`Slice 2 — empty .so link`):
   ```bash
   cmake --build --preset android-ndk-arm64 --target SmatchetMobile -- -k 0
   ```
   (`-- -k 0` = Ninja keep-going, to enumerate every failing TU in one run.)

The `android-ndk-arm64` preset: generator Ninja, `binaryDir build/android-ndk-arm64`, toolchain `$env{ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake`, `CMAKE_BUILD_TYPE=RelWithDebInfo`, `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-24`, `SMATCHET_WITH_LUA_AUTOMATION=OFF`, `SMATCHET_WARNINGS_AS_ERRORS=OFF` (plus the inherited `_smatchet-light-features` `MCP/AI/WHISPER=OFF`). The TLS cache vars (`CPR_FORCE_OPENSSL_BACKEND`, `CMAKE_FIND_ROOT_PATH_MODE_*`) live only in `smatchet_prepare_cpr()`, not in the preset (kept DRY).

### Mobile CI jobs (shipped)

Three jobs now cover the Android target — all **blocking** since the all-gates-blocking flip (the NDK + APK jobs are branch-protection required contexts; the emulator smoke is poller-blocked via block-on-any-red, made reliable by the cold-boot fix that removed its snapshot-restore boot race):

- **`mobile-android-ndk`** — configure + link of the `arm64-v8a` `SmatchetMobile` `.so` (validates Slice-1 TLS backend); stops at link.
- **`mobile-android-apk`** — `./gradlew assembleDebug -Pandroid.injected.build.abi=arm64-v8a`; packages the release-ABI APK (`needs: mobile-android-ndk`, restores its OpenSSL/_deps cache).
- **`mobile-emulator-smoke`** (`mobile-emulator-smoke.yml`) — `assembleDebug` for `x86_64`, boots an x86_64 emulator (`reactivecircus/android-emulator-runner`), installs the APK, and asserts the native shell reaches its first-frame marker (`.github/scripts/mobile-emulator-smoke.sh`) — the only job that runs the app, not just builds it.

---

## 9. Architecture notes (orientation)

Just enough to navigate the Android host shell (`Source/Mobile/Android/`); not an API reference.

- **Native entry point:** `android_main(android_app* app)` in `Source/Mobile/Android/android_main.cpp`, hosted by NDK `native_app_glue` (`<android_native_app_glue.h>`) — **not** a custom `ANativeActivity_onCreate`. It wires `app->userData = &state`, `app->onAppCmd = OnAppCmd`, `app->onInputEvent = OnInputEvent`, calls `BootCoreOnce`, then runs the looper loop. App quit is requested via `ANativeActivity_finish(app->activity)`. The linker keeps the glue's entry point alive via `target_link_options(SmatchetMobile PRIVATE -u ANativeActivity_onCreate)`.
- **Sibling of the desktop bootstrap:** mirrors `Source/Standalone/StandaloneAppBootstrap.cpp` — the same engine-agnostic Core stack (`AppController` + `PluginHost` + `SmatchetUI`) but over EGL/GLES3 + `imgui_impl_android` + `imgui_impl_opengl3` instead of GLFW/desktop-GL.
- **Per-frame loop (`RenderOneFrame`):** guard (`hasSurface && imguiReady && coreBooted`) → `ime.ShowKeyboardIfNeeded(io)` → `ime.PollUnicodeChars(io)` → `mainThreadDispatcher.Drain()` → `Scenarios().Tick()` → `ImGui_ImplOpenGL3_NewFrame` → `ImGui_ImplAndroid_NewFrame` → `ImGui::NewFrame` → `DockSpaceOverViewport` → `mainWindow->Draw(app)` → `pluginHost->OnDraw(app)` → `ImGui::Render` → `glViewport`/`glClear` → `ImGui_ImplOpenGL3_RenderDrawData` → `egl.SwapBuffers`.
- **Looper / scheduling:** `ALooper_pollOnce` with timeout `hasSurface ? 0` (spin, render every loop) `: -1` (block when backgrounded so an idle app burns no CPU). `destroyRequested` → `TeardownAll` and return.
- **Core boot timing:** `BootCoreOnce` runs at `android_main` start, window-independent (Core + sqlite + tracker live before the first frame). ImGui + the EGL surface are created later on the first `APP_CMD_INIT_WINDOW` (`InitImGuiFirstTime`).
- **EGL:** owned entirely by `SmatchetAndroidEgl` (`SmatchetAndroidEgl.h` / `SmatchetAndroidEgl.cpp`) — a self-contained RAII class with no ImGui/Core includes (keeps GLES headers out of Core TUs). It owns `EGLDisplay`/`EGLContext` for process lifetime + one `EGLSurface` bound to the current `ANativeWindow`; picks a GLES3 RGB8+D24 config. On `APP_CMD_TERM_WINDOW`: `hasSurface=false`, `DestroySurface()` (context + GL objects + ImGui context survive). On `APP_CMD_INIT_WINDOW` re-entry: `CreateSurface()` rebuilds the surface, then `ImGui_ImplAndroid_Init(app->window)` re-points the backend (no full ImGui reinit).
- **JNI IME bridge** (`SmatchetAndroidImeBridge`): `imgui_impl_android` only emits raw key events and cannot raise the soft keyboard or produce characters (imgui #3446), so the host owns text input. The **Java** `SmatchetActivity` (extends `android.app.NativeActivity`) exposes **3 methods**, resolved by `GetMethodID` on the activity class and driven from the render thread, debounced on `io.WantTextInput` edges to keep JNI off the 6.94 ms frame budget:

  | Method | JNI signature | Behaviour |
  |---|---|---|
  | `showSoftInput` | `()V` | Raise keyboard (called on `WantTextInput` rising edge). |
  | `hideSoftInput` | `()V` | Lower keyboard (falling edge). |
  | `pollUnicodeChar` | `()I` | Drain one queued Unicode char; returns `0` (≤ 0) when the IME queue is empty. |

  Chars are produced by `dispatchKeyEvent` offering `event.getUnicodeChar(metaState)` into a `LinkedBlockingQueue<Integer>(256)` (non-blocking `offer()` drops on overflow). `PollUnicodeChars` drains into `io.AddInputCharacter`, bounded to 64 iterations/frame. If any `jmethodID` is null, the bridge clears the pending JNI exception and degrades to no-op (keys still work, no keyboard/chars). **These signatures must stay exactly in sync between the Java activity and the native bridge.**
- **Host-injection seams** (numbered in `android_main.cpp`):
  - **#12 Font (`SmatchetSetInjectedFontBytes`):** `ReadApkAssetFont(assetManager, "fonts/Roboto-Medium.ttf")` reads the TTF from the APK into a process-lifetime blob; if non-empty it is injected **before** the first atlas build at `fontPx = 16.0 * densityScale`, then `SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(io)`. Empty blob → Core's default font.
  - **#13 Density (`SmatchetTheme::ApplyUiDensityScale`):** `densityScale = AConfiguration_getDensity()/160` (160 dpi = mdpi = 1.0; null/zero → 1.0). Applied **after** `StyleColorsDark` (so it isn't wiped); `ReapplyHostDensityScale` persists it across later `ApplyStyle` calls.
  - **#14 Data dir + DB path (`ConfigManager::SetPlatformSharedUserDataDirectoryOverride`):** `dataDir = NormalizeDir(activity->internalDataPath)` (private dir, always ends `/`). Android cwd is `/` (read-only) so all writable paths must be absolute. Sets the shared/user/runtime-asset directories to `dataDir`; the SQLite `cfg.DbPath` is prefixed with `dataDir` if empty or not absolute. Keeps `imgui.ini` + config + sqlite db inside the private dir.
- **Platform logging:** the host shell logs to logcat via `__android_log_print` (`SLOG`/`SLOGE`, tag `"Smatchet"`). The macros are declared in `SmatchetAndroidPlatform.h`; `SmatchetAndroidPlatform.cpp` is the TU that includes the Android asset-manager/configuration headers (`<android/asset_manager.h>` / `<android/configuration.h>`). Core code keeps using `LOG_*` from `Logger.h`.

---

## 10. Appendix — file map & committed/gitignored

### `Source/Mobile/Android/` (native host shell, compiled into `libSmatchetMobile.so`)

| File | Role |
|---|---|
| `android_main.cpp` | Native entry `android_main`; native_app_glue wiring, `BootCoreOnce`, looper loop, `RenderOneFrame`, the #12/#13/#14 host-injection seams. |
| `SmatchetAndroidEgl.h` | Self-contained RAII EGL/GLES3 surface+context owner interface (no ImGui/Core includes). |
| `SmatchetAndroidEgl.cpp` | EGL/GLES3 display+context+surface implementation (config pick, create/destroy/swap). |
| `SmatchetAndroidImeBridge.h` | JNI IME bridge interface (soft keyboard + Unicode input). |
| `SmatchetAndroidImeBridge.cpp` | Resolves the 3 activity methods (`showSoftInput`/`hideSoftInput`/`pollUnicodeChar`), edge-debounced show/hide, queue drain. |
| `SmatchetAndroidPlatform.h` | Platform logging macros (`SLOG`/`SLOGE` over `__android_log_print`). |
| `SmatchetAndroidPlatform.cpp` | Platform impl TU; the one host TU including the Android asset-manager / configuration headers. |

(The CMake `SmatchetMobile` target globs `Source/Mobile/Android/*.cpp` plus the shared `CORE_SOURCES`.)

### `Source/Mobile/AndroidApp/` (the Gradle project)

| File | Role |
|---|---|
| `settings.gradle` | `rootProject.name = SmatchetMobile`; `include ':app'`; plugin/dependency repositories. |
| `build.gradle` | Declares AGP `8.5.2` (`apply false`). |
| `gradle.properties` | `org.gradle.jvmargs=-Xmx2048m …`; `android.useAndroidX=true`; `android.nonTransitiveRClass=true`. |
| `app/build.gradle` | The app module: namespace/applicationId `com.smatchet.mobile`, compile/target SDK 34, minSdk 24, `ndkVersion 26.3.11579264`, `abiFilters [arm64-v8a, x86_64]`, `externalNativeBuild` → repo-root `CMakeLists.txt`, CMake `'3.24.0+'`, target `SmatchetMobile`, the light-feature `-D` flags, and the OpenSSL-base handoff. |
| `gradle/wrapper/gradle-wrapper.properties` | `distributionUrl = …/gradle-8.7-bin.zip`. |
| `app/src/main/AndroidManifest.xml` | `INTERNET` permission; `.SmatchetActivity` (`NativeActivity`, exported, `MAIN`/`LAUNCHER`); `android.app.lib_name = SmatchetMobile`. |
| `app/src/main/java/com/smatchet/mobile/SmatchetActivity.java` | The **Java** activity (extends `NativeActivity`); the 3 JNI IME methods + the `LinkedBlockingQueue` input path. |
| `local.properties` | **Gitignored, machine-local:** `sdk.dir` + `cmake.dir`. |
| `.gitignore` | Excludes `.gradle/`, `build/`, `app/build/`, `app/.cxx/`, `.cxx/`, `local.properties`, `*.iml`, `.idea/`. |

### Committed vs. gitignored

| Committed (in VCS) | Gitignored (machine-local / generated) |
|---|---|
| `gradle/wrapper/gradle-wrapper.jar`, `gradlew`, `gradlew.bat`, `gradle-wrapper.properties` (CI runs `./gradlew`) | `local.properties` (`sdk.dir` differs per machine; `ANDROID_HOME` env preferred) |
| All `*.gradle`, manifest, Java/native sources | `.gradle/`, `build/`, `app/build/`, `app/.cxx/`, `.cxx/`, `*.iml`, `.idea/` |
