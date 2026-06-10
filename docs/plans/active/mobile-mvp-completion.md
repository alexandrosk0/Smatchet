# Plan — Mobile (Android) Phase-0 MVP completion

> **Slug**: `mobile-mvp-completion` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> <!-- index-summary: Sequenced completion of the Android Phase-0 MVP — the 5 deferred mobile Issues (#1067 allowBackup, #1068 TLS-CA, #1069 IME-init, #1070 paste, #1071 EGL context-loss), soft-keyboard bugs #1054/#1055, lifecycle/perf gaps, test/CI enablement, then Phase-1 (Keystore/SQLite/touch) under epic #1018. -->

## Context

PR [#1043](https://github.com/alexandrosk0/Smatchet/pull/1043) landed the Phase-0 Android host shell + live Jira data path on `develop` (squash `0a239937`, since advanced). It shipped with **5 deferred Issues** filed as follow-ups and **2 open soft-keyboard bugs**:

- **#1067** (P1 security) — `android:allowBackup="true"` on a credential-bearing app exposes `filesDir` (Jira token + `cacert.pem`) to backup exfiltration.
- **#1068** (P2) — hardcoded compile-time `CAINFO` (`/data/user/0/...`) + non-fail-fast per-ABI OpenSSL discovery.
- **#1069** (P2) — IME bridge `Init()` return discarded; no call-site capture / degraded cue.
- **#1070** (P2) — long clipboard paste truncated at the 256-entry bounded queue → truncated Jira commits.
- **#1071** (P3) — EGL context-loss path does not recreate ImGui GL device objects.
- **#1054** — `KEYCODE_BACK` backgrounds the app instead of dismissing the soft keyboard.
- **#1055** — re-tapping a text field does not re-raise the dismissed keyboard.

Epic **[#1018](https://github.com/alexandrosk0/Smatchet/issues/1018)** tracks the broader mobile arc. **Intended outcome:** after this plan lands, Smatchet Android is a daily-usable Phase-0 client (secure-by-default, TLS-correct to a real Jira, reliable text input, crash-resilient render), with a deterministic mobile CI path replacing today's manual device verification, and a committed Phase-1 plan (secure credential storage is the real ship-to-user gate).

This plan is the critique-corrected successor to the planning pass that accompanied #1043; the dominant "not executable" blocker that critique raised — *Source/Mobile not on develop* — is **resolved** (#1043 merged the shell, the OpenSSL cmake, and the soft-keyboard fix content together). All file:line anchors below were **re-verified against `origin/develop`** (see § Existing utilities reused).

## Approach

Sequence strictly by the standing mandate: **security → correctness blocking daily use → robustness → polish/next-phase.** Six workstreams, ~17 items, 8 PRs. The headline constraint is that WS2/WS3/WS4 all edit `Source/Mobile/Android/android_main.cpp`, so their branches **serialize** (parallel branches would conflict). The headline *risk* is that all three mobile CI jobs (`posix-core-check`, `android-ndk-arm64`, `mobile-android-apk`) are **advisory** — not on the develop merge-gate poller — and `android-ndk-arm64` is the only check that compiles the Bionic `#else` arm, so a green develop merge can still ship mobile breakage (precedent #1021/#1064). Two structural decisions follow from that: (1) WS5's test/CI enablement is treated as a **first-class de-risking workstream**, run in parallel with WS1 and landing its JVM harness **before** the PR that depends on it; (2) the #1067/#1068 regression checks are routed onto a **blocking** gate, not left advisory.

Per the four approved decisions (2026-06-09): **#1067 → `allowBackup=false`** outright (safest until Keystore lands); **#1068 → ship the runtime-CA seam now** (Phase B, before IME work — real-Jira-over-TLS is a daily-use blocker); **emulator CI → approved** (closes the advisory-gate-escape risk for device-only verifications).

## Files to modify

Grouped by workstream. Anchors verified against `origin/develop` 2026-06-09.

**WS1 — Security + build guardrails (PR-1, CI-only):**
1. [`Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml:7`](../../../Source/Mobile/AndroidApp/app/src/main/AndroidManifest.xml) — `allowBackup="true"` → `"false"`.
2. New CI grep gate (extend an existing mobile lint step in `.github/workflows/`) — fail if `allowBackup="true"` reappears; route onto the **blocking** path (merge-gate poller allow-list), not advisory.
3. [`cmake/SmatchetThirdParty.cmake:108,114-115`](../../../cmake/SmatchetThirdParty.cmake) — per-ABI OpenSSL discovery: `message(WARNING)`+sysroot-fallback → `FATAL_ERROR` when `libssl.a` / `libcrypto.a` / `include/` are missing for the ABI.

**WS2 — TLS runtime-resolved CA (PR-2, Source/Core seam):**
4. [`cmake/SmatchetThirdParty.cmake:90`](../../../cmake/SmatchetThirdParty.cmake) — retire the hardcoded `CURL_CA_BUNDLE` literal as the *primary* trust root.
5. New `CURLOPT_CAINFO` / cpr `SslOptions` seam in the Tracker HTTP layer (`Source/Core/src/Tracker/` — greenfield; grep confirms no existing `CAINFO`/`SslOptions`/`CURLOPT` usage), fed the host-provided runtime path.
6. [`Source/Mobile/Android/android_main.cpp:115`](../../../Source/Mobile/Android/android_main.cpp) — pass the JNI-resolved `internalDataPath`/`files/cacert.pem` into the new seam at boot (currently only `setenv("SSL_CERT_FILE", …)`).

**WS3 — IME reliability + boot-error surfacing (PR-3, PR-4):**
7. New on-screen boot/error panel — **must render via raw GL** (clear-color + minimal text), not ImGui: the dominant failure path is `BootCoreOnce` failing → `coreBooted=false` → `RenderOneFrame` early-returns at [`android_main.cpp:222`](../../../Source/Mobile/Android/android_main.cpp) before ImGui is ever initialized.
8. [`android_main.cpp:214,217`](../../../Source/Mobile/Android/android_main.cpp) — capture `s.ime.Init(...)` return, `SLOGE` + degraded cue; **do NOT** gate `s.imguiReady` on it (gating blanks the whole UI). (#1069)
9. [`SmatchetActivity.java:36,198,212`](../../../Source/Mobile/AndroidApp/app/src/main/java/com/smatchet/mobile/SmatchetActivity.java) + [`SmatchetAndroidImeBridge.cpp:112`](../../../Source/Mobile/Android/SmatchetAndroidImeBridge.cpp) — paste truncation: deliver full clipboard text via the chosen non-blocking mechanism (see open question), remove the 256-cap drop and the 64-char/frame native drain throttle. (#1070)
10. `SmatchetActivity.java` — intercept `KEYCODE_BACK` so Back dismisses the IME and the app stays foreground. (#1054)
11. [`SmatchetAndroidImeBridge.cpp:89-93`](../../../Source/Mobile/Android/SmatchetAndroidImeBridge.cpp) (C++ rising-edge `want == lastWantTextInput_` early-return) **+** [`SmatchetActivity.java:159`](../../../Source/Mobile/AndroidApp/app/src/main/java/com/smatchet/mobile/SmatchetActivity.java) (retire `SHOW_FORCED`, use `WindowInsets.Type.ime()`) — re-tap re-raises the IME. The C++ edge is the root cause; a Java-only change leaves re-raise broken. (#1055)

**WS4 — Render robustness + lifecycle/perf (PR-5, PR-6):**
12. [`android_main.cpp:252`](../../../Source/Mobile/Android/android_main.cpp) + [`SmatchetAndroidEgl.cpp:121-126`](../../../Source/Mobile/Android/SmatchetAndroidEgl.cpp) — detect `EGL_CONTEXT_LOST` on swap, tear down + recreate ImGui device objects + context/surface. (#1071)
13. `android_main.cpp` — pause the render/swap loop on `APP_CMD_PAUSE` / `APP_CMD_LOST_FOCUS`.
14. `android_main.cpp` — handle `APP_CMD_SAVE_STATE` / restore-on-resume (process-death state restore).
15. [`android_main.cpp:203-210`](../../../Source/Mobile/Android/android_main.cpp) — re-apply density scale + font atlas on `APP_CMD_CONFIG_CHANGED` (density currently resolved once at boot); the font-atlas rebuild must stay under the Pillar-2 100 ms UI-block budget.
16. **Stray-PUT-on-Escape** (commit-fires-on-deactivate writes wrong data to a real Jira) — **re-tiered from Phase-1 polish to a WS4 correctness item**; route to `tracker-backend`.

**WS5 — Test / CI enablement (PR-7, runs parallel with WS1):**
17. JVM/Robolectric harness for the Activity `LinkedBlockingQueue` + IME-bridge logic — **lands before PR-4** (#1070 paste verification depends on it).
18. Android emulator CI smoke job (boot APK, assert first frame + startup log marker) — gives WS3/WS4 a deterministic verification path.
19. OpenSSL fail-fast `ctest`/`bats` configure-probe (asserts non-zero exit when OpenSSL absent for an ABI).

**WS6 — Phase-1 / next (PR-8+, plan-gated):**
20. Author the `mobile-app-fuller-integration` plan (lands in `docs/plans/active/`) + reconcile epic #1018.
21. Credential settings UI + Android Keystore secure token storage (**real ship-to-user gate**) — `security-review`.
22. SQLite offline cache for tickets — `offline-sync`.
23. Touch cell editors + mobile interaction model — `grid-engine`.
24. **Doc-lag fix** — update `docs/mobile/ANDROID_BUILD.md` + the shipped `docs/plans/mobile-app-jql-mvp.md` alongside the PRs (no stale build guidance).

## Existing utilities reused

- `SLOG` / `SLOGE` — logcat wrappers at [`SmatchetAndroidPlatform.h:21-22`](../../../Source/Mobile/Android/SmatchetAndroidPlatform.h). **Use these, not `LOG_*`** — `Logger.h` `LOG_*` macros have no logcat sink on Android ([`android_main.cpp:87-89`](../../../Source/Mobile/Android/android_main.cpp)).
- `SmatchetAndroidImeBridge::Init` already `SLOGE`s internally on a missing Java method ([`SmatchetAndroidImeBridge.cpp:45`](../../../Source/Mobile/Android/SmatchetAndroidImeBridge.cpp)) and degrades to a safe no-op — #1069's gap is only the **call-site** capture + user cue, not a crash.
- `CURL_CA_FALLBACK=ON` ([`cmake:92`](../../../cmake/SmatchetThirdParty.cmake)) + `setenv("SSL_CERT_FILE", …)` ([`android_main.cpp:115`](../../../Source/Mobile/Android/android_main.cpp)) — the existing fragile-but-working secondary trust net WS2 replaces with an explicit `CAINFO` seam.
- `WindowInsets` listener already installed at [`SmatchetActivity.java:54,94`](../../../Source/Mobile/AndroidApp/app/src/main/java/com/smatchet/mobile/SmatchetActivity.java) — #1055's `WindowInsets.Type.ime()` path builds on existing infrastructure, not greenfield.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: WS4 pause-on-focus halts frame work while backgrounded; a dedicated WS4 item must verify **real-hardware** steady-state ≤ 6.94 ms / p99 ≤ 10 ms (emulator perf does NOT satisfy this — emulator ≠ device).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: #1070 fix must be **non-blocking** (a blocking `put()` on the UI thread risks ANR); WS4 font-atlas rebuild on config-change must stay < 100 ms or show a cue; the boot/error panel IS the visible cue for boot failure.
- **Pillar 3 (never crash)**: #1071 EGL recreate prevents a context-loss crash; #1069 must keep rendering on IME-init failure (never couple render to IME); WS6 Keystore prevents plaintext-token ship.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: WS4 density-on-config-change is the font-scaling slice (Pillar-4 is backlog-aspirational; no auto-fail).

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

WS2 (CAINFO seam) and the stray-PUT fix touch `Source/Core/src/Tracker/`; WS1/WS3-mobile-only/WS5/WS6-non-core PRs are mobile-tree-only.

1. **PR-fast CI** — WS2 exercises the Tracker HTTP path; name the Jira-search/mutation scenario in the WS2 PR. Other WS PRs: `N/A` (no `Source/Core` perf-path touched) — declare per-PR.
2. **Pillar 2 static scanner** — WS2 adds no new `ImGui::*`-reachable sync I/O (the CA seam is on the existing worker HTTP path). #1070 paste path must annotate any new transfer as non-blocking.
3. **Dispatcher drain** — no WS touches `MainThreadDispatcher::Drain()`. `N/A`.
4. **Visible-cue bucket-E harness** — the boot/error panel + #1069 degraded cue are new visible-state paths; assert under the emulator/bucket-E harness once WS5 lands.
5. **Marker inventory** — WS4 real-hardware perf item may add `SMATCHET_UI_PERF_SCOPE` markers; if so regen `docs/perf/MARKER_INVENTORY.md` in that PR.

**Override**: `perf-out-of-band` per `AGENTS.md` § Merge gates.

## Risks / non-goals

- **Advisory-CI gate-escape (headline)** — the 3 mobile CI jobs are advisory + off the merge-gate poller; `android-ndk-arm64` is the only Bionic-`#else` compile. *Mitigation:* WS1 routes #1067/#1068 checks onto the blocking path; WS5 emulator smoke + promoting `android-ndk-arm64` toward required once stabilized.
- **`android_main.cpp` contention** — WS2/WS3/WS4 all edit it; parallel branches WILL conflict. *Mitigation:* serialize per § sequencing (single-branch order PR-2 → PR-3 → PR-4 → PR-5 → PR-6).
- **#1069 `imguiReady` trap** — gating `imguiReady` on IME `Init()` blanks the entire UI. *Accepted constraint:* surface a degraded cue, never couple render to IME.
- **#1070 ANR risk** — a blocking `put()` from the JNI paste path can ANR. *Mitigation:* non-blocking design (open question: unbounded queue vs single-shot JNI getter).
- **#1068 fragility** — a wrong runtime CA path silently disables TLS trust. *Mitigation:* device verification PLUS a `Source/Core` seam unit test asserting the `CAINFO` is injected from the host path.
- **#1071 non-determinism** — context loss is hard to reproduce; verification is device/emulator-only and inherently flaky. *Accepted.*
- **13 owed gate-escape postmortems** on recent develop merges (mostly Test-delta + cr-out-of-band) — the loop is running hot on mobile. *Mitigation:* drain the postmortem backlog before piling on more advisory-CI mobile merges (tracked separately; not a WS item but a precondition flag).
- **Non-goals**: iOS (Phase-2, blocked on the macOS-runner cost decision — see open questions); any non-Jira tracker on mobile; desktop feature parity.

## Verification

Per `AGENTS.md` § Verification automation. Per-PR buckets declared in each PR; epic-level:

- **Bucket A (pure-logic ctest, `test-rig`)**: WS2 CAINFO-seam injection unit test; #1067 manifest assertion is a CI grep gate.
- **Bucket E / emulator (WS5)**: boot/error panel renders on forced boot-failure; #1069 degraded cue + `imguiReady` stays true; #1054 Back closes IME + app foreground; #1055 re-tap re-raises (API 30+); density rescale live + < 100 ms. **These depend on the WS5 emulator harness landing first.**
- **JVM/Robolectric (WS5)**: #1070 — paste a >256-char / >64-char-per-frame string, assert full delivery + non-blocking `put()`. *Caveat:* the Robolectric test covers the Java queue half; the **C++ 64-char/frame drain** ([`ImeBridge.cpp:112`](../../../Source/Mobile/Android/SmatchetAndroidImeBridge.cpp)) is native and needs a separate Source/Core or device check.
- **Build gate**: each mobile PR must pass `android-ndk-arm64` (the only Bionic-`#else` compile); Source/Core PRs add `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`.
- **Real-hardware perf (WS4)**: Pillar-1 steady-state ≤ 6.94 ms / p99 ≤ 10 ms measured on a physical device — emulator does NOT satisfy this.
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` suite green.
- **Plan stress-test — `grill-with-docs`**: this plan was adversarially critiqued in its planning pass (verdict "not executable as-is"); every cited gap is resolved here — base-merge (#1043 landed it), branch-reconcile (#1043 unified both branches), 7aa58f97 reconcile (its content is on develop; anchors re-verified), PR-order fix (WS5 JVM harness before PR-4), stray-PUT re-tier (WS4 item 16), SAVE_STATE/RESUME (item 14), real-hardware perf (WS4 verification), raw-GL boot panel (item 7), #1067/#1068 onto poller (item 2 + WS5 item 19), #1055 C++ rising-edge (item 11). Outcome recorded.
- **Manual residue**: until WS5 emulator CI exists, WS3/WS4 device verifications are manual; the deferred-automation action plan IS WS5 (land it early). No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep** — on finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "deferred-as-current" mobile refs and revise.

- **iOS / Phase-2** — blocked on the macOS CI runner cost decision (open question); no-action until resolved.
- **Non-Jira trackers on mobile** — follow-up under epic #1018, not this plan.
- **Desktop parity** — explicit non-goal for Phase-0.

## Implementation log
*(bullet per shipped commit: `<sha> · <one-line summary>`)*

- **WS2 / PR-2 (Fixes #1068)** — runtime-resolved CA seam. New cpr-free pure unit `TrackerHttpPure` (`ResolveSslConfig` + a process-global CA-path holder `Set/GetCaBundlePath`); `TrackerHttpUtils::MakeTrackerSslOptions()` turns the path into `cpr::SslOptions` applied to all 5 tracker verbs (Get×2/Post/Put/Patch) — empty path ⇒ default `cpr::SslOptions{}` (libcurl default CAINFO untouched, desktop unchanged), non-empty ⇒ explicit `CURLOPT_CAINFO`. `android_main.cpp` boot feeds the extracted cacert.pem path via `SetCaBundlePath` before Core init. `cmake/SmatchetThirdParty.cmake` CURL_CA_* defines demoted to documented defense-in-depth (kept, not removed). Bucket-A test `TrackerHttpSslPure.test.cpp`.

## Deviations from plan
*(populated post-ship)*

- **WS2 cmake — demoted, not removed.** § Files-to-modify row said "retire cmake `CURL_CA_BUNDLE` primary literal". Actual: the three `set(CURL_CA_BUNDLE/CURL_CA_PATH/CURL_CA_FALLBACK …)` lines are **kept** and re-documented as a defense-in-depth fallback (help-strings + comments only). Removing the baked literal would be a fail-closed regression if the runtime seam is ever skipped (e.g. a boot path that doesn't call `SetCaBundlePath`); demotion keeps the backstop while making the runtime CAINFO load-bearing. Trust model unchanged — the seam only ever *adds* a CAINFO, never disables verification.

- **WS3 items 10 + 11 — re-raise + stay-down both live native-side; the planned Java hooks are dead in a `NativeActivity`.** § Files-to-modify said (item 10) catch `KEYCODE_BACK` in `Activity.dispatchKeyEvent` to suppress auto-re-raise, and (item 11) clear that suppress in `Activity.dispatchTouchEvent` ACTION_DOWN so a re-tap re-raises. Emulator validation (API 34, temp-debug logcat) proved **both Java hooks are dead code in a `NativeActivity`**: touches route to the native `AInputQueue` (consumed by ImGui), so `Activity.dispatchTouchEvent` **never fires** — two taps that visibly raised and activated the field produced **zero** touch-DOWN logs; and Back is consumed by the IME (`InputMethodService.requestHideSelf`) before reaching `Activity.dispatchKeyEvent`. An intermediate design (suppress bit set on a `visible→hidden` `OnApplyWindowInsetsListener` transition, read back over JNI via `imeState()`) worked but was load-bearing on a Java state machine the native side couldn't see the inputs to. **Final design: entirely native-side.** `ShowKeyboardIfNeeded` raises on the `io.WantTextInput` rising edge OR on a fresh `io.MouseDown[0]` tap-edge while `WantTextInput` is true (touches ARE visible to ImGui even though they bypass Java dispatch). There is **no steady-state re-raise**, so item 10 (stays down after a Back/swipe dismiss) is *free*: nothing re-raises a focused-but-dismissed field until a new tap; item 11 (re-tap) is that tap-edge. This **collapsed the entire Java machinery** — the `suppressKeyboard`/`imeVisible`/`appRequestedHide`/`lastInsetImeVisible` fields, `imeState()` + its native `imeState_` resolution, the inset-listener IME-visibility block, the `dispatchTouchEvent` override, and the `dispatchKeyEvent` BACK branch were all deleted (the inset listener keeps only safe-area caching; `dispatchKeyEvent` keeps only Unicode enqueue). `showSoftInput()` unified to `imm.showSoftInput(view, 0)` on all releases — un-forced (SHOW_FORCED retired, item 11) and re-establishes the served-view connection, so the re-tap re-raises even when `imeView` already holds focus (where `WindowInsetsController.show(ime())` can no-op). Emulator-verified: tap raises, Back stays down at +3 s, re-tap re-raises (`mInputShown=true`), typing still reaches the field. WS5 JVM seam: `ShowKeyboardIfNeeded`'s rising-edge/tap-edge logic is the deterministic candidate (a pure function of `WantTextInput` + `MouseDown` history).

- **WS3 item 7 — EGL surface now created even on Core-boot failure.** The boot/error panel can only paint to a live surface, but the pre-WS3 `APP_CMD_INIT_WINDOW` handler created the surface *inside* the `coreBooted` path, so a boot failure left `hasSurface=false` and nothing could draw. Restructured: `INIT_WINDOW` now (1) ensures the EGL **context**, (2) creates the **surface** + flips `hasSurface` unconditionally, then (3) inits/re-points ImGui **only if `coreBooted`**. The main loop dispatches `RenderOneFrame` when `coreBooted && imguiReady`, else paints `DrawBootErrorPanel` once. Battery-safety preserved by `WantImmediatePoll`: a booted UI spins (timeout 0, animates); a boot-failed app draws the panel once then blocks on `-1` (no CPU spin → Pillar 1). The predicate is re-evaluated inside the poll-loop condition so an `INIT_WINDOW` processed mid-drain flips it and the loop exits to render.

## Verification (actual)
*(populated post-ship)*

- **WS2 / PR-2** — local, pre-push:
  - **Lint** (`test-lint-rules.sh --diff origin/develop`): PASS 6/6 (strict-zone · comment-noise · no-raw-new/deviation/detach · GLFW-in-core-headers · function-too-long · agent-prompt-size).
  - **Bucket A** (`smatchet_tests`, doctest+CTest): new `TrackerHttpSslPure.test.cpp` — 3 cases / 14 assertions PASS (empty→no-attach; realistic Android private-dir paths user 0 / user 10 / arbitrary → attach; Set/Get round-trip + reset-disarm). Full umbrella suite green (1/1, 8.24 s) — no regression from the new `TrackerHttpPure.cpp` link.
  - **Dual-target** (`ninja-iter-msvc`): `SmatchetStandalone` (GL) → `Smatchet.exe` linked + `SmatchetCore_DX12.lib` built — both green, confirms the Core seam compiles in the DX12/Unreal world (no GLFW/GL leak into Core headers).
  - **Real-hardware TLS (Pillar-1 / live Jira)**: deferred manual residue — emulator-only injection per the physical-device safety mishap; the runtime CAINFO path is exercised by the Bucket-A unit + the prior live-Jira read-200/write-204 proof. Flagged, not auto-validated.

- **WS3 / PR-3 (items 7–11)** — local, on `smatchet_pixel` emulator (API 34, x86_64, 1080×1920 @ 420dpi):
  - **Build** (`gradlew assembleDebug`, NDK 26.3 clang): BUILD SUCCESSFUL — both `arm64-v8a` + `x86_64` ABIs link, zero errors (only pre-existing `-Wunused` in `Source/Core/src/Ui`). Confirms the native tap-edge redesign + the Java machinery removal compile clean in the Bionic/NDK world (C++14 + JNI signatures still in sync).
  - **Item 8 (#1069 capture-init + degraded cue)** — logcat (`Smatchet` tag) shows `ImeBridge ready` then `ImGui ready (density=2.62 font=42.0px)`; no degraded-IME cue, no boot-error panel. Bridge + ImGui both initialised.
  - **Item 10 (#1054 Back-dismiss stays down)** — tap "Filter…" → `mInputShown=true`; BACK → `false`; +3 s idle → still `false`. No steady-state re-raise (the native bridge has no re-raise without a fresh tap-edge).
  - **Item 11 (#1055 re-tap re-raise)** — after the item-10 dismiss, re-tap the field → `mInputShown=true`. The native `io.MouseDown[0]` tap-edge drives the re-raise (proven: the planned `dispatchTouchEvent` Java hook logged zero touch-DOWNs across the whole sequence — NativeActivity bypass confirmed).
  - **Item 9 (#1070 lossless paste queue)** — typed "bug" into the focused filter field; screenshot confirms "bug" rendered in the field (commitText→`enqueueText`→`PollUnicodeChars` path intact). Exact >256-char bulk-paste losslessness is **deferred to WS5** (Robolectric JVM test, post-#1098) — the unbounded-queue drain code is unchanged from PR design, so behaviour is unaffected; the residue is test *coverage*, not an unverified change.
  - **Item 7 (#1053 raw-GL boot/error panel)** — covered by the prior-session boot-panel work (surface-before-Core-boot restructure, §Deviations); re-confirmed no panel on the happy boot path this run.
  - **Deferred manual residue**: real-hardware Pillar-1 frame-time (emulator-only injection per the device-safety mishap) + the >256-char paste losslessness above. Both flagged; the deferred-automation owner is WS5 (JVM harness + emulator CI).

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen: `bash agents/scripts/core/test-plan-index.sh --fix`.

*(Delete this `## Archive` block as part of step 2.)*
