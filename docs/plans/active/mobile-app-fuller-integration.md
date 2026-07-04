# Mobile app — fuller integration (Phase 1)

> **Slug**: `mobile-app-fuller-integration`
> **Status**: `active` — **reconciled 2026-06-20**: the original "no Phase-1 slices started" status was **stale**. P1.0 / P1.1 / P1.4 are shipped, P1.2 / P1.5 partial, **P1.6 ships in this PR** (research doc); **P1.3** (touch editors — spike-first) is the main unstarted slice. Per-slice evidence in § Current state. The `d8ea206c` "Today" analyses below are **historical** (that commit is no longer in `develop`'s history — re-verify per anchor).
> <!-- index-summary: Phase-1 Android app: Keystore-encrypted token, offline-cache replay on device, touch cell editors + explicit-commit interaction model, attachments, multi-backend, a11y research. -->

## Current state (verified 2026-06-20 @ `develop` HEAD)

**This plan was authored at Phase-0 close; its per-slice "Today" analyses are anchored to `d8ea206c`,
which is no longer in `develop`'s history.** A fresh code audit on 2026-06-20 found the original
"no Phase-1 slices started" status materially wrong — **four of the seven slices are wholly or largely
done**. Corrected per-slice status (evidence = current-tree `file:line`):

| Slice | Status | Evidence (current tree) |
|---|---|---|
| **P1.0** Keystore token | ✅ **Shipped** (audit H2 / CR #1357) | `Source/Mobile/Android/SmatchetAndroidSecretBridge.{h,cpp}` (JNI AES-GCM Keystore bridge, fail-closed, StrongBox, Shutdown-vs-Protect race fix); `ConfigManager.cpp:506–544` (`__ANDROID__` `WriteSecretFields` seals every secret, **no plaintext fallback**) + `:971–1007` (`__ANDROID__` `LoadSecretFields` `unsealSecret` + `migrate.LegacyPlaintext` scrub); installed at boot `android_main.cpp:285,379–386`. The migration scrub the plan called "mandatory/missing" exists. |
| **P1.1** Offline replay | ✅ **Shipped** | The replay/sync drive runs every frame in the **shared Core loop** (not a separate Android hook): `SmatchetUI.cpp:470–473` calls `TickOfflineCreates` / `TickOfflineFieldEdits` / `SyncWithBackend`; reachability transitions `AppController_Connectivity.cpp:72–78,118–120`; dead-letter surface `SmatchetOfflineQueueUi.cpp`. Runs on Android because the loop is shared. |
| **P1.2** Saved views + touch switcher | ✅ **Shipped this PR** | The missing piece — a dedicated touch tab-strip quick-switcher in the grid area — now ships: `drawMobileViewQuickSwitcher` (`SmatchetViewsDashboardUi.cpp`) draws a horizontal-scroll strip of saved-view tabs (active highlighted) + a trailing "+" between the app bar and content dock, reserved by `drawMobileShell` (`SmatchetMobileShellUi.cpp`, `kViewSwitcherBaseHeightPx`). Reuses the existing dirty-aware `viewsRequestActivate` / `viewsCreateNewView`; Grid-page-only (off-grid pages reserve no band). Emulator-verified (switch + dirty discard-confirm). Earlier pieces still hold: view commands (`ViewCommands.cpp:151–282`), mobile drawer + modals (`buildMobileViewsCtx` / `drawMobileDrawerViews` / `drawMobileViewsModals`). |
| **P1.3** Touch cell editors | ❌ **Not started** (the spike-first, highest-risk slice) | `kMobileInlineEditBuild=true` on Android (`TicketFieldEditor.cpp:71–73`) + `SingleClickToEditGridCells` default-on exist, but **no long-press detector** (`SmatchetAndroidImeBridge.cpp`) and **no touch Save/Cancel affordance** for the four combo/modal editors. |
| **P1.4** Attachments | ✅ **Shipped** (decode cross-platform; residual ported this PR) | `SmatchetImageTextureCache.cpp:139–157` (`DecodeWithStb`, no `_WIN32` guard) + renderer-agnostic `RegisterUserTexture`; thumbnails enabled globally. **Residual now shipped:** the previously Win32-only bitmap thumbnail decode (`SmatchetAttachmentPreviewUi.cpp`) is cross-platform — WIC (with decode-scale) kept on Windows, a new stb full-decode + pure area-average `DownscaleRgba32` (`Source/Core/include/Ui/RgbaDownscalePure.h`, unit-tested `tests/Core/RgbaDownscalePure.test.cpp`) on non-Windows, bounded by a 32 MiB file read + 16 MP pre-decode cap, downscaling to ≤2048 px. Same thumbnail UX on every platform. |
| **P1.5** Multi-backend | ✅ **Shipped this PR** | `ITrackerBackend` abstraction + Jira / Plane / GitHub / Linear all in Core; the missing piece — a touch-first backend picker — now ships: `DrawTrackerBackendSelection` (`SmatchetPreferencesUi.cpp:234`) forks on `d.effectiveUiMode == EffectiveUiMode::Mobile` to render the four backends as full-width `ImGui::Selectable` rows (the drawer page-list touch idiom) instead of the tiny desktop `ImGui::Combo`, writing the **same** `d.trackerTypeBuf` so `DrawTrackerBackendConfig` + the multi-backend layer are inherited unchanged (a widget swap, not new backend code). |
| **P1.6** Accessibility | ✅ **Research shipped 2026-06-20**; **accent-contrast fix shipped**; **fontScale seam shipped this PR** | Research: [`docs/mobile/PHASE1_ACCESSIBILITY_RESEARCH.md`](../../mobile/PHASE1_ACCESSIBILITY_RESEARCH.md) + Pillar-4 backlog [`debt/2026-06-20-mobile-accessibility-pillar4.md`](../../self-improvement/categories/debt/2026-06-20-mobile-accessibility-pillar4.md). **Accent fix:** `SmatchetTheme.cpp::ApplySmatchetDark` accent darkened `(0.35,0.55,0.95)`→`(0.26,0.42,0.72)` (Finding 3, white-on-fill 2.90→4.67:1 AA, on-dark 3.16:1 UI-floor); doctest pin `tests/Core/SmatchetThemeAccentContrast.test.cpp`. **ModernDark follow-up (stacked PR):** `ApplyModernDark` accent darkened `(0.45,0.65,0.95)`→its own `(0.29,0.42,0.62)` — test 4 cases / 43 assertions (all 14 re-tinted slots pinned RGBA). **fontScale seam (P1.6b, this PR):** OS accessibility "Font size" (`Configuration.fontScale`) now scales mobile text — JNI reader `SmatchetActivity.getDisplayFontScale()` → `android_main.cpp` `QueryDisplayFontScale` → composed into the **font-atlas px only** via `SmatchetTheme::ComposeFontDensityScale` (header-only, `[0.85,2.0]` clamped); `ApplyUiDensityScale`/`HostDensityScale()` stay raw-DPI (Auto-mode safe). Doctest `tests/Core/SmatchetThemeDensity.test.cpp` (6 cases). Metrics-scaling = Scope-2 follow-up (Pillar-4 debt). |

**Residual P1.0 finding — ✅ SHIPPED 2026-06-20:** the Android plaintext-token warning at
`SmatchetPreferencesUi.cpp` (now line 275) was guarded `#if !defined(_WIN32)`, so it still told
**Android** users "the API token is stored unencrypted" even though P1.0 Keystore-seals it. Fixed by
tightening the guard to `#if !defined(_WIN32) && !defined(__ANDROID__)` (warning kept for desktop
Linux/macOS, which genuinely is plaintext; dropped on Android). Verified on an x86_64 API-34 emulator —
Settings ▸ Tracker renders with no warning banner; desktop dual-target + Android x86_64 `.so` both build
clean. The Windows TU is byte-identical (block was already excluded under `_WIN32`).

**Validation blocker — LIFTED 2026-06-20.** The earlier "no Android NDK/SDK/adb/emulator" blocker
referred to the *cloud authoring* environment. Work now runs on a local Windows box with the full Android
toolchain (NDK 26.3, SDK, `adb`, an x86_64 API-34 AVD `smatchet_pixel`) **and** a working desktop MSVC
dual-target build, so every remaining slice is now buildable + emulator-verifiable here. P1.3 remains
*spike-first* + under the visual-validation exception. Execution order: P1.0 warning fix (✅ done) →
P1.3 spike → P1.2 / P1.5 → P1.4 residual → P1.6 follow-ups.

## Context

Phase 0 (epic [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018)) shipped a runnable Android build of Smatchet: triple-target build infra, EGL/GLES host, soft-keyboard IME bridge, density-aware theme, live JQL fetch + single-field inline edit with a **mobile explicit-commit** policy, and an emulator boot+first-frame smoke CI job. The Phase-0 close-out is [`mobile-app-jql-mvp.md`](../shipped/mobile-app-jql-mvp.md) (the JQL-MVP) + [`mobile-mvp-completion.md`](../shipped/mobile-mvp-completion.md) (WS1–WS6).

Phase 1 turns that MVP into a *fuller* mobile client — the seven slices the Phase-0 §Phase-1-skeleton deferred. Three of them carry deferred [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018) items: **item 21** (secure token storage — Phase-0 ships the token in *plaintext* on Android), **item 22** (offline cache replay on device), **item 23** (the explicit long-press / commit-button affordance PR-6 deferred — Phase-0 only commit-gates the single-line text editor).

This is a Phase-1 *roadmap* plan, not a single-PR execution plan. Each slice (P1.0–P1.6) ships as its own PR, batched per the AGENTS.md one-PR-per-feature rule, routed to its owning specialist. Anchors below are file:line on `origin/develop` at Phase-0 close (commit `d8ea206c`); re-verify before each slice's execution plan.

## Approach

### P1.0 — Android Keystore secure token *(owner: security-review · [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018) item 21 · ship-to-user gate)*

**Today (verified at `d8ea206c`).** `ProtectSecretForConfig()` / `UnprotectSecretFromConfig()` in [`ConfigManager_PathUtils.cpp`](../../../Source/Core/src/Config/ConfigManager_PathUtils.cpp) use Win32 DPAPI (`CryptProtectData`/`CryptUnprotectData`, lines 285/303) with a plaintext passthrough on non-Win32 (lines 332/333). **But on Android those helpers are never reached for the token**: `WriteSecretFields()` and `LoadSecretFields()` (`ConfigManager.cpp`) are split by `#if defined(_WIN32)` — only the Win32 arm calls `Protect*`/`Unprotect*` (`ConfigManager.cpp:411` / `:787`, writing/reading `token_enc`). The **non-Win32 `#else` arm writes the token as raw plaintext**: `WriteSecretFields` erases `token_enc` and sets `j["token"] = config.ApiToken` (`ConfigManager.cpp:465`/`:473`); `LoadSecretFields` reads `cfg.ApiToken = j.value("token", …)` (`ConfigManager.cpp:838`). So on Android the token persists in **plaintext under the `token` key** today, and `token_enc` is explicitly erased. The amber warning at `SmatchetPreferencesUi.cpp:257` (under the `#if !defined(_WIN32)` guard at line 251) is therefore accurate.

**Approach.** Add an **encrypt/decrypt callback-pair host-injection seam** — same shape as the data-dir override (`ConfigManager::SetPlatformSharedUserDataDirectoryOverride`, decl `Source/Core/include/Config/ConfigManager.h:654`, def `ConfigManager_PathUtils.cpp:415`) and the font-bytes seam (`SmatchetSetInjectedFontBytes`, `SmatchetImGuiFonts.h:19`). Core gains `ConfigManager::SetSecretCryptoOverride(encryptFn, decryptFn)`. **Critically, wiring the crypto pair into `ProtectSecretForConfig` alone is a no-op on Android** — that helper is unreached on non-Win32 (verified above). The seam must **rewire the `#else` arms of `WriteSecretFields` (`ConfigManager.cpp:464–488`) and `LoadSecretFields` (`:837–849`)**: when an override is set, route `ApiToken` (at minimum) through the injected pair and emit/read `token_enc` instead of the plaintext `token` key; when no override is set, keep today's plaintext fallback (desktop-Linux dev builds unaffected). Note the load path's wrapper `UnprotectSecretFieldFromConfig` (`ConfigManager.cpp:787`) is itself **Win32-only** (no `#else` def — it sits inside the `#if _WIN32` block of `ConfigManager_PathUtils.cpp:324–330`), so the rewired `#else` load arm must either call the base `UnprotectSecretFromConfig` directly or gain an `#else` passthrough definition (else it won't link on Android). The Android host injects a JNI bridge to **Android Keystore** (hardware-backed AES-GCM via `KeyStore`/`Cipher`, key alias `smatchet.token.v1`) from `BootCoreOnce()` in `android_main.cpp` (alongside the data-dir override injected at line 319; the font/density injections at lines 450/453 live in `InitImGuiFirstTime()`, a separate function — inject the crypto pair in `BootCoreOnce()` before the first config load). When the override is present, flip the `SmatchetPreferencesUi.cpp:257` warning to "encrypted via the Android Keystore".

**Migration / scrub (mandatory).** A Phase-0 install already wrote the token in plaintext under the `token` key. On first run after P1.0 lands, the `#else` load path must read any legacy plaintext `token`, re-persist it encrypted (`token_enc`), and **erase the plaintext key from disk** — mirroring the Win32 `SecretMigrationFlags` pattern (the Win32 arm sets `migrate.*` at `ConfigManager.cpp:810`/`:815` etc.; the `#else` arm currently sets none). Without this, the residual plaintext token survives on any upgraded device.

**Why a callback pair, not `#ifdef __ANDROID__` crypto in Core**: keeps JNI/Keystore out of `Source/Core/` (dual-target purity — Core must compile for DX12/Unreal with no Android headers), mirrors the established host-injection pattern, and keeps the seam unit-testable on desktop with a stub crypto pair.

### P1.1 — Offline cache replay on device *(owner: offline-sync · [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018) item 22)*

**Today (verified).** The SQLite cache is **already active on Android** — `LocalCacheManager` (`LocalCacheManager.h:31`; ctor `LocalCacheManager.cpp:130` creates `tickets_v2` / `pending_creates` / `pending_field_edits`) is opened *unconditionally* at `AppController.cpp:1656` with the resolved `dbPath`; the Phase-0 #14 dataDir prefix routes `cfg.DbPath` to filesDir (`android_main.cpp:369–371`). The DB opens and caches on device today. No platform `#if` gates cache init.

**What Phase-1 adds.** The replay/sync *drive*. `OfflineQueueService::TickOfflineCreates()` (`OfflineQueueService.h:138`) / `TickOfflineFieldEdits()` (h:142) and `TicketSyncService::SyncWithBackend()` (`TicketSyncService.h:53`) are owned by AppController (`AppController.cpp:1697` `offlineQueue_`, `:1703` per-context `ticketSync_`) via `GridContextDepsAdapter` (`GridContextDepsAdapter.h:44`). Phase-1: (a) confirm/wire those ticks into the **Android frame loop** (desktop drives them off a timer/frame hook — the mobile run-loop must call the same ticks); (b) **network-reachability** transition on Android (offline→online flips replay on); (c) **dead-letter** surface for replay failures; (d) **on-device proof** of pending-create / pending-field-edit replay after an offline edit, with `BackendAuditTrail` (`AppendBegin/Result/Event`, `BackendAuditTrail.h:27–32`) entries. The replay tick must run off the UI thread or inside the frame budget (Pillar 2).

### P1.2 — Multiple saved views + touch switcher *(owner: grid-engine / command-system)*

ViewDefinition storage is already per-backend (desktop multi-view works). Phase-1 adds a **touch view-switcher** chrome (tab strip / bottom-sheet) and ensures save/rename/delete-view commands are reachable without a desktop menu bar. Mostly UI chrome over an existing model.

### P1.3 — Touch cell editors + mobile interaction model *(owner: grid-engine · [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018) item 23 — the PR-6-deferred explicit-commit affordance)*

**Today (verified at `d8ea206c`; re-verify per anchor before execution).** The grid (`Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`) assumes desktop input: hover-reveal (370, 1270), double-click-to-open (`IsMouseDoubleClicked(0)`, 1166), selection-rect hit (1294–1295, 1527), keyboard shortcuts Ctrl+C/Esc/Shift+Space (1534/1537/1543). The cell-render flow crosses **out of** the grid file into the field-editor TU: cell loop → `TicketFieldEditor::RenderFieldCell` (call site grid:1252; def in `Source/Core/src/TicketFieldEditor.cpp`) → `DispatchEditorByPlan` (decl `TicketFieldEditor.h:33`, call `TicketFieldEditor.cpp:1351`, def `:1359`) → per-RenderPlan editor; edits open via `state.StartEditingField()` (call sites `TicketFieldEditor.cpp:543` + `TrackerDateTimeFieldEditor.cpp:422`; decl `SpreadsheetState.h:97`), gated by the `singleClickToEdit` parameter each editor takes from `d.cfg.SingleClickToEditGridCells` (grid:1255). PR-6 added `kMobileInlineEditBuild` (`TicketFieldEditor.cpp:68–72`) + `ShouldCommitInlineFieldEdit` (`Source/Core/include/TicketFieldEditorCommitPolicyPure.h:17`, call site `TicketFieldEditor.cpp:434`) — but **only inside `RenderTextInlineEdit` (398–440)**, the single-line `InputText` editor whose `IsItemDeactivatedAfterEdit` focus-loss signal the policy gates.

**The other four editors do *not* commit on focus-loss / click-away** (verified): MultiSelect (commit `TicketFieldEditor.cpp:843`) and Cascading (`:887`) commit via `QueueEdit` on an explicit in-combo `Selectable` click — the popup auto-disarms with no PUT on tap-away; Labels commits via explicit Add/select in `DrawLabelsComboBody` (`Source/Core/src/Tracker/TrackerLabelsEditor.cpp:177`); DateTime is a `BeginPopupModal` with explicit Apply/Clear/Cancel + Escape (`Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:449–461`). None produce the `deactivatedAfterEdit` signal `ShouldCommitInlineFieldEdit` is shaped for, so PR-6's stray-PUT-on-focus-loss bug **does not exist** for these four.

**Approach.** (a) **Not** "route the four through `ShouldCommitInlineFieldEdit`" (a no-op/misfit — they have no focus-loss commit). The real mobile risk is **touch dismissal / tap-away semantics** of the non-modal `BeginCombo` popups (`TicketFieldEditor.cpp:843`/`:887`, `TrackerLabelsEditor.cpp:176`) and **modal sizing/fit** for the DateTime `BeginPopupModal` on a phone screen — design the touch Save/Cancel affordance around the existing **arm-then-popup** model so a tap-away discards with no PUT (the same broad "no implicit commit on mobile" contract PR-6 set for text). (b) Add the **deferred long-press-to-open + explicit Save/Cancel affordance** ([#1018](https://github.com/alexandrosk0/Smatchet/issues/1018) item 23): single-tap opens a full-width touch popup, **Save** commits (PUT), **Cancel / Back / tap-away discards with no PUT**. (c) Touch has no double-click — for cell edit-open this mostly means **defaulting `SingleClickToEditGridCells` on for the mobile build** (the plumbing already exists at grid:1255); reserve new code for the genuinely-deferred double-click-open at grid:1166. (d) Hook a **long-press detector** into the Android input path — `OnInputEvent` (`android_main.cpp:729`) → `ImGui_ImplAndroid_HandleInputEvent`, measuring hold duration on the `io.MouseDown[0]` rising edge the IME bridge already reads (`SmatchetAndroidImeBridge.cpp:94`). **Spike first**: long-press-vs-tap popup model (one slice's worth of interaction-model risk).

### P1.4 — Attachments via stb_image *(owner: tracker-backend / ui-host)*

**Premise corrected (verified at `d8ea206c`).** The image decode is **already cross-platform** — `SmatchetImageTextureCache.cpp` decodes with `stbi_load_from_memory` (`DecodeWithStb`, line 141, no `_WIN32` guard) and uploads via ImGui's renderer-agnostic `ImTextureData` / `RegisterUserTexture` path (`CreateTextureFromRgba`, lines 70–99), which works on GLES3. `AppController_Attachments.cpp` is the network *downloader* (the auth-header consumer at line 133), **not** the decoder. So "mobile has no decoder" is false — P1.4 is largely already done in Core. The only Win32-gated piece is the *optional* bitmap thumbnail behind `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` (`SmatchetAttachmentPreviewUi.cpp:112`). **Re-scoped slice**: verify the existing `SmatchetImageTextureCache.cpp` path renders an attachment thumbnail on a real Android device; only if the optional bitmap thumbnail is in scope, port the `SmatchetAttachmentPreviewUi.cpp:112` Win32-gated piece. No new dependency, likely no new decode code.

### P1.5 — Multi-backend (Plane / GitHub) *(owner: tracker-backend)*

`ITrackerBackend` already abstracts Jira vs Plane; mobile inherits multi-backend for free once the **backend-selection UI is touch-ready** (depends on P1.3's touch chrome). Mostly a verification + touch-UI slice, little new backend code.

### P1.6 — Accessibility *(research-only · Pillar 4 backlogged)*

Research slice: Android **TalkBack** / screen-reader exposure for an immediate-mode ImGui surface (the hard part — IMGUI has no native a11y tree), font scaling (the `ApplyUiDensityScale` seam, `SmatchetTheme.h:66`, already exists), WCAG AA contrast audit of the mobile palette. Output = a findings doc + a Pillar-4 backlog entry, not shipped code.

## Files to modify *(per slice — indicative, hardened at each slice's execution plan)*

| # | Slice | Primary files |
|---|---|---|
| P1.0 | Keystore | `Source/Core/src/Config/ConfigManager.cpp` (rewire the `#else` arms of `WriteSecretFields`/`LoadSecretFields` + the plaintext-`token` migration scrub), `Source/Core/src/Config/ConfigManager_PathUtils.cpp` (+ `Source/Core/include/Config/ConfigManager.h` — the `SetSecretCryptoOverride` decl + an `#else` `UnprotectSecretFieldFromConfig` passthrough if the load arm uses the wrapper), `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:251–257`, `Source/Mobile/Android/android_main.cpp` (+ a new JNI Keystore bridge TU), `Source/Mobile/Android/app/.../*.java` (Keystore) |
| P1.1 | Offline replay | `Source/Mobile/Android/android_main.cpp` (frame-loop tick), `Source/Core/src/Sync/OfflineQueueService.*`, `TicketSyncService.*`, a dead-letter UI surface |
| P1.2 | Saved views | `Source/Core/src/Ui/Smatchet*Ui*.cpp` (touch switcher), `Source/Core/src/Commands/ViewCommands.cpp` |
| P1.3 | Touch editors | `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp`, `Source/Core/src/TicketFieldEditor.cpp`, `Source/Core/src/Tracker/TrackerLabelsEditor.cpp`, `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp`, `Source/Core/include/TicketFieldEditorCommitPolicyPure.h`, `Source/Mobile/Android/android_main.cpp` (long-press), `SmatchetAndroidImeBridge.cpp` |
| P1.4 | Attachments | ✅ shipped: `Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp` (WIC kept `#if _WIN32`, new stb decode `#else` arm, `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` now unconditional), NEW `Source/Core/include/Ui/RgbaDownscalePure.h` (pure area-average downscale) + NEW `tests/Core/RgbaDownscalePure.test.cpp` + `tests/CMakeLists.txt` registration. Underlying cross-platform stb path `Source/Core/src/Persistence/SmatchetImageTextureCache.cpp` already shipped. |
| P1.5 | Multi-backend | touch backend-selection UI (depends P1.3) |
| P1.6 | a11y | research doc + Pillar-4 backlog entry only |

## Existing utilities reused

- **Host-injection seam pattern** (P1.0): `SetPlatformSharedUserDataDirectoryOverride` / `SmatchetSetInjectedFontBytes` / `ApplyUiDensityScale` — the established Core-exposes-setter / host-injects-impl seam.
- **Commit-policy seam** (P1.3): `ShouldCommitInlineFieldEdit` + `kMobileInlineEditBuild` (PR-6) — extend, don't reinvent.
- **Cache + queue + sync services** (P1.1): `LocalCacheManager`, `OfflineQueueService`, `TicketSyncService`, `BackendAuditTrail`, `GridContextDepsAdapter` — already wired and active on device.
- **stb_image attachment decode** (P1.4): already wired cross-platform in `SmatchetImageTextureCache.cpp` (`DecodeWithStb` + `RegisterUserTexture`) — verify on device, don't re-add.
- **`ITrackerBackend`** (P1.5): existing Jira/Plane abstraction.

## UX Pillar callouts

- **Pillar 1 (perf ≤6.94 ms / p99 ≤10 ms)**: grid scroll with touch cell editors active (P1.3); on real hardware, not just emulator.
- **Pillar 2 (no UI freeze >100 ms)**: JNI Keystore encrypt/decrypt (P1.0) on save/load only, never steady-state; SQLite replay tick (P1.1) off-UI-thread or budgeted; network-reachability check non-blocking.
- **Pillar 3 (never crash)**: JNI-exception safety on the Keystore bridge; SQLite error → graceful degrade (cache miss, not crash); EGL ctx-loss already handled (Phase-0 item 12). RAII over the JNI local refs.
- **Pillar 4 (a11y)**: the explicit subject of P1.6 (research, backlogged).
- **Pillar 5 (DRY)**: P1.0/P1.3 explicitly extend existing seams; the duplication gate must stay green.

## Perf-review-system gates

*(Mandatory — Phase-1 touches `Source/Core/`: the Keystore crypto seam, the sync-replay tick, and the grid/field-editor touch path.)*

Per [`docs/guides/perf-workflow.md`](../../guides/perf-workflow.md), each Source/Core-touching slice runs the affected `scripts/dev/perf-run.sh` scenarios and compares against baselines before merge:

- **P1.0 Keystore**: no steady-state scenario (crypto fires only on config save/load) — assert *absence* of per-frame cost; one save/load latency check (<100 ms, Pillar 2).
- **P1.1 Replay tick**: the sync/replay scenario must show the tick off the UI-thread frame budget (Pillar 2) — instrument with a `perf_temp:` scope on the tick, assert the UI-thread frame stays ≤6.94 ms while replay runs.
- **P1.3 Touch editors**: grid-scroll + cell-edit-open scenarios — Pillar-1 steady-state ≤6.94 ms, popup-open Pillar-2 <100 ms. Baseline = current grid-scroll scenario.
- **P1.5 Backend picker**: no steady-state scenario (the picker is a `Combo`↔`Selectable` widget swap rendered only inside the `EffectiveUiMode::Mobile` fork, while the Preferences page is open — never on the grid/scroll hot path) — assert *absence* of per-frame cost: the four `Selectable`s are O(1) + allocation-free, and the desktop `Combo` arm's codegen is unchanged (zero desktop regression by construction). No mobile perf scenario exists yet (same gap as P1.2/P1.3); CI **Perf PR-fast** is the authoritative headless gate.
- **P1.6 Accent-contrast fix (this PR)**: **zero hot-path delta — pure compile-time color constant.** The change replaces `ImVec4` literals inside `ApplySmatchetDark`, which runs once per theme-apply (boot + explicit theme switch), never per frame. No new branches, allocations, or call sites; the assignment count and codegen shape are identical to before (same `colors[ImGuiCol_*] = ImVec4(...)` writes, different constants). No steady-state scenario applies; no `perf-run.sh` subset needed. Per-frame UI cost is provably unchanged.
- **P1.6b fontScale seam (this PR)**: **zero steady-state delta — the only new Core symbol (`ComposeFontDensityScale`) is a header-only pure multiply called exclusively from the Android host on boot (`InitImGuiFirstTime`) and on `APP_CMD_CONFIG_CHANGED`, never per frame.** Desktop never calls it (no `Configuration.fontScale`), so desktop codegen is untouched by construction. The one perf-sensitive path is the **CONFIG_CHANGED atlas rebuild** (now also triggered by a runtime font-size change, not just a DPI move) — already wrapped in the Pillar-2 100 ms UI-block timer + `SLOGE` over-budget guard, and the clamp `[0.85, 2.0]` bounds the atlas size so a pathological OEM value can't blow that budget or exhaust texture memory. No `perf-run.sh` subset applies (no steady-state surface); CI **Perf PR-fast** is the headless backstop.
- **P1.4 Thumbnail decode (this PR)**: **zero UI-thread steady-state delta on every platform.** The decode (WIC on Windows, stb full-decode + `DownscaleRgba32` elsewhere) runs **off the UI thread** on the existing S5 worker-pool path (`MaybeKickThumbnailDecode` → `LaunchBackgroundTask`; upload posted back via `PostToMainThread`), rate-limited to `kMaxConcurrentThumbnailDecodes=4`, and **once per attachment** (cached after upload) — never per-frame, so Pillar-1 (≤6.94 ms) + Pillar-2 (no >100 ms freeze) hold by construction. The new `DownscaleRgba32` is O(srcW·srcH) bounded by the 16 MP pre-decode cap, on the worker. Windows codegen unchanged (WIC arm untouched) → zero desktop regression. Memory: the stb arm transiently holds full-res RGBA (≤16 MP ≈ 64 MiB per in-flight decode, ≤4 concurrent) before downscaling — a bounded worker-thread transient, not steady-state/UI cost (WIC decode-scales so it never materialises that buffer). No mobile perf scenario exists yet (same gap as P1.2/P1.3/P1.5); CI **Perf PR-fast** is authoritative.
- Each slice's execution plan carries its own filled Perf-gate section; `perf-gatekeeper` runs the diff→scenario subset at PR time.

## Risks / non-goals

- **P1.0 is a ship-to-user security gate** — plaintext token on Android must not reach a public release. security-review sign-off is mandatory; the seam must fail *closed* (if the injected crypto pair throws, refuse to persist the token rather than silently writing plaintext).
- **P1.3 is the highest-risk slice** — touch interaction model is a redesign, not a port; the long-press spike gates the rest. Risk of regressing desktop input — the `kMobileInlineEditBuild` gate must keep desktop paths byte-identical.
- **Non-goal**: iOS. Phase-1 is Android-only; the host-injection seams are designed so an iOS host *could* inject Keychain crypto later, but no iOS target ships here.
- **Non-goal**: real-time collaborative editing / push notifications.
- **Sequencing**: P1.5 depends on P1.3 (touch backend-selection UI); P1.2 and P1.4 are independent; P1.0 and P1.1 are independent and can ship first.

## Verification

- **P1.0**: unit-test the crypto seam on desktop with a stub pair (round-trip encrypt→decrypt, and that the rewired `#else` arms emit `token_enc` / read it back when an override is set); on-device — token persists across app restart, survives `adb backup` exclusion (Phase-0 `allowBackup=false`, #1067), and is *not* readable as plaintext in the on-device config file. **Migration**: an upgraded install (one that ran a Phase-0 plaintext build) no longer contains the plaintext `token` key on disk after the first post-P1.0 launch. security-review of the JNI bridge.
- **P1.1**: on-device — go offline (airplane mode), edit a field, confirm `pending_field_edits` row written; go online, confirm replay PUT fires and the row clears; force a replay failure, confirm dead-letter surface. Emulator for the airplane-mode toggle; **live PUT against a real ticket requires explicit confirmation** (external-service mutation).
- **P1.3**: emulator UI-test (ImGui Test Engine bucket-E if feasible) for the explicit-commit policy across all five RenderPlan editors; on-device long-press / Save / Cancel / Back / tap-away matrix — **discard paths must issue no PUT** (the PR-6 stray-PUT proof, extended to the four non-text editors). EMULATOR-only input injection (pin `-s emulator-5554`); never coordinate-inject the physical device.
- **Cross-cutting**: real-hardware Pillar-1 ≤6.94 ms steady-state; emulator boot+first-frame smoke (Phase-0 CI) stays green.
- Automation residue (no Android-emulator UI-interaction CI harness beyond boot-smoke yet) is tracked in [`docs/self-improvement/categories/tooling.md`](../../self-improvement/categories/tooling.md); each slice closes as much residue as feasible per `test-author`.

**Plan stress-test — `grill-with-docs` (3-lens adversarial workflow, verdict `revise` → applied).** Anchors fact-checked against code at `d8ea206c`. Two **CRITICAL** caught + fixed: (1) P1.0's seam as first drafted was a **no-op on Android** — `Protect*`/`Unprotect*` are reached only on the `#if _WIN32` arm; the non-Win32 `#else` arm of `WriteSecretFields`/`LoadSecretFields` writes/reads the token in plaintext under the `token` key, so the seam must rewire *those* arms (verified `ConfigManager.cpp:465/473/838`); (2) no scrub of the pre-existing Phase-0 plaintext token → added a mandatory migration step. **Majors fixed**: P1.3's "route the four editors through `ShouldCommitInlineFieldEdit`" was a misfit (combo/modal editors commit on explicit `Selectable`/Apply, not focus-loss — no stray-PUT bug to fix there) → re-scoped to touch-dismissal of the arm-then-popup model; P1.4's "mobile has no decoder" was **false** (decode is already cross-platform via `SmatchetImageTextureCache.cpp:141` stb_image + renderer-agnostic `ImTextureData`) → re-scoped to verify-on-device. **Minors fixed**: `UnprotectSecretFieldFromConfig` wrapper is Win32-only (link risk); font/density inject in `InitImGuiFirstTime()` not `BootCoreOnce()`; `BackendAuditTrail.h` range widened to `:27–32`; P1.3 anchor + path drift corrected (`RenderFieldCell` grid:1252, `DispatchEditorByPlan`/`StartEditingField` live in `TicketFieldEditor.cpp`, editor dirs `src/`+`src/Tracker/`, header `include/`).

## Out of scope

- iOS / desktop-mobile responsive reflow beyond the existing density scale.
- Phase-2 ideas: push notifications, offline attachment caching, biometric unlock of the Keystore key, widget/home-screen surfaces.

## Implementation log

_(per-slice; appended as each PR ships)_

- **2026-06-20 — P1.0 residual: false Android plaintext warning (own PR).** Tightened the Tracker-tab
  preferences guard in `SmatchetPreferencesUi.cpp` from `#if !defined(_WIN32)` to
  `#if !defined(_WIN32) && !defined(__ANDROID__)` and refreshed the now-stale comment (Keystore landed in
  P1.0). 1 file, +6/−5. Built desktop dual-target (light) + Android x86_64; installed + launched on the
  `smatchet_pixel` emulator and screenshot-confirmed the warning is gone on Settings ▸ Tracker. First
  slice shipped after the local Android toolchain came online (validation blocker lifted).
- **2026-06-20 — reconciliation + P1.6 (this PR).** Audited the tree against the stale `d8ea206c`
  anchors (see § Current state). Recorded P1.0 / P1.1 / P1.4 as already-shipped with evidence and
  P1.2 / P1.5 as partial — **no re-implementation attempted** (the original status was stale, not the
  code). **Shipped P1.6** (research-only slice): [`docs/mobile/PHASE1_ACCESSIBILITY_RESEARCH.md`](../../mobile/PHASE1_ACCESSIBILITY_RESEARCH.md)
  + Pillar-4 backlog `debt/2026-06-20-mobile-accessibility-pillar4.md` (TalkBack/`AccessibilityNodeProvider`
  gap, `Configuration.fontScale` seam gap, WCAG contrast audit incl. the **2.90:1 white-on-accent** fail,
  48 dp touch targets). Pure-docs PR.
- **2026-06-23 — P1.2 touch view quick-switcher (this PR).** Added `drawMobileViewQuickSwitcher`
  (`SmatchetViewsDashboardUi.cpp`, +54): a horizontal-scroll band of saved-view tabs (active one
  highlighted via `ImGuiCol_ButtonActive`) + a trailing `+##MobileNewView`, drawn in the Grid page's
  chrome between the app bar and the content dock. `drawMobileShell` reserves the band
  (`kViewSwitcherBaseHeightPx = 40 px`, scaled by touch-density × host-density) **only on the Grid page**
  — other pages get the full content region (`switcherH = gridPage ? … : 0`). Routes taps through the
  **existing** dirty-aware `viewsRequestActivate` (a dirty switch latches the shell-level discard-confirm
  modal kept rendering by `drawMobileViewsModals`) and `viewsCreateNewView` (deferred-create latch); the
  chosen id is applied *after* the draw loop so no `store`/`activeView` reference is read across the
  activation mutation. Null-`activeView` guard (both helpers deref `*activeView`). No new gate
  (`kMobileInlineEditBuild` was P1.3-specific): the band is purely additive mobile chrome behind the
  `EffectiveUiMode::Mobile` → `drawMobileShell` fork, so desktop paths are untouched by construction.
  3 files, +75/−5. Built desktop `SmatchetStandalone` (clean link) + Android x86_64 `assembleDebug`;
  emulator-verified (below).
- **2026-06-23 — P1.3 touch cell editors: long-press open + explicit-commit policy (this PR).** Builds on
  the two prior branch commits (the unit-tested pure long-press gate `52da1075` + the pure touch-popup
  commit policy `aeaff745`). This commit adds the **ImGui glue** that wires those pure seams into the live
  editors. New shared header `Source/Core/include/Ui/TouchCellEditGesture.h` exposes two seams:
  `ShouldOpenCellEditorOnGesture(cellClicked, openOnClick)` (the open gesture, consuming the unit-tested
  pure `ShouldOpenCellEditorByLongPress`) and `ArmThenPopupCellGate(...)` (the arm-then-popup state-machine
  + collapsed-preview `Selectable` shared by the four combo/popup editors), unifying both the open gesture
  and the arm scaffold across the five grid cell editors. `kMobileTouchBuild`
  (`__ANDROID__`-gated `constexpr bool`) makes the touch branch dead-eliminate on desktop, collapsing the
  gate to the exact pre-existing `clicked && (openOnClick || IsMouseDoubleClicked(0))` — byte-identical
  desktop codegen (the Pillar-3/Risk invariant on line 131). Wired three TUs: `TicketFieldEditor.cpp`
  (replaced the file-local open-helper; routed the three combo arm sites — SingleSelect / MultiSelect /
  Cascading — through the shared `ArmThenPopupCellGate`, each caller keeping its own divergent tail
  (SingleSelect's icon-overlay + lazy tooltip stays inline); kept `kMobileInlineEditBuild` for the existing
  inline-text commit policy);
  `TrackerLabelsEditor.cpp` (Labels arm site → helper); `TrackerDateTimeFieldEditor.cpp` (DateTime arm site
  → helper; **phone-centered** the modal on touch via `SetNextWindowPos(displayCenter)` vs desktop
  `MousePos`; rewired Apply/Clear/Cancel/Back through the pure `ShouldCommitTouchPopupEdit` /
  `ShouldCloseTouchPopupEdit` — Apply→canon queued PUT, Clear→empty vector, Cancel/Back→close with no PUT,
  plus no-op-PUT suppression via `valueChanged`; this gives both pure seams real callers, no dead code).
  3 files + 1 new header, ~+76/−67 (post-format). Built desktop dual-target (`SmatchetStandalone` +
  `SmatchetCore_DX12`, clean link) + Android x86_64 `assembleDebug` (BUILD SUCCESSFUL); emulator-verified
  (below). Visual-validation pause raised for the user (touches Tracker editors → ship-loop exception 5).
- **2026-06-24 — P1.5 touch backend-selection UI (this PR).** Forked `DrawTrackerBackendSelection`
  (`SmatchetPreferencesUi.cpp:234`) on `d.effectiveUiMode == EffectiveUiMode::Mobile`: when the mobile shell
  renders the Preferences page (phone, or a narrow desktop window pinned/auto-resolved to Mobile) the four
  backends (`Jira`/`Plane`/`GitHub`/`Linear`) draw as full-width `ImGui::Selectable` rows (the same touch
  idiom as the drawer page-list) instead of the tiny desktop `ImGui::Combo` whose popup hit-targets are
  finger-hostile. The touch branch writes the **identical** `d.trackerTypeBuf` the combo writes, so
  `DrawTrackerBackendConfig` below and the multi-backend `ITrackerBackend` layer are inherited unchanged —
  a widget swap, not new backend code (Pillar-5 DRY: zero new backend logic). **RUNTIME** fork
  (`effectiveUiMode`), not the P1.3 compile-time `kMobileTouchBuild` idiom, because backend-selection is a
  *layout* choice and the mobile shell legitimately renders on desktop (Auto-narrow / Mobile-pin) — the
  selectable rows must appear there too. The touch path omits `MarkPrefsDirty` exactly as the desktop combo
  does (the save at `:634` reads `trackerTypeBuf` regardless), so behaviour is identical. The density-scaled
  mobile style already enlarges each `Selectable` to a touch-comfortable target, so no explicit row height is
  needed (and no wrapper-incompatible `::ImGui::` metric getter is called — this TU `#define ImGui
  SmatchetLocalizedImGui`s, and the wrapper forwards `Selectable`/`TextUnformatted` but not metric getters).
  1 file, +18/−1. Built desktop dual-target (`SmatchetStandalone` + `SmatchetCore_DX12`, clean link via
  `with-msvc-env.sh`); Android coverage via the PR's advisory `android-ndk-arm64` CI job (pure portable
  C++14 UI logic — no new symbols/includes; the same widgets already compile in the Android-built
  `SmatchetMobileShellUi.cpp`).
- **2026-06-24 — P1.6 accent-contrast fix: WCAG AA default-theme darken (this PR).** Fixed the documented
  Finding-3 defect in `SmatchetTheme.cpp::ApplySmatchetDark` (the **default** theme, shown on desktop AND
  mobile). The accent `(0.35,0.55,0.95)` was the opaque fill behind white(0.95) Text on `ButtonActive` /
  `HeaderActive` → **white-on-fill 2.90:1**, failing both the 4.5 AA-normal floor and the 3.0 UI floor.
  **User decision (locked): global uniform darken** — replaced the accent uniformly across `ApplySmatchetDark`
  (every slot kept its existing alpha) with a single darker shade **`(0.26,0.42,0.72)`**, the one
  hue-preserving value that clears BOTH constraints (a naive swap to the research doc's `(0.20,0.34,0.62)`
  would fix white-on-fill but drop the opaque on-dark glyphs — CheckMark / SliderGrab / SeparatorActive /
  NavHighlight, 1.0α on WindowBg `(0.12,0.12,0.14)` — to 2.35:1, failing the 3.0 floor). **Computed both
  WCAG ratios in-tree** (sRGB→linear, 0.2126R+0.7152G+0.0722B, (L1+0.05)/(L2+0.05)):
    - **white(0.95)-on-fill**: 2.90:1 → **4.67:1** (≥ 4.5 AA-normal) ✅
    - **accent-on-WindowBg**: 5.09:1 → **3.16:1** (still ≥ 3.0 UI-floor; darkening *lowers* this but stays above) ✅
  `SliderGrabActive` re-derived to `(0.36,0.52,0.77)` to preserve the original "+0.10,+0.10,+0.05 one-step-
  brighter" relationship over `SliderGrab`. The 14 slots touched: CheckMark, SliderGrab, SliderGrabActive,
  ButtonActive, HeaderActive, SeparatorHovered, SeparatorActive, ResizeGrip{,Hovered,Active}, TabHovered,
  DockingPreview, TextSelectedBg, NavHighlight (alpha preserved per slot). `AiUserBubbleBg` (a separate,
  already-AA-verified 0.18α chat *tint*, doctest-pinned) intentionally **left unchanged** — it is not an
  opaque fill behind white and the P1.6 darken doesn't apply (comment updated to say so). Doctest pin added:
  `tests/Core/SmatchetThemeAccentContrast.test.cpp` (computes both contrast ratios in-test + pins the literal
  + a regression guard that the OLD accent fails). 3 files (`SmatchetTheme.cpp`, new test, `tests/CMakeLists.txt`).
  Built desktop dual-target (`SmatchetStandalone` + `SmatchetCore_DX12`, clean link via `with-msvc-env.sh`).
  Color-literal-only change — zero hot-path delta (see § Perf-review-system-gates P1.6 bullet).
  Visual-validation pause raised for the user (touches `SmatchetTheme.cpp` → ship-loop exception 5).
- **2026-06-24 — P1.6 ModernDark accent-contrast follow-up (stacked PR off #1563).** Closed the deferral the
  accent fix left open: `ApplyModernDark` carried the same opaque-fill defect (old accent `(0.45,0.65,0.95)`,
  **white-on-fill 2.12:1** — worse than SmatchetDark's). ModernDark needs its **own** shade, not the SmatchetDark
  one: its `Text(0.92,0.93,0.95)` is dimmer, so reusing `(0.26,0.42,0.72)` lands 4.45:1 (<4.5). Derived
  **`(0.29,0.42,0.62)`** (hue-faithful 1:1.45:2.14) — the band clearing BOTH ModernDark floors:
    - **text-on-fill**: 2.12:1 → **4.61:1** (≥ 4.5 AA-normal) ✅
    - **accent-on-WindowBg `(0.10,0.11,0.13)`**: 6.86:1 → **3.16:1** (still ≥ 3.0 UI-floor) ✅
  14 accent slots swapped (alpha preserved per slot); `SliderGrabActive`→`(0.39,0.52,0.67)` preserves the
  one-step-brighter delta. `TabActive` `(0.28,0.38,0.55)` (separate desaturated blue, 5.34:1 white-on-tab) and
  the low-alpha `AiUserBubbleBg` tint left unchanged (comment updated). Stacked on #1563 so the test file extends
  in place (no DRY-clone of the WCAG helpers): `SmatchetThemeAccentContrast.test.cpp` now **4 cases / 36
  assertions, SUCCESS** (`ninja-test-msvc`), incl. a guard pinning that the SmatchetDark shade fails on
  ModernDark's text. 2 files (`SmatchetTheme.cpp`, the test). Color-literal-only — zero hot-path delta. Built
  `SmatchetStandalone` clean; visual-validation pause raised (touches `SmatchetTheme.cpp` → exception 5).
- **2026-06-27 — P1.6b fontScale seam: feed `Configuration.fontScale` into the atlas size (this PR).** Closed
  the `Configuration.fontScale` seam gap the P1.6 research doc flagged. Honours the OS accessibility "Font
  size" preference (0.85 Small … ~2.0) so mobile text scales with the user's system setting. **Three
  layers, all additive:** (1) **Core pure helper** `SmatchetTheme::ComposeFontDensityScale(densityScale,
  fontScale)` (header-only, ImGui-free, in `Ui/SmatchetThemeDensity.h`) — multiplies the DPI density by the
  clamped font scale (`[kMinFontScale 0.85, kMaxFontScale 2.0]`), guards non-finite / non-positive inputs on
  both args → fall back to no-bump / 1.0. (2) **Java JNI reader** `SmatchetActivity.getDisplayFontScale()`
  returns `getResources().getConfiguration().fontScale` (1.0 if unavailable) — the NDK `AConfiguration` has
  no font-scale getter, so JNI is the only path. (3) **Android host wiring** `android_main.cpp`:
  `QueryDisplayFontScale(app)` (clone of `QueryIsChromeOS`, `()F` / `CallFloatMethod`, returns 1.0 on any
  JNI failure) feeds the composed scale into the **font-atlas pixel size only** at `InitImGuiFirstTime` and
  `OnConfigChanged`; `OnConfigChanged` now rebuilds the atlas when **either** the DPI bucket OR the font
  scale moved (a runtime font-size change lands with the density bucket unchanged). **Key design choice
  (see Deviations):** the font scale is **atlas-only** — it is deliberately NOT fed into
  `ApplyUiDensityScale` / `HostDensityScale()`, which stay raw-DPI so the Auto UI-mode logical-width
  breakpoint (`SmatchetMobileShellUi.cpp:64`) is unaffected. New `s.fontScale` host-state field tracks the
  last applied value for the change-detection gate. Doctest: `tests/Core/SmatchetThemeDensity.test.cpp`
  extended with 6 `ComposeFontDensityScale` cases (identity, composite multiply, density-passthrough, the
  `[0.85,2.0]` clamp incl. exact bounds, non-finite/non-positive fallback for both args). 4 files
  (`Ui/SmatchetThemeDensity.h`, `SmatchetActivity.java`, `android_main.cpp`, the test). Built + ran the
  Core test rig on Windows (below); the Android host code compiles only via the NDK (verification note).
- **2026-06-27 — P1.4 residual: cross-platform bitmap attachment thumbnail decode (this PR).** Ported the
  last Win32-only piece of P1.4 — the optional bitmap thumbnail-to-texture decode at
  `SmatchetAttachmentPreviewUi.cpp` (was behind a `_WIN32`-only `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS`).
  **User-locked design ("Downscale (faithful)")**: keep the shipped **WIC** path (with its `IWICBitmapScaler`
  decode-scale) untouched on Windows — zero desktop regression — and add a **stb_image** decode arm on every
  other platform that reproduces the same UX. Because stb cannot decode-scale the way WIC does (no
  `stb_image_resize` vendored), the stb arm full-decodes then **CPU area-average downscales** to the same
  ≤2048 px longest-side budget the WIC scaler enforces. New pure seam
  `Source/Core/include/Ui/RgbaDownscalePure.h` (`FitWithinLongestSide` + `DownscaleRgba32`: ImGui-free,
  allocation-bounded box-average — unit-tested on the doctest rig). Restructured the decode TU:
  `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` now **unconditional**; shared constants
  (`kMaxThumbnailDimension = 2048`, `kMaxConcurrentThumbnailDecodes = 4`) hoisted above the platform split;
  WIC under `#if defined(_WIN32)`, the new stb arm under `#else` with **memory-budget guards stb lacks
  natively** — a 32 MiB bounded file read + a `stbi_info_from_memory` 16 MP pre-decode pixel cap (DoS guard
  mirroring `SmatchetImageTextureCache.cpp`'s `DecodeWithStb`) before `stbi_load_from_memory`, then the
  longest-side downscale. stb is included **declarations-only** (the single `STB_IMAGE_IMPLEMENTATION` lives in
  `SmatchetImageTextureCache.cpp`, same Core lib, so the `stbi_*` symbols link there). The off-thread decode
  scaffold (`MaybeKickThumbnailDecode` → worker pool → main-thread upload) and the renderer-agnostic upload
  (`CreateAttachmentTextureFromRgba` / `RegisterUserTexture` / `ImTextureData`) were **already** cross-platform —
  only the DECODE was Win32-only. 2 files modified (`SmatchetAttachmentPreviewUi.cpp`, `tests/CMakeLists.txt`)
  + 2 new (`RgbaDownscalePure.h`, `tests/Core/RgbaDownscalePure.test.cpp`). Built both targets: Windows
  doctest rig (WIC arm + pure helper) + real Android x86_64 NDK (stb arm compiles clean on Clang 17,
  `libSmatchetMobile.so` links). DRY exemption for the idiomatic ImGui-localization + Win32 preamble clone
  (see § Deviations).
- **2026-06-27 — P1.4 CodeRabbit round on #1572: fail-closed thumbnail decode budget (`9247fc6e`).** CR
  flagged the stb decode-budget gate: the original code wrapped the `kMaxThumbnailDecodePixels` dimension
  check *inside* `if (stbi_info_from_memory(...) != 0)`, so a header that **failed** info-parse skipped the
  budget entirely and fell through to an unbounded `stbi_load_from_memory` — a DoS hole (attacker crafts a
  header `stbi_info` rejects but `stbi_load` decodes, bypassing the cap; `stbi_info`/`stbi_load` take
  different code paths and can disagree). Fix: (1) **fail closed** — `stbi_info_from_memory == 0` now returns
  an error instead of skipping the gate; (2) the budget check is **unconditional** (overflow-safe
  `unsigned long long` product, rejects `infoW/H <= 0`); (3) **defense-in-depth** — re-checks decoded `w*h`
  against the cap AFTER the decode (decoded dims can diverge from the header path) before the RGBA copy,
  freeing `pix` on overflow. 1 file (`SmatchetAttachmentPreviewUi.cpp`, +21/-7). Compiled clean
  (`SmatchetCore_DX12` object), lint diff-gate PASS, **security-review CLEAN** (no CRITICAL/HIGH/MEDIUM; the
  reported hole genuinely fixed; one P3 awareness-only note — `STBI_MAX_DIMENSIONS` left at the stb default
  `1<<24` per side vs the 16 MP area cap, backstopped by the existing 32 MiB file cap — out of scope for
  this commit).

## Deviations from plan

_(per-slice)_

- **Status correction (2026-06-20).** The plan's "no Phase-1 slices started" status was stale; P1.0,
  P1.1, P1.4 were already shipped (and P1.2 / P1.5 partially) before this plan was re-opened. See
  § Current state — the deviation is in the *plan's bookkeeping*, not the code.
- **P1.0 residual — resolved 2026-06-20** (was: "deferred, no buildable C++ environment"). Shipped once
  the local Android toolchain + emulator came online; the guard fix is now compile- and emulator-verified.
- **Remaining code slices — un-escalated 2026-06-20** (was: escalated for lack of a device). The Android
  emulator + NDK + desktop MSVC build are now available locally, so P1.2 / P1.3 / P1.5 are being executed
  + emulator-verified per slice instead of escalated. P1.3 stays spike-first + visual-validation-gated.
- **P1.2 — `tests-out-of-band` label applied (2026-06-23).** The Test-delta gate FAILed because the slice
  touches two `Source/Core/` files (`SmatchetMobileShellUi.cpp`, `SmatchetViewsDashboardUi.cpp`) with no
  matching `tests/` delta. `drawMobileViewQuickSwitcher` is pure **ImGui draw surface** (direct ImGui
  immediate-mode calls + the `ViewState` store/activate helpers) — the `test-rig` agent explicitly refuses
  ImGui surfaces, so a doctest unit test is the wrong vehicle. The only pure seam (the band-height
  reservation arithmetic in `drawMobileShell`) is trivial subtraction not worth extracting at the cost of
  churning a clean UI slice. Applied the gate's sanctioned `tests-out-of-band` escape **with** a concrete
  deferred-automation plan (not a flat out-of-scope): a bucket-E ImGui Test Engine case that drives the
  switcher band — tap a non-active tab → assert active view changes; tap a tab on a dirty view → assert the
  discard-confirm modal latches; assert the band is absent on non-Grid pages. Backlogged at
  `docs/self-improvement/categories/test/2026-06-23-mobile-view-switcher-bucket-e.md`. Automation is
  currently CI-blocked anyway: the Mesa-GL bucket-C/E lanes can't boot the CI exe (AGENTS.md merge-gates),
  so bucket-E mobile coverage isn't runnable until that lane is restored. On-emulator verification (below)
  covers the behaviour in the interim.
- **P1.3 — files-to-modify drift (2026-06-23).** The original §Files-to-modify listed an Android-input
  change for the long-press timer (item *d*); found **unnecessary** — `imgui_impl_android` sustains
  `io.MouseDown[0]` for the full hold, so ImGui's own `io.MouseDownDuration[0]` *is* the long-press timer.
  No custom Android-path code. Net file delta vs plan: **+1** new shared header
  (`Source/Core/include/Ui/TouchCellEditGesture.h`, not in the original list), **−** the planned
  Android-input file(s). Item *c* ("single-click-to-edit for empty cells") also needs **no new code**:
  `SingleClickToEditGridCells` (default-true) folds into the helper's `openOnClick`, and on the touch build
  long-press supersedes (`openOnClick` ignored). This slice implements the `grill-with-docs` re-scope
  already recorded on plan line 144 (arm-then-popup touch-dismissal, not "route four editors through
  `ShouldCommitInlineFieldEdit`"). The Test-delta gate is satisfied without an out-of-band label: the
  branch carries the `tests/Core/TicketFieldEditorCommitPolicyPure.test.cpp` delta (commits `52da1075` /
  `aeaff745`) covering every pure seam the glue routes through; the glue itself is ImGui immediate-mode
  surface (`test-rig` refuses ImGui), deferred to the bucket-E follow-up below.
- **P1.3 — incidental CI fix bundled (2026-06-24).** The sole genuine RED on this PR was the
  **`Agentic self-tests (bats)`** check, not the mobile diff: `test-pre-push-merged-pr-guard.sh` reported
  6/12 failures **only on the PR's merge-ref checkout** (`refs/pull/1552/merge`), green on a head-only local
  run. Root cause — the test was **non-hermetic**: GitHub `pull_request` checks out the merge ref (head
  auto-merged with `develop`), whose `pre-push` hook contains develop's **gate D** (local-CI delta-gate);
  the test's stripped `env -i` sandbox lacks gate D's tooling (python/clang-format/…), so
  `test-lint-rules.sh` exits non-zero → the hook refuses early, *before* the merged-PR guard the test
  actually exercises. Fixed by neutralising the two sibling pre-gh probes inside the test's `run_hook`
  (`SMATCHET_SKIP_PRESHIP_GATE=1` + `SMATCHET_ALLOW_UNLOCKED_PUSH=1`), isolating the unit under test.
  Verified 12/12 against the gate-D merge-ref hook in a throwaway merge-ref worktree (the exact CI
  condition). Bundled on this branch rather than split to its own PR because it is the gating RED *for this
  PR* and must live on this branch's merge ref for CI to pick it up; 1 file, test-env only, no product code.
  Self-improvement note: `docs/self-improvement/categories/test/2026-06-24-pre-push-guard-test-not-hermetic.md`.
- **P1.5 — "depends on P1.3 chrome" was looser than the spec implied; runtime idiom chosen (2026-06-24).**
  The §Approach line cast P1.5 as *blocked on* P1.3's touch chrome. In practice the backend picker needs
  **none** of P1.3's machinery — no long-press, no arm-then-popup, no `kMobileTouchBuild`. It reuses the
  pre-existing **mobile-shell runtime fork** (`d.effectiveUiMode == EffectiveUiMode::Mobile`, the same seam
  P1.0/P1.2 already ride) and the plain `Selectable` page-list idiom. So the only real P1.3 dependency was
  *temporal* (P1.3 landed first), not architectural. Deliberately **did not** use P1.3's compile-time
  `kMobileTouchBuild` constexpr: that idiom keeps desktop codegen byte-identical for the grid cell editors,
  but backend-selection is a *layout* decision and the mobile shell renders on desktop too (Auto-narrow /
  Mobile-pin), so the touch rows must be reachable at runtime on desktop — a compile-time fork would wrongly
  hide them. Net file delta vs the §Files-to-modify "touch backend-selection UI" row: **1 existing file**
  (`SmatchetPreferencesUi.cpp`), **no new file**, **no backend-code change**.
- **P1.5 — `tests-out-of-band` anticipated (2026-06-24).** Like P1.2, the slice touches one `Source/Core/`
  file with no `tests/` delta and the changed surface is pure **ImGui immediate-mode draw** (a `Combo`↔
  `Selectable` widget swap behind the `EffectiveUiMode::Mobile` fork) — `test-rig` refuses ImGui surfaces, so
  a doctest is the wrong vehicle and there is no new pure seam to extract (the picker writes the existing
  `trackerTypeBuf`; the case-insensitive match it reads was already present). If the Test-delta gate FAILs,
  apply the sanctioned `tests-out-of-band` escape with the deferred-automation plan folded into the **same**
  bucket-E ImGui Test Engine backlog as P1.2/P1.3 (drive the mobile Preferences page → tap a backend row →
  assert `trackerTypeBuf` / `cfg.TrackerType` flips → assert the desktop `Combo` path still works when
  `effectiveUiMode == Desktop`). That bucket-E lane is CI-blocked today (Mesa-GL can't boot the CI exe).
- **P1.6 — ModernDark NOT darkened in this slice; flagged as a one-line follow-up (2026-06-24).** The brief
  permitted mirroring the uniform darken into `ApplyModernDark` (non-default, opt-in theme, even brighter
  `(0.45,0.65,0.95)` accent with the same white-on-fill failure) **only if it were a clean mechanical mirror
  that keeps that theme coherent.** It is **not**: ModernDark's `Text` is `(0.92,0.93,0.95)` (dimmer than
  SmatchetDark's `0.95`) and its `WindowBg` is `(0.10,0.11,0.13)`. Reusing the SmatchetDark target
  `(0.26,0.42,0.72)` lands ModernDark at **text-on-fill 4.45:1 — *below* the 4.5 AA-normal floor** (vs
  SmatchetDark's 4.67:1). A clean mechanical mirror would therefore ship a *still-failing* ModernDark — the
  exact "trade one failure for another" trap the brief said to STOP on. ModernDark needs its **own** slightly
  different shade (feasible band ≈`(0.28,0.41,0.60)`, text-on-fill ≥4.5 AND accent-on-WindowBg ≥3.0). Left
  untouched here. **Follow-up:** apply ModernDark's own AA-clearing accent (separate slice — non-default
  theme, not on the mobile-default path). Backlog: tracked in the Pillar-4 debt entry
  `debt/2026-06-20-mobile-accessibility-pillar4.md`.
  **✅ Resolved 2026-06-24 (stacked PR off #1563).** ModernDark darkened to its **own** AA-clearing shade
  **`(0.29,0.42,0.62)`** (the precisely-derived value; the ≈`(0.28,0.41,0.60)` band estimate above was close).
  On ModernDark's dimmer `Text(0.92,0.93,0.95)` / `WindowBg(0.10,0.11,0.13)`: **text-on-fill 2.12:1 → 4.61:1**
  (≥4.5 AA-normal) and **accent-on-WindowBg 6.86:1 → 3.16:1** (≥3.0 UI-floor). 14 accent slots swapped (alphas
  preserved); `SliderGrabActive`→`(0.39,0.52,0.67)` keeps the +.10,+.10,+.05 step; `TabActive` `(0.28,0.38,0.55)`
  (separate desaturated blue, 5.34:1 white-on-tab) and the low-alpha `AiUserBubbleBg` tint left untouched.
  `SmatchetThemeAccentContrast.test.cpp` extended (now **4 cases / 43 assertions, SUCCESS**) with two ModernDark
  cases incl. a regression guard pinning that the SmatchetDark `(0.26,0.42,0.72)` shade fails AA on ModernDark's
  dimmer Text (the documented reason the two themes carry different shades). **Post-review (CR finding, 2026-06-27):**
  the ModernDark literal-shade case was widened from 8 RGB-only slots to **all 14 re-tinted slots pinned RGBA**
  (new `ApproxEqRgba` helper) so an omitted-slot OR an alpha-flatten regression can't slip through.
- **P1.6 — golden-image-approval: no golden regenerated; surfaced for human verdict (2026-06-24).** This
  restyles the default `SmatchetDark` accent, so the three bucket-C goldens (`dock-gap-sentinel`,
  `command-palette-fuzzy`, `code-syntax-coloring`) **would** diff where an active tab / selected-row /
  `HeaderActive` accent fill is visible (the goldens in this worktree are captured under the live SmatchetDark
  theme — the stock-style insulation knob `SMATCHET_TEST_DEFAULT_IMGUI_THEME` described in
  `docs/agent-rules/golden-image-approval.md` is **not wired in this tree**, a separate gap). Per the
  golden-image-approval contract, an agent **must not** headlessly bootstrap or self-approve a golden — the
  user opens the captured PNGs and gives an explicit "looks right" verdict before any `git add tests/golden/`.
  **Mitigating facts:** the `bucket-c-screenshot-diff` job **and** its inner diff step are both
  `continue-on-error: true` (advisory), and `Bucket-` was dropped from the merge-gate meant-to-block
  allow-list (Mesa-GL can't boot the CI exe) — so **no merge gate blocks on these goldens**. Goldens are also
  per-developer-bootstrapped (GPU-specific), not authoritative committed references. **Action for the user:**
  if you want the committed goldens refreshed, run `bash scripts/dev/test-screenshot-diff.sh --bootstrap`,
  eyeball the 3 PNGs, then approve. No approval marker / label is needed on the PR itself (advisory lane).
- **P1.6 — Test-delta gate satisfied (no out-of-band label needed).** Unlike P1.2 / P1.3 / P1.5, this slice
  ships a real `tests/` delta: `tests/Core/SmatchetThemeAccentContrast.test.cpp` is a pure doctest (no ImGui
  *surface* — it only reads `ImGui::GetStyle().Colors` after `ApplyStyle`, the same pattern as the existing
  `SmatchetThemeAiColors.test.cpp`) that computes both WCAG ratios and pins the literal. `test-rig`'s
  ImGui-surface refusal doesn't apply (palette readback, not draw).
- **P1.6b — font scale composed into the ATLAS only, NOT into `ApplyUiDensityScale` (2026-06-27).** The slice
  was framed as "feed `Configuration.fontScale` into the existing `ApplyUiDensityScale` seam." Literal wiring
  has a **side effect**: `ApplyUiDensityScale` owns `HostDensityScale()`, which the Auto UI-mode breakpoint
  consumes as the logical-width divisor — `logicalWidth = io.DisplaySize.x / HostDensityScale()`
  (`SmatchetMobileShellUi.cpp:64`). Folding the font factor in would shrink `logicalWidth` by up to 2× at the
  max font scale and could wrongly flip a large-font tablet from the Desktop dockspace into the Mobile shell.
  **Decision (user-confirmed via AskUserQuestion, "Text-only atlas"):** compose the font scale into the
  **font-atlas pixel size only** (the platform-conventional behaviour — Android `fontScale` scales *text*);
  keep `ApplyUiDensityScale` / `HostDensityScale()` on the raw DPI density so the Auto breakpoint + mobile
  band heights stay correct. Style **metrics** (padding / hit-target sizes) are therefore NOT enlarged by the
  font scale in this slice — a documented, bounded follow-up (Scope 2 "Text + metrics"): scale metrics too
  via a new host-injection seam (`SmatchetSetHostFontScale`) composed into the existing `ApplyTouchScale`
  path (Auto-safe — transient-on-top-of-base, persists across `ApplyStyle`), backlogged in the Pillar-4 debt
  entry `debt/2026-06-20-mobile-accessibility-pillar4.md`.
- **P1.6b — Android host + Java are NDK-only; not covered by the Windows build (2026-06-27).** The Core pure
  helper + its doctest build and run on the Windows `ninja-test-msvc` rig (below). `android_main.cpp` +
  `SmatchetActivity.java` compile only through the NDK / Gradle Android toolchain — the desktop MSVC build
  excludes them, so the JNI wiring is verified by (a) cloning the proven `QueryIsChromeOS` JNI idiom
  byte-for-byte (same env-acquire / `JNI_EDETACHED` attach, `ExceptionClear` on missing-method,
  `DeleteLocalRef`, false-safe default), and (b) the Android `assembleDebug` build + on-emulator font-scale
  confirmation recorded under § Verification (actual).
- **P1.4 — "no new decode code" premise was wrong; a pure downscale helper was required (2026-06-27).** The
  re-scoped slice (plan line 79) predicted "**No new dependency, likely no new decode code**" — verify the
  existing cross-platform `SmatchetImageTextureCache.cpp` path on device and *only* port the optional bitmap
  thumbnail if in scope. The port itself **did** need new code: WIC decode-scales during the pixel pull, but
  stb_image cannot (no `stb_image_resize` is vendored — confirmed against `Source/Core/ThirdParty/stb/`), so
  faithfully matching the Windows UX on other platforms required a NEW pure area-average downscale helper
  (`RgbaDownscalePure.h`) plus its unit test. Net file delta vs the §Files-to-modify P1.4 row (which named only
  `SmatchetAttachmentPreviewUi.cpp`): **+2 new files** (`Source/Core/include/Ui/RgbaDownscalePure.h`,
  `tests/Core/RgbaDownscalePure.test.cpp`) + the `tests/CMakeLists.txt` registration. No new third-party
  dependency (stb is already vendored); the "no new dependency" half of the premise held.
- **P1.4 — `duplication` (DRY Pillar 5) exemption, not extraction (2026-06-27).** Removing the in-`_WIN32`-block
  `#define SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` (it had to go unconditional) merged the file's
  ImGui-localization + Win32 preamble into one contiguous matching run, tripping the now-**blocking** dup gate
  (graduated 2026-06-21, ADR-0015) as an 80-token clone vs `TicketFieldEditor.cpp` (the
  `#include "imgui.h"`/`imgui_internal.h`/`SmatchetLocalizedImGui.h` + `#define ImGui SmatchetLocalizedImGui` +
  `WIN32_LEAN_AND_MEAN`/`NOMINMAX` guard preamble). Chose **exemption over extraction** (the opposite of P1.3's
  arm-then-popup call) because this is *idiomatic per-TU boilerplate, not logic*: the `#define ImGui` wrapper
  must follow the imgui includes per-TU, so it is not extractable into a shared header without breaking the
  established localized-ImGui idiom (the same reasoning behind the existing `JiraClient.h` / `GitHubClient.h`
  interface-symmetry dup exemptions). Marker placed within the clone span:
  `SMATCHET_DEVIATION(rule=duplication; reason=idiomatic per-TU ImGui-localization preamble …; owner=ui-host;
  revisit=2026-12-31)`.

## Verification (actual)

_(per-slice)_

- **P1.0 residual (2026-06-20):** desktop dual-target build (`cmake --build --preset ninja-iter-msvc
  --target SmatchetStandalone SmatchetCore_DX12`, light features) → both link clean. Android x86_64
  `assembleDebug` → `app-debug.apk` (27 MB). Installed `-r -t` on the `smatchet_pixel` x86_64 API-34
  emulator, launched `com.smatchet.mobile/.SmatchetActivity`, clean boot (no logcat FATAL). Navigated
  Settings ▸ Tracker and screenshot-confirmed the "API token is stored unencrypted" banner is **absent**
  (pre-fix it rendered between the backend combo and "Jira Configuration"). Windows TU byte-identical by
  construction (the warning block stays excluded under `_WIN32`).
- **P1.6 (this PR):** pure-docs slice — validated with `bash scripts/dev/test-docs.sh` (the local mirror
  of the `doc-validation.yml` gate: anchor resolution, plan-ref integrity, kebab/naming, agent contract).
  The findings doc's WCAG contrast ratios are computed from the source palette values (deterministic,
  device-independent). On-device TalkBack / touch-target confirmation is a documented follow-up — no
  Android device/emulator in the authoring environment.
- **P1.6b fontScale seam (this PR):**
  - **Build** — configure + build `SmatchetTests` via `bash scripts/dev/with-msvc-env.sh cmake --preset
    ninja-test-msvc` then `--build --preset ninja-test-msvc --target SmatchetTests` → exit 0, clean.
  - **Unit** — `build/ninja-test-msvc/tests/SmatchetTests.exe --test-case="*ComposeFontDensityScale*"` →
    **6 cases / 17 assertions, SUCCESS** (identity, composite multiply, density-passthrough, the
    `[kMinFontScale 0.85, kMaxFontScale 2.0]` clamp incl. exact bounds, non-finite/non-positive fallback for
    both args).
  - **Android host / JNI** — NDK-only (see § Deviations); the desktop MSVC build excludes `android_main.cpp`
    + `SmatchetActivity.java`. Residual manual step: `assembleDebug` → install `-r -t` on `emulator-5554` →
    set Settings ▸ Display ▸ Font size to Largest → confirm Smatchet text grows while the Auto UI-mode
    layout (Desktop-vs-Mobile shell selection) is unchanged; then change font size while the app is
    foregrounded → confirm `CONFIG_CHANGED … atlas-rebuild` logcat line + live text re-scale within the
    Pillar-2 100 ms budget.
- **P1.2 touch view quick-switcher (2026-06-23):**
  - **Build** — desktop `SmatchetStandalone` (`cmake --build --preset ninja-iter-msvc`, light features)
    links clean; Android x86_64 `assembleDebug` → `app-debug.apk` BUILD SUCCESSFUL.
  - **Emulator** — installed `-r -t` on `emulator-5554`, launched `.SmatchetActivity`: clean boot
    (PID 5490, no logcat FATAL, EGL GLES3 surface 1080×1920, ImGui density 2.62, steady frames).
  - **On-device feature checks** (screenshot-confirmed, taps pinned `-s emulator-5554`): (1) the switcher
    band renders between the app bar and the grid dock with one tab per saved view; (2) the active view's
    tab carries the `ButtonActive` highlight; (3) tapping `+##MobileNewView` on a dirty view raised the
    shell-level **"Discard changes?"** confirm (proves the reuse of `viewsRequestActivate` /
    `drawMobileViewsModals` dirty path); (4) confirming the switch reloaded the grid for the new view;
    (5) the band is **absent** on non-Grid pages (Settings/Activity) — the `gridPage` gate holds.
  - **Perf** — no mobile perf scenario exists; the band is mobile-only (behind the
    `EffectiveUiMode::Mobile` → `drawMobileShell` fork) so no desktop scenario path touches it →
    zero desktop regression by construction. The local `priority-grid-scroll` headless run flaked
    (the GUI harness hit its own `scenario.run` timeout → `std::terminate`, a known local-harness
    issue, not a code regression — desktop exe boots + `perf.reset` runs fine). CI **Perf PR-fast** is
    the authoritative headless gate; real-hardware mobile Pillar-1 remains the cross-cutting deferred
    gate (plan lines 111/141).
  - **Lint** — `agents/scripts/project/test-lint-rules.sh --diff origin/develop` green (exit 0;
    `Source/Core/src/Ui/` is the Light/ungated zone, no strict-zone files touched).
- **P1.3 touch cell editors (2026-06-23):**
  - **DRY extraction** — the first glue draft *flattened* the three combo arm sites inline, which made the
    MultiSelect / Cascading scaffolds byte-identical to the Labels arm site → the `duplication` gate FAILed
    with two NEW 103-token clones (`TicketFieldEditor.cpp` MultiSelect/Cascading ↔ `TrackerLabelsEditor.cpp`
    Labels). Fixed by **extraction, not exemption**: hoisted the shared arm-then-popup state-machine +
    collapsed `Selectable` into `SmatchetTouchEdit::ArmThenPopupCellGate` (header) and routed all four combo/
    popup editors through it (chose extraction over a `SMATCHET_DEVIATION(rule=duplication)` because leaving
    a clone of the very scaffold the slice unifies is incoherent). Re-verified clean:
    `python agents/scripts/core/dup_audit.py --diff origin/develop` → **exit 0** (zero new clones).
  - **Lint** — `agents/scripts/project/test-lint-rules.sh --diff origin/develop` green (exit 0), including
    the **strict zone** `Source/Core/src/Tracker/` (`TrackerLabelsEditor.cpp` + `TrackerDateTimeFieldEditor.cpp`):
    no new strict-zone / comment-noise / oversized-function / include-cycle / fan-in violations. (Only the
    pre-existing non-blocking `comment-ratio` WARN on `TicketFieldEditorCommitPolicyPure.h`.)
  - **Perf** — desktop codegen is **byte-identical** by construction: `kMobileTouchBuild` is
    `constexpr false` off-Android, so `ShouldOpenCellEditorOnGesture` dead-eliminates the touch branch
    and collapses to the exact pre-existing `clicked && (openOnClick || IsMouseDoubleClicked(0))`
    expression — no desktop scenario path changes, zero desktop regression by construction. The new
    touch open/arm gesture is mobile-only (`__ANDROID__`), and the per-cell collapsed-preview helpers
    (`EmptySelectPreviewLabel`, the arm-then-popup `Selectable`) are O(1), allocation-free, and already
    on the SingleSelect hot path; the DateTime no-op canon round-trip runs only on Apply/Clear (a user
    action, never per-frame). No mobile perf scenario exists yet (same gap as P1.2); CI **Perf PR-fast**
    is the authoritative headless gate and `perf-gatekeeper` runs the diff→scenario subset at PR time;
    real-hardware mobile Pillar-1 remains the cross-cutting deferred gate (plan lines 111/141).
  - **Build (post-extraction rebuild)** — desktop dual-target
    (`cmake --build build/ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`, via
    `with-msvc-env.sh`) → both link clean; Android x86_64 `assembleDebug` → `app-debug.apk` **BUILD
    SUCCESSFUL (54.6 MB)**. Proves the `ArmThenPopupCellGate` extraction + the self-included
    `imgui_internal.h` compile in all three targets.
  - **Emulator (rebuilt binary)** — installed `-r -t` on `emulator-5554`, launched `.SmatchetActivity`:
    clean boot (PID 7698, no logcat FATAL, EGL GLES3 1080×1920, swiftshader ~28–32 ms/frame — software
    renderer, not device-representative perf). Taps pinned `-s emulator-5554`; **never** coordinate-injected
    the physical device.
  - **On-device discard matrix** (screenshot-confirmed, logcat mutation-tag scan = **zero**
    `BackendAuditTrail` / `FieldEditAudit` / `AppendBegin` / `QueueFieldEdit` after IME-noise filter):
    (1) **inline text** — long-press the *Description* cell → `EditLongText` modal opens with the live value
    ("tghg") → `Escape` → modal closes, **no PUT, no queued edit**; (2) **SingleSelect combo** — long-press
    the *Assignee* cell → the combo arms + opens (soft-keyboard filter raised) → tap-away on the app-bar →
    combo closes, cell reverts to the original value, **no mutation tag**. These are the PR-6 stray-PUT proof
    extended to the rebuilt post-extraction binary.
  - **Live commit→PUT — user-authorised, emulator-blocked (escalate-when-unvalidatable)** — the user granted
    the external-service mutation ("safe to mutate any ticket"). The **Save / Apply → live PUT** path was
    hand-attempted on `emulator-5554` (long-press the *Assignee* SingleSelect → arm/open → narrow the filter →
    select a different user) across multiple tries over two sessions, but a **clean capture could not be
    obtained**: the emulator's soft-keyboard (auto-raised by the combo filter InputText) + the variable
    Read-tool screenshot scale make coordinate-injected option-taps land on the filter field rather than the
    target Selectable, and `Escape` collapses the whole combo. This is an **emulator/swiftshader input-tooling
    limitation, not a product defect** (the same `ServiceNotFoundException: No service published for: input`
    boot-race flakes the CI `Android emulator smoke` lane). Per the escalate-when-unvalidatable invariant the
    result is recorded honestly rather than claimed. **Safety**: a full-session logcat scan confirmed **zero**
    mutation tags — no accidental PUT fired; the ticket assignee remained `Alexandros Konstantonis` throughout;
    input stayed pinned `-s emulator-5554`. Commit-path **correctness** is covered by the pure unit test
    `ShouldCommitTouchPopupEdit` (Save=PUT / unchanged=no-op) and the deferred bucket-E case below; the
    on-device evidence here is the **discard** half of the matrix (the security-critical stray-PUT proof).
  - **Deferred automation** — the full long-press / Save / Cancel / Back / tap-away matrix across all five
    editors (incl. MultiSelect / Cascading / Labels / DateTime-centering) is glue-on-ImGui-surface, so it is
    backlogged as a bucket-E ImGui Test Engine case (CI-blocked today on the Mesa-GL lane, same as P1.2);
    backlog entry below.
- **P1.5 touch backend-selection UI (2026-06-24):**
  - **Build** — desktop dual-target via the MSVC env wrapper
    (`bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
    SmatchetCore_DX12`) → both link clean (`SmatchetCore_DX12.lib` + `Smatchet.exe`, 117/117). Android
    coverage is the PR's advisory `android-ndk-arm64` CI job — the change is pure portable C++14 UI logic
    (no new symbols, no new includes; `Selectable` / `TextUnformatted` / `CopyStringToBuffer` / `IM_ARRAYSIZE`
    all already compile in the Android-built `SmatchetMobileShellUi.cpp` + this same TU), so a local Android
    reconfigure (which needs CI-runtime OpenSSL-for-Android absolute paths) was skipped in favour of the
    authoritative CI Android lane.
  - **Behaviour-identical save path** — the touch branch omits `MarkPrefsDirty` exactly as the desktop combo
    does; the Tracker-tab save reads `d.trackerTypeBuf` unconditionally (`SmatchetPreferencesUi.cpp:634`,
    `cfg.TrackerType = NormalizeViewsBackendKey(trackerTypeBuf)`), so both widget paths persist identically.
    The case-insensitive `currentItem` resolve (handles hand-edited lowercase `plane`/`github`/`linear`) is
    shared by both branches.
  - **Perf** — see the §Perf-review-system-gates P1.5 bullet: the picker is rendered only inside the
    `EffectiveUiMode::Mobile` fork, fires the four `Selectable`s once per Preferences-page frame (O(1),
    allocation-free, only while Settings is open — never on the grid hot path), and changes **zero** desktop
    `Combo` codegen (the `else if` arm is unchanged). No new per-frame steady-state cost; no mobile perf
    scenario exists (same gap as P1.2/P1.3); CI **Perf PR-fast** is the authoritative headless gate.
  - **Lint** — `agents/scripts/project/test-lint-rules.sh --diff origin/develop` to be run before push
    (`Source/Core/src/Ui/` is the Light/ungated zone; no strict-zone file touched).
  - **Emulator** — on-device confirmation of the touch rows rendering in Settings ▸ Tracker on
    `emulator-5554` is a documented follow-up bundled with the bucket-E automation (taps pinned
    `-s emulator-5554`; never coordinate-inject the physical device).
- **P1.6 accent-contrast fix (2026-06-24):**
  - **Build** — desktop dual-target via the MSVC env wrapper
    (`bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
    SmatchetCore_DX12`) → **both link clean** (`SmatchetCore_DX12.lib` step 886/895 + `Smatchet.exe`
    step 894/895, no errors/warnings). This worktree had no pre-existing `build/` dir (fresh worktree), so a
    one-time non-destructive `cmake --preset ninja-iter-msvc` configure preceded the build — nothing was
    wiped or reconfigured-over (no incremental state existed to preserve).
  - **WCAG contrast math (computed in-tree, not taken from the research doc):** sRGB→linear, luminance
    `0.2126R+0.7152G+0.0722B`, ratio `(L1+0.05)/(L2+0.05)`.
    - white(0.95)-on-fill: **2.90:1 (before) → 4.67:1 (after)** — clears the 4.5 AA-normal floor. ✅
    - accent-on-WindowBg(0.12,.12,.14): **5.09:1 (before) → 3.16:1 (after)** — still clears the 3.0 UI-floor
      (darkening lowers this; the chosen `(0.26,0.42,0.72)` is the band that keeps BOTH above floor). ✅
    The research doc's suggested `(0.20,0.34,0.62)` was **rejected** by this math (white-on-fill 6.29 but
    accent-on-dark drops to 2.35 — a new 3.0-floor failure). No single shade trading one fail for another
    was shipped.
  - **Doctest pin** — `tests/Core/SmatchetThemeAccentContrast.test.cpp` (registered in `tests/CMakeLists.txt`)
    recomputes both ratios in-test (CHECK ≥4.5 and ≥3.0 + Approx pins at 4.665 / 3.163) and guards that the
    OLD `(0.35,0.55,0.95)` accent fails AA. **Run locally** under `ninja-test-msvc` (one-time fresh-worktree
    configure, no destructive reconfigure of the `ninja-iter-msvc` product dir): `SmatchetTests.exe`
    `--test-case="*accent*"` → **2 cases / 17 assertions, 0 failed (SUCCESS)**. The CI Coverage/Test lane
    reruns it as the authoritative backstop.
  - **Perf** — zero hot-path delta. Color-literal-only change inside `ApplySmatchetDark` (runs once per
    theme-apply, never per frame); identical assignment count + codegen shape, different constants. No
    `perf-run.sh` subset applies (see §Perf-review-system-gates P1.6 bullet).
  - **Visual-validation** — **PENDING (ship-loop exception 5).** Touches `SmatchetTheme.cpp` with no
    bucket-C/E coverage that gates → the launched exe is surfaced to the user for eyeball verification before
    merge. Exe: `C:\Dev\trees\mobile-p1.6\build\ninja-iter-msvc\Smatchet.exe`.
- **P1.6 ModernDark accent-contrast follow-up (2026-06-24, stacked off the SmatchetDark fix):**
  - **Why a separate shade** — ModernDark is the opt-in (non-default) theme and carried the SAME old defect
    (its old accent `(0.45,0.65,0.95)` → white-on-fill **2.12:1**). But its Text is `(0.92,0.93,0.95)` —
    *dimmer* than SmatchetDark's `(0.95,0.95,0.95)` — so the shared SmatchetDark shade `(0.26,0.42,0.72)`
    lands only **~4.45:1** on ModernDark's text (below 4.5). ModernDark needs its own darker
    `(0.29,0.42,0.62)`.
  - **Build** — desktop product via the MSVC env wrapper
    (`bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone`)
    → `Smatchet.exe` links clean (no errors/warnings). Reuses the parent slice's already-configured
    `ninja-iter-msvc` dir (no reconfigure).
  - **WCAG contrast math (in-tree):** ModernDark Text `(0.92,0.93,0.95)` on WindowBg `(0.10,0.11,0.13)`.
    - text-on-fill: **2.12:1 (before) → 4.61:1 (after)** — clears the 4.5 AA-normal floor. ✅
    - accent-on-WindowBg: **6.86:1 (before) → 3.16:1 (after)** — still clears the 3.0 UI-floor. ✅
    - The shared SmatchetDark shade `(0.26,0.42,0.72)` was **rejected** here (≈4.45 < 4.5 on the dimmer text);
      `(0.29,0.42,0.62)` is the hue-faithful value clearing both ModernDark floors.
  - **Doctest pin** — `tests/Core/SmatchetThemeAccentContrast.test.cpp` **extended** (no new file → reuses the
    SmatchetDark WCAG helpers, no Pillar-5 clone) with 2 ModernDark cases: literal-shade pin
    (`== (0.29,0.42,0.62)`, SliderGrabActive `(0.39,0.52,0.67)` one step brighter, `CHECK_FALSE` it equals
    the SmatchetDark shade) + the dual-floor WCAG pin (`Approx(4.610)/(3.160).epsilon(0.01)`, regression
    guards that BOTH the old `(0.45,0.65,0.95)` AND the shared `(0.26,0.42,0.72)` fail AA on ModernDark text).
    Full file: **4 cases / 43 assertions, 0 failed (SUCCESS)** under `ninja-test-msvc`; CI Coverage/Test lane
    is the authoritative backstop.
  - **Post-review fixes (2026-06-27, clearing the merge wedge):** (1) CR flagged the ModernDark literal-shade
    case as under-covering the changed surface (8 RGB-only slots of 14) — widened to **all 14 re-tinted slots
    pinned RGBA** via a new `ApproxEqRgba` helper, plus a `TabActive` guard (the separate desaturated blue must
    NOT collapse onto the accent), so omitted-slot AND alpha-flatten regressions are both caught (36→43
    assertions). (2) `comment-commented-out-code` false-positive on the AI-palette comment (a prose line ended
    in `;`, read as a statement) — reworded to drop the trailing `;`. (3) Rebased onto current `develop`
    (`d6e39900`) — the branch was 7 days behind. All three were real reds; no override label used.
  - **Perf** — zero hot-path delta. Color-literal-only change inside `ApplyModernDark` (runs once per
    theme-apply, never per frame); identical assignment count + codegen shape, different constants.
  - **Lint** — `Source/Core/src/Ui/` is the Light/ungated zone; no strict-zone file touched.
  - **Visual-validation** — **PENDING (ship-loop exception 5).** Touches `SmatchetTheme.cpp`; the launched exe
    (which reflects both the SmatchetDark default + this ModernDark change) is surfaced for eyeball
    verification. ModernDark is opt-in (Settings ▸ Appearance), so verify by switching to it.
    Exe: `C:\Dev\trees\mobile-p1.6\build\ninja-iter-msvc\Smatchet.exe`.
- **P1.4 cross-platform thumbnail decode (2026-06-27):**
  - **Unit test (pure downscale seam)** — `tests/Core/RgbaDownscalePure.test.cpp` built into `SmatchetTests`
    (`ninja-test-msvc`) and run: `SmatchetTests.exe --test-case="*DownscaleRgba32*,*FitWithinLongestSide*"`
    → **11 cases / 46 assertions, 0 failed (SUCCESS)**. Pins the longest-side fit math (aspect preserved,
    minor-side clamp ≥1), the already-fits / `maxDimension<=0` passthrough, box (area) averaging vs point
    sampling (2×2→1×1 and 4×2→2×1 exact means), and the bad-input guards (non-positive dims, short buffer).
  - **Build (Windows)** — the doctest rig compiles only the WIC `_WIN32` arm of the decode TU; it built green
    (`SmatchetTests` linked), exercising the Windows decode path + the pure helper.
  - **Build (Android — the arm the Windows rig can't see)** — to verify the previously-unexercised stb `#else`
    arm and the now-always-compiled enable block on the real target, configured + built the NDK x86_64 target
    directly (`cmake … -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-24 …
    -DSMATCHET_ANDROID_OPENSSL_BASE="C:/Android/openssl-android"`, the mobile-CI light-feature flags; Clang
    17.0.2, C++14, `-Wall -Wextra -Wpedantic`). `SmatchetAttachmentPreviewUi.cpp.o` compiled clean and
    `libSmatchetMobile.so` **linked** — proving the stb arm + `RgbaDownscalePure` compile *and* that the
    `stbi_*` symbols resolve via the single `STB_IMAGE_IMPLEMENTATION` in `SmatchetImageTextureCache.cpp`
    (same Core lib). The only build warning was pre-existing/unrelated (`g_openFilePathsHandlerInstalled`,
    `SmatchetUI.cpp:70`).
  - **Perf-gate (mandatory — touches `Source/Core/`)** — **zero UI-thread steady-state delta on every
    platform.** The decode (WIC on Windows, stb+downscale elsewhere) runs **off the UI thread** on the
    existing S5 worker-pool path (`MaybeKickThumbnailDecode` → `app.LaunchBackgroundTask`; the upload is
    posted back via `mainThreadDispatcher.PostToMainThread`), is rate-limited to
    `kMaxConcurrentThumbnailDecodes = 4`, and runs **once per attachment** (cached after upload) — never on a
    per-frame path, so Pillar-1 (≤6.94 ms steady-state) and Pillar-2 (no >100 ms UI freeze) both hold by
    construction. The new `DownscaleRgba32` is an O(srcW·srcH) box-average bounded by the 16 MP pre-decode
    cap, executed on that worker, not the frame thread. Windows codegen is **unchanged** (WIC arm untouched)
    → zero desktop regression. **Memory note (the WIC-vs-stb asymmetry):** WIC decode-scales so it never
    materialises a multi-megapixel buffer; the stb arm transiently holds full-res RGBA (capped at 16 MP ≈
    64 MiB per in-flight decode, ≤4 concurrent) before downscaling to ≤2048 px — a bounded worker-thread
    transient, not a steady-state or UI-thread cost. No mobile perf scenario exists yet (same gap as
    P1.2/P1.3/P1.5); CI **Perf PR-fast** is the authoritative headless gate.
  - **Lint** — `agents/scripts/project/test-lint-rules.sh --diff origin/develop` green after the
    `rule=duplication` exemption for the idiomatic ImGui-localization preamble (see § Deviations); no
    strict-zone file touched (`Source/Core/src/Ui/` is the Light/ungated zone).
  - **Emulator (on-device thumbnail render)** — confirming a real bitmap attachment renders its decoded
    thumbnail on `emulator-5554` is a documented follow-up bundled with the bucket-E automation backlog (the
    Mesa-GL bucket-C/E lane is CI-blocked today); taps pinned `-s emulator-5554`, never coordinate-inject the
    physical device. The decode + downscale correctness is covered by the pure unit test above; the build
    proof confirms the arm compiles + links on the real target.
- **P1.4 CodeRabbit round on #1572 — fail-closed decode budget (`9247fc6e`, 2026-06-27):**
  - **Build** — rebuilt the exact object `CMakeFiles/SmatchetCore_DX12.dir/.../SmatchetAttachmentPreviewUi.cpp.obj`
    (the doctest `SmatchetTests` target does NOT compile this TU; the flag is an in-file `#define`, not a
    `-D`) → compiled clean.
  - **Lint** — `test-lint-rules.sh --diff origin/develop` **PASS** (no new oversized functions / strict-zone
    / comment-noise; the only WARN is the pre-existing backlog-tracked sync-`ifstream` at line 85, untouched).
  - **Security-review** — `security-review` agent on the `9247fc6e` diff: **CLEAN**, no CRITICAL/HIGH/MEDIUM.
    Confirmed the fail-closed control flow (exactly one fall-through to `stbi_load`, only reachable after
    `infoW/H>0 && infoPixels<=16 MP`), both products done in `unsigned long long` (overflow-safe), the
    post-decode RGBA copy bounded ≤64 MiB, and `stbi_image_free(pix)` on the reject path (no leak). One P3
    awareness-only note (align `STBI_MAX_DIMENSIONS` to the area cap) deferred as codebase-wide hardening.
  - **Perf-gate (touches `Source/Core/`)** — **perf-inert.** The change adds two integer comparisons + one
    early return on the same off-UI-thread worker decode path described above; no new allocation, no per-frame
    or UI-thread work. Pillar-1/Pillar-2 hold by the same construction as the parent P1.4 entry.
