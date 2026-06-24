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
| **P1.4** Attachments | ✅ **Shipped** (decode cross-platform) | `SmatchetImageTextureCache.cpp:139–157` (`DecodeWithStb`, no `_WIN32` guard) + renderer-agnostic `RegisterUserTexture`; thumbnails enabled globally. **Residual:** the *optional* Win32-only bitmap thumbnail-to-file at `SmatchetAttachmentPreviewUi.cpp:112` (low priority). |
| **P1.5** Multi-backend | 🟡 **Infra ready** | `ITrackerBackend` abstraction + Jira / Plane / GitHub all in Core; backend selection lives in the Preferences combo (`SmatchetPreferencesUi.cpp:253`). **Missing:** a touch-first backend-selection UI (depends on P1.3 chrome). |
| **P1.6** Accessibility | ✅ **Shipped this PR** (research) | [`docs/mobile/PHASE1_ACCESSIBILITY_RESEARCH.md`](../../mobile/PHASE1_ACCESSIBILITY_RESEARCH.md) + Pillar-4 backlog [`debt/2026-06-20-mobile-accessibility-pillar4.md`](../../self-improvement/categories/debt/2026-06-20-mobile-accessibility-pillar4.md). |

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
| P1.4 | Attachments | verify-on-device of `Source/Core/src/Persistence/SmatchetImageTextureCache.cpp` (existing cross-platform stb_image path); `Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp:112` only if the optional `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` thumbnail is in scope |
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
