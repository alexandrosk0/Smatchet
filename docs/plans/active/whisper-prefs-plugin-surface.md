# Plan — Move the Whisper preferences page behind the plugin contract

> **Slug**: `whisper-prefs-plugin-surface` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Plan-link convention (tier-less — write ALL plan references this way)**: link other plans by the **tier-less** form `docs/plans/<slug>.md` — NOT `docs/plans/active/<slug>.md` or `docs/plans/shipped/<slug>.md`. The link gates (`test-markdown-links`) resolve a tier-less path against `active/`, `shipped/`, or `deferred/`, so the reference keeps working when a plan is archived (`git mv active → shipped`) without any ref-sweep. A **tiered** link passes the local check while the target still lives in `active/`, then 404s on CI after the archival move — the `markdown-links-local-passes-ci-fails-after-plan-archive` trap; `test-markdown-links.sh --merge-tree-warn` flags tiered links before they rot.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

**DRAFT. Awaiting the owner's accept or reject. No implementation is authorised by this plan.**

Today `Source/Core/src/Ui/**` and `Source/Core/src/SmatchetWhisperSetupBanner.cpp` include headers that live in `Source/Plugins/Whisper/`. Core UI therefore knows the Whisper plugin's types. The Whisper preferences page (`SmatchetPreferencesUi_Whisper.cpp`, 943 lines) and the first-run banner (`SmatchetWhisperSetupBanner.cpp`, 366 lines) are plugin code that sits in core.

After this lands: core UI includes no `Source/Plugins/Whisper/` header. The page and banner live in the plugin. Core reaches them through a capability query on `IPlugin`. The MCP-gated `TryGetMcpStatusSnapshot` virtual moves onto the same mechanism. That closes the dormant-watch debt entry `docs/self-improvement/categories/debt/2026-08-29-iplugin-mcp-accessor-dormant-watch.md`, whose trigger is "a second plugin-type accessor lands on `IPlugin`".

"Core UI" here means the UI TUs. Three other core-side files include plugin headers and are not UI. All three stay:

- `Source/Core/src/Commands/Scenarios/WhisperDictationScenario.cpp` (lines 36-37) and `WhisperAiAssistantAutosendScenario.cpp` (line 46) use the `WhisperPlugin::SetMockTranscription` test seam. They are a follow-up (Open question 1).
- `Source/Standalone/StandaloneAppBootstrap.cpp:83` is the composition root that registers the plugin.

### Measured include inventory

Checked with `git grep` over every header in `Source/Plugins/Whisper/`, searching `Source/` (tests under `tests/` also include these headers and are not in scope).

| Core file | Plugin headers included | In scope |
|---|---|---|
| `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp` (943 lines; whole TU under `#if defined(SMATCHET_WITH_WHISPER)`) | HotkeyParse, ModelCatalog, ModelDownloader, WavWriter, WhisperApiClient, WhisperApiKeyResolve, WhisperConsentGate, WhisperLocal, WhisperPlugin, WindowsAudioCapture (10), plus the core-private `SmatchetWhisperSetupBanner.h` | Yes: moves into the plugin |
| `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` lines 76-91 (`#if defined(SMATCHET_WITH_WHISPER)` block) | the same 10 (ModelCatalog appears twice), plus `SmatchetWhisperSetupBanner.h`, `MainThreadDispatcher.h`, `<cpr/cpr.h>` | Yes: dead includes. A grep of the TU for the plugin symbols and `cpr::` outside `#include` lines returns nothing. The only gated use is the `DrawWhisperPreferencesTab(app, d)` call at lines 1669-1671. |
| `Source/Core/src/SmatchetWhisperSetupBanner.cpp` (366 lines; added to `CORE_SOURCES` only when Whisper is ON, `Source/Core/CMakeLists.txt` lines 128 and 143) | ModelCatalog, ModelDownloader, WhisperConsentGate | Yes: moves into the plugin. It owns `BannerOwnedDownloader()`, which the page polls (`SmatchetPreferencesUi_Whisper.cpp:175`), so the two move together or banner first. |
| `Source/Core/src/Commands/Scenarios/WhisperDictationScenario.cpp:36-37`, `WhisperAiAssistantAutosendScenario.cpp:46` | WhisperConsentGate, WhisperPlugin | No: Commands, test seam |
| `Source/Standalone/StandaloneAppBootstrap.cpp:83` | WhisperPlugin | No: composition root |
| `Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp`, `Source/Core/src/DictationInsertionRouter_Whisper.cpp` | none | No |

### Measured contract today

- `IPlugin` (`Source/Core/include/IPlugin.h`): `Id`, `OnEarlyInit`, `OnStart`, `OnDraw`, `OnStop`, `NeedsRestart`, plus the `#if defined(SMATCHET_WITH_MCP)` blocks (include at lines 3-5, virtual at lines 34-40) holding `TryGetMcpStatusSnapshot`.
- `PluginHost` (`Source/Core/include/PluginHost.h`): `Register`, the lifecycle fan-out, and the MCP-gated `SyncMcpPluginWithConfig` and `GetMcpServerStatus`.
- UI reaches the host through `AppController::RuntimePluginHost()` (`AppController.h:494`), set in `StandaloneAppBootstrap.cpp:386` and `SmatchetImGuiHost.cpp:605`. The DX12 host registers only `McpPlugin` and `LuaConsolePlugin` (`SmatchetImGuiHost.cpp:539,547`), never Whisper.
- The banner sits in `SmatchetUI::drawChromeAndModeToggles` (`SmatchetUI.cpp:935-953`), after the toolbar ("before the dock space" is unverified). `PluginHost::OnDraw` runs after `mainWindow.Draw` (`StandaloneAppBootstrap.cpp:164-168`). The banner therefore cannot move into `OnDraw`.

## Approach

Add one query virtual to `IPlugin`: `void* QueryCapability(PluginCapabilityId)`. Each capability is a small abstract interface with a static `CapabilityId()`. The plugin implements capabilities by multiple inheritance and returns `static_cast<Cap*>(this)`. A typed helper `QueryPluginCapability<Cap>(IPlugin&)` wraps the cast.

Three capabilities: `McpStatus` (migrates the existing virtual), `PreferencesPage` (a category-scoped, ordered page body) and `MainWindowBanner` (the pre-dock-space slot). `IPlugin.h` loses its only `#if`, so its vtable is identical across feature-flag states.

The page needs the preferences section chrome (`PrefsSectionBegin` / `PrefsSectionEnd` / `PrefsSection`). Those move from the private `Source/Core/src/Ui/SmatchetPreferencesUi_detail.h` to a public ImGui-free header. Then the banner and the page move into the plugin with `git mv`, one commit per move, so the duplication delta-gate sees a move and not a clone. Doing nothing is a real option and costs nothing now: the debt entry is a P3 watch with "No action while the count stays at one.", and the Whisper page works today. The case for acting is layering (core UI stops knowing plugin types) and a flag-independent `IPlugin` vtable; the cost is 28 rows across four slices, a new public header surface, and Windows-only validation of S2-S4 (§ Risks). Trade-off in one sentence: a query virtual costs one indirect call per plugin per frame, and in return every future plugin surface (menus, dock panes) adds a capability and not a vtable slot on every plugin.

## Interface contracts

### New `Source/Core/include/PluginCapabilities.h` (ImGui-free, GLFW-free, no feature gates)

```cpp
#pragma once
class IAppThreading; struct UiDrawSession; struct TrackerConfig; class IPlugin;

/// Stable numeric ids. Append-only; never renumber (future DLL ABI).
enum class PluginCapabilityId : unsigned int {
    McpStatus = 1,
    PreferencesPage = 2,
    MainWindowBanner = 3,
};

/// Preferences right-pane contribution. UI thread only.
class IPluginPreferencesPage {
  public:
    static PluginCapabilityId CapabilityId() { return PluginCapabilityId::PreferencesPage; }
    /// SmatchetPrefsSchema category id this page draws into ("ai_voice"). Must name a
    /// category kCategories already compiles in; an unknown id is logged once and skipped.
    virtual const char* PreferencesCategoryId() const = 0;
    /// Ascending sort key within the category. Core-owned sections draw first
    /// (implicit order 0..99); plugins use >= 100. Ties break on IPlugin::Id().
    virtual int PreferencesOrder() const = 0;
    /// Draws the page body: one or more PrefsSection(d, "<schema section id>", ...)
    /// blocks. Must not block (Pillar 2: background work via app.LaunchBackgroundTask
    /// + PostToMainThread) and must not throw.
    virtual void DrawPreferencesPage(IAppThreading& app, UiDrawSession& d) = 0;
  protected:
    ~IPluginPreferencesPage() = default; // never deleted through the capability
};

/// Pre-dock-space banner slot under the menu bar / toolbar. UI thread only.
class IPluginMainWindowBanner {
  public:
    static PluginCapabilityId CapabilityId() { return PluginCapabilityId::MainWindowBanner; }
    /// Draws the banner when the plugin wants it (the plugin owns the visibility test,
    /// e.g. !cfg.WhisperSetupCompleted). Returns true when cfg changed and the caller
    /// must ConfigManager::Save(cfg).
    virtual bool DrawMainWindowBanner(IAppThreading& app, TrackerConfig& cfg) = 0;
  protected:
    ~IPluginMainWindowBanner() = default;
};

/// Typed query. nullptr when the plugin lacks the capability.
template <typename Cap> Cap* QueryPluginCapability(IPlugin& p); // defined in IPlugin.h after the class
```

### `Source/Core/include/IPlugin.h` delta

- Remove the `#if defined(SMATCHET_WITH_MCP)` include of `McpServerStatus.h` and the `TryGetMcpStatusSnapshot` virtual.
- Add as the last virtual, after `NeedsRestart`:

```cpp
/** Capability query (COM-style, RTTI-free, DLL-stable). Return static_cast<Cap*>(this)
 *  for each implemented Cap whose Cap::CapabilityId() == id, else nullptr. Must be pure:
 *  no allocation, no locks, same answer for the plugin's whole lifetime. */
virtual void* QueryCapability(PluginCapabilityId /*id*/) { return nullptr; }
```

- Add after the class:

```cpp
template <typename Cap> inline Cap* QueryPluginCapability(IPlugin& p) {
    return static_cast<Cap*>(p.QueryCapability(Cap::CapabilityId()));
}
```

- Invariant: the `void*` must come from exactly `Cap*`. Implementers write `return static_cast<IPluginPreferencesPage*>(this);`, never `return this;`. Under multiple inheritance a `void*` taken from `this` points at the wrong subobject.

### New `Source/Core/include/PluginMcpStatusCapability.h` (included only from MCP-gated TUs)

```cpp
#pragma once
#include "McpServerStatus.h"
#include "PluginCapabilities.h"
class IPluginMcpStatus {
  public:
    static PluginCapabilityId CapabilityId() { return PluginCapabilityId::McpStatus; }
    virtual bool TryGetMcpStatusSnapshot(McpServerStatus& out) const = 0;
  protected:
    ~IPluginMcpStatus() = default;
};
```

### MCP accessor migration (the chokepoint)

Zero callers use `TryGetMcpStatusSnapshot` directly outside the host. The one consumer is `PluginHost::GetMcpServerStatus` (`Source/Core/src/PluginHost.cpp:80-89`), reached from `SmatchetMcpServerUi.cpp:223` and `tests/ui/ai_prefs_autosave_flow.test.cpp:410,436`. The change site is `PluginHost.cpp`: the loop calls `QueryPluginCapability<IPluginMcpStatus>(*plugins_[i])`. The public signature of `GetMcpServerStatus` is unchanged, so no UI or test edit is needed.

`Source/Plugins/Mcp/McpPlugin.h:29`: the class becomes `: public IPlugin, public IPluginMcpStatus`, with a `QueryCapability` override. The existing `override` on `TryGetMcpStatusSnapshot` stays valid because it now overrides the capability's pure virtual.

### `Source/Core/include/PluginHost.h` delta (not feature-gated)

```cpp
/// UI thread. Draws every registered IPluginPreferencesPage whose category matches, in
/// (PreferencesOrder, Id) order. Catches + LOG_ERRORs a throwing page and disables that
/// page (by plugin Id) for the session. No-op with zero plugins.
void DrawPreferencesPages(IAppThreading& app, UiDrawSession& d, const char* categoryId);
/// UI thread. Draws every IPluginMainWindowBanner in registration order; returns true
/// when any banner asked for a save.
bool DrawMainWindowBanners(IAppThreading& app, TrackerConfig& cfg);
```

- Private state: `std::vector<std::string> disabledPages_` (plugin Ids).
- A static pure helper `PluginHost::SortPreferencesPages(std::vector<std::pair<const IPlugin*, IPluginPreferencesPage*>>&)` (stable sort by order, then `strcmp` on `Id()`), declared in the header so a doctest can drive fake plugins.

### Plumbing, hop by hop

- Page: `SmatchetUI::drawPreferencesCategoryBody(AppController& app, UiDrawSession& d)` (`SmatchetPreferencesUi.cpp:1649`) → `app.RuntimePluginHost()` (`PluginHost*`, may be null) → `DrawPreferencesPages(app, d, "<category id>")` (`AppController` derives from `IAppThreading`, `AppController.h:176`) → `IPluginPreferencesPage::DrawPreferencesPage(IAppThreading&, UiDrawSession&)`. Every hop exists, and no typedef widens.
- Banner: `SmatchetUI::drawChromeAndModeToggles` (has `app`, `d.cfg`) → `RuntimePluginHost()->DrawMainWindowBanners(app, d.cfg)`. The arity matches the existing `banner::Render(IAppThreading&, TrackerConfig&)`.

### Discovery, ordering, lifetime, threading

- **Discovery.** Re-queried every frame the Preferences window is open (banners: every frame). Never cached. `SyncMcpPluginWithConfig` can remove a plugin mid-session, and a cached capability pointer would dangle. Cost: one virtual call per plugin per frame.
- **Lifetime.** A capability pointer is borrowed. It is valid only for the call that obtained it.
- **Ordering.** Core sections for a category draw first. In `AiVoice` that is the Assistant tab under `#if defined(SMATCHET_WITH_AI)`. Then `DrawPreferencesPages(app, d, "ai_voice")` runs. Every case in `drawPreferencesCategoryBody` ends with a `DrawPreferencesPages` call for its category id, so a future plugin can join any category.
- **Schema.** `PreferencesSchema.cpp` (`#if defined(SMATCHET_WITH_WHISPER)` at lines 53 and 263) and the nav rail (`SmatchetPreferencesUi_Shell.cpp:180`) keep their static gates. The plugin draws rows whose descriptors core already compiles in. Runtime schema registration is out of scope (Open question 3).
- **Threading.** All capability calls run on the UI thread. Plugin pages start background work only through `IAppThreading::LaunchBackgroundTask` and marshal results back with `PostToMainThread`, as `SmatchetPreferencesUi_Whisper.cpp` already does.

### New `Source/Core/include/Ui/PreferencesSectionChrome.h` (promoted, ImGui-free)

- Holds `namespace SmatchetPreferencesUiDetail { bool PrefsSectionBegin(UiDrawSession&, const char*); void PrefsSectionEnd(UiDrawSession&, const char*); template<class BodyFn> void PrefsSection(...); }`. The declarations and the template move verbatim from `Source/Core/src/Ui/SmatchetPreferencesUi_detail.h:39-47`.
- The definitions stay in `SmatchetPreferencesUi_Shell.cpp:123`.
- `_detail.h` includes the new header. The namespace is unchanged, so no call site changes.

### Shared helpers the plugin needs from core

Verified to sit on `SmatchetCoreInterface`'s include path and link line (`Source/Core/CMakeLists.txt`: `Source/Core/ThirdParty/IconsFontAwesome6` among the INTERFACE include directories, `cpr::cpr` in the INTERFACE link list), which the plugin links PUBLIC:

- `Ui/PreferencesSectionChrome.h` (new), `Ui/SmatchetUiSession.h` (`UiDrawSession`, `d.cfg`, `MarkPrefsDirty` at line 1434), `Commands/IAppThreading.h`, `SmatchetLocalizedImGui.h`, `Ui/SmatchetHelpMarker.h`, `Ui/SmatchetIconButtons.h`, `Ui/SmatchetSecretInput.h`, `Ui/SmatchetToast.h` (the page includes it; the architect report omitted it), `MainThreadDispatcher.h`, `UiPerfMonitor.h` (defines `SMATCHET_UI_PERF_SCOPE`), `IconsFontAwesome6.h`, `<cpr/cpr.h>`.
- `SmatchetLocalization.h` and `Logger.h` (both under `Source/Core/include/`) and `ConfigManager.h` (`Source/Core/include/Config/`, reachable bare because `Source/Core/include/Config` is on the interface include path).

The plugin TUs must NOT include `AppController.h`, so the `app-controller-fan-in` ratchet holds.

## Files to modify

Grouped by slice. Anchor for the dual-target build (this diff touches `SMATCHET_WITH_WHISPER` source-list gating): `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`, anchored to `Source/Core/CMakeLists.txt` lines 128 and 143 and the include comment at lines 337-340, plus `PluginHost.cpp`, which the DX12 target also compiles.

Pre-seal probes run: every existing path below was confirmed present with `ls` / `git grep`; every "new" path was confirmed absent (`Source/Plugins/Whisper/` holds no `AGENTS.md`, no `WhisperSetupBanner.*`, no `WhisperPreferencesPage.cpp`).

**S1 — capability contract + MCP migration**

1. `Source/Core/include/PluginCapabilities.h` — new.
2. `Source/Core/include/PluginMcpStatusCapability.h` — new.
3. `Source/Core/include/IPlugin.h` — drop the gated include and virtual; add `QueryCapability` and the template helper.
4. `Source/Core/include/PluginHost.h`, `Source/Core/src/PluginHost.cpp` — add `DrawPreferencesPages`, `DrawMainWindowBanners`, `SortPreferencesPages`; `GetMcpServerStatus` queries the capability.
5. `Source/Plugins/Mcp/McpPlugin.h` — inherit `IPluginMcpStatus`, add `QueryCapability`.
6. `tests/Core/PluginCapabilities.test.cpp` — new bucket-A doctest; add it to `add_executable(SmatchetTests …)` in `tests/CMakeLists.txt` (configure FATALs on an unlisted test).
7. `docs/self-improvement/categories/debt/2026-08-29-iplugin-mcp-accessor-dormant-watch.md` — archive to `docs/self-improvement/categories/applied.md`.

**S2 — core UI seams (no behaviour change)**

8. `Source/Core/include/Ui/PreferencesSectionChrome.h` — new (promoted declarations).
9. `Source/Core/src/Ui/SmatchetPreferencesUi_detail.h` — include the new header; drop the moved declarations.
10. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` — delete the dead include block (lines 76-91); add an unconditional `#include "PluginHost.h"` (today it is included only under `SMATCHET_WITH_MCP`, so the MCP-OFF build would not compile the new call); add a `DrawPreferencesPages` call at the end of each case of `drawPreferencesCategoryBody`.
11. `Source/Core/src/Ui/SmatchetUI.cpp` — add an unconditional `#include "PluginHost.h"` if absent, and the generic `DrawMainWindowBanners` call beside the existing direct Whisper call (the direct call stays until S3). The `ZenMode` test stays in core and suppresses all banners.
12. `tests/ui/funcsize_preferences_tabs.test.cpp` — add the Whisper "Voice dictation" section assertion now, so it guards S4.

**S3 — banner move**

13. `Source/Core/src/SmatchetWhisperSetupBanner.{h,cpp}` → `git mv` to `Source/Plugins/Whisper/WhisperSetupBanner.{h,cpp}`.
14. `Source/Plugins/Whisper/WhisperPlugin.{h,cpp}` — implement `IPluginMainWindowBanner`; the `!WhisperSetupCompleted` test moves here.
15. `Source/Plugins/Whisper/CMakeLists.txt` — add `WhisperSetupBanner.cpp` to `SmatchetPlugin_Whisper`.
16. `Source/Core/CMakeLists.txt` — remove the banner `REMOVE_ITEM` (line 128) and `APPEND` (line 143).
17. `Source/Core/src/Ui/SmatchetUI.cpp` — delete the direct banner call (lines 944-953 region) and the `SmatchetWhisperSetupBanner.h` include (line 59). Keep the `SMATCHET_UI_PERF_SCOPE("SmatchetWhisperSetupBanner::Render")` literal byte-identical.
18. `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp` — repoint its banner include. Reachable only while that TU is still in core, through the core PRIVATE include of `Source/Plugins/Whisper` (`Source/Core/CMakeLists.txt` line 340); acceptable for one slice.
19. `Source/Plugins/Whisper/AGENTS.md` — new leaf doc; add a `CONTEXT-MAP.md` row (the registry row for `Mcp (plugin)` is at `CONTEXT-MAP.md:23`, model the new row on it).
20. `tests/ui/whisper_setup_banner.test.cpp` — new bucket-E test; register in `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp`.
21. `docs/perf/MARKER_INVENTORY.md` — regenerate; its row for this marker is anchored to `SmatchetUI.cpp:923`.

**S4 — preferences page move**

22. `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp` → `git mv` to `Source/Plugins/Whisper/WhisperPreferencesPage.cpp`; entry `DrawWhisperPreferencesTab` (line 900) becomes `smatchet::whisper::prefs::DrawPage(WhisperPlugin&, IAppThreading&, UiDrawSession&)`; `WhisperPlugin::InstanceForUi()` at line 284 is replaced by the passed `WhisperPlugin&`.
23. `Source/Plugins/Whisper/WhisperPlugin.{h,cpp}` — implement `IPluginPreferencesPage` (`"ai_voice"`, order `100`).
24. `Source/Plugins/Whisper/CMakeLists.txt` — add `WhisperPreferencesPage.cpp`.
25. `Source/Core/src/Ui/SmatchetPreferencesUi_detail.h` — drop the `DrawWhisperPreferencesTab` declaration (line 342).
26. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp` — drop the call at lines 1669-1671.
27. `tests/ui/funcsize_preferences_tabs.test.cpp:8` — comment path.
28. `Source/Core/CMakeLists.txt` lines 337-340 — rewrite the include-dir comment to name only the two Scenarios. The `target_include_directories` line stays for them.

### File-level changes

| Symbol | CMake variable |
|---|---|
| `TryGetMcpStatusSnapshot` | `CORE_SOURCES` |
| `GetMcpServerStatus` | `SMATCHET_WITH_MCP` |
| `DrawWhisperPreferencesTab` | |
| `BannerOwnedDownloader` | |
| `InstanceForUi` | |
| `RuntimePluginHost` | |
| `PrefsSectionBegin` | |
| `SyncMcpPluginWithConfig` | |

## Existing utilities reused

- `PluginHost::Register` and the `plugins_` vector (`PluginHost.h`) — the registry the capability queries iterate.
- `AppController::RuntimePluginHost()` (`AppController.h:494`) — how UI already reaches the host.
- `SmatchetPreferencesUiDetail::PrefsSection` / `PrefsSectionBegin` / `PrefsSectionEnd` (`SmatchetPreferencesUi_detail.h:39-47`, defined in `SmatchetPreferencesUi_Shell.cpp:123`) — section chrome, promoted not rewritten.
- `IAppThreading::LaunchBackgroundTask` / `PostToMainThread` — already the page's threading idiom.
- `smatchet::whisper::banner::Render(IAppThreading&, TrackerConfig&)` (`SmatchetWhisperSetupBanner.h`) — same arity as the new banner capability.
- `McpPlugin::TryGetMcpStatusSnapshot` (`Source/Plugins/Mcp/McpPlugin.h:29`) — body reused as the capability implementation.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan moves two TUs between subsystems and splits no over-cap prompt, `AGENTS.md` section or TU. The moved TUs keep their size (943 and 366 lines), so `function-too-long` and `tu-line-ceiling` are unchanged by a pure move. `Source/Plugins/Whisper/AGENTS.md` is new and far below the 150-line cap.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: one `QueryCapability` virtual call per registered plugin per frame for banners (at most 3 plugins today), and the same per frame while Preferences is open. No allocation on the hot path. The perf-scope literal is preserved.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no new sync I/O. The moved page keeps its `LaunchBackgroundTask` + `PostToMainThread` pattern. The capability contract forbids blocking in `DrawPreferencesPage`.
- **Pillar 3 (never crash)**: `DrawPreferencesPages` catches a throwing page, logs it, and disables that page for the session. The ImGui ID stack may be left unbalanced after such a throw; recovery is best-effort and accepted. The wrong-subobject `void*` hazard is covered by a bucket-A test.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no behaviour change; widgets, labels and localization keys move unchanged. **Visual-validation exception applies**: S3 and S4 delete or move `Smatchet*Ui*.cpp` files (`SmatchetPreferencesUi_Whisper.cpp`, and the `SmatchetUI.cpp` banner block). Unless bucket-C or bucket-E coverage lifts it (see Verification), the orchestrator pauses with a launched exe and awaits the user's verdict ([`ship-loops.md`](../../../agent-layer/docs/agent-rules/ship-loops.md) § Visual-validation exception).
- **Pillar 5 (DRY)**: each move is one `git mv` commit so `dup_audit.py` sees a move.
- **Pillar 6 (offline-first)**: no cache, queue or `TrackerError` path is touched.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

The diff touches `Source/Core/include/`, `Source/Core/src/PluginHost.cpp` and `Source/Core/src/Ui/`, so the section applies. Gate state:

1. **PR-fast CI** — fires on `windows-msvc`. The nearest mapped scenario for the preferences UI is `preferences-slider-drag` (glob `Source/Core/src/Ui/SmatchetPreferencesUi*.cpp` in [`scripts/dev/perf-scenario-map.json`](../../../scripts/dev/perf-scenario-map.json)), but it is not in `scripts/dev/perf-pr-fast-set.json`. `idle` is in the set and draws the banner slot every frame, so it is the PR-fast signal for the new per-frame query. Note: once the page moves to `Source/Plugins/Whisper/`, the `SmatchetPreferencesUi*.cpp` glob no longer matches it; whether to add a `Source/Plugins/Whisper/` glob to the map is an Open question. No glob in the map names `SmatchetUI.cpp`, so a banner-only edit maps to no scenario.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`. The moved page keeps its existing worker-thread annotations unchanged; the S4 implementer re-runs the scanner on the moved file to confirm the annotations travel with it.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` is not touched.
4. **Visible-cue bucket-E harness** — N/A: no new sync-stall code path.
5. **Marker inventory** — the `SMATCHET_UI_PERF_SCOPE("SmatchetWhisperSetupBanner::Render")` literal is kept, but its source line changes (inventory row `SmatchetUI.cpp:923`). Regenerate `docs/perf/MARKER_INVENTORY.md` in the S3 commit.

**Pre-push local check**: run `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `idle` before opening the PR.

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — not expected.

## Risks / non-goals

- **Persisted formats.** None change: config keys, schema section and row ids, localization keys and the SQLite cache are untouched. Accepted.
- **Vtable shape.** `IPlugin` gains a slot and loses a gated one. Plugins are static libraries today (`docs/CONTEXT.md` § Static plugin), so there is no ABI break, and flag-independence improves the future DLL story. Mitigation: append-only enum and slot order documented in the header.
- **Wrong-subobject `void*`.** Bucket-A test with a two-base fake plugin.
- **Exception in a plugin page unbalances the ImGui ID stack.** Host catches, `LOG_ERROR`s and disables that page for the session. Accepted as best-effort.
- **Dual-target build.** `SmatchetCore_DX12` compiles `PluginHost.cpp` and the new headers. Keep them ImGui-free and GLFW-free (`no-glfw-in-core-headers`). The DX12 build keeps `SMATCHET_WITH_WHISPER` OFF, so only the Whisper-OFF path is compiled there.
- **Shim discipline (ADR-0002).** No new `SMATCHET_WITH_*` gate is introduced, so no new shim is needed. The plugin already links the AI and MCP shims when those flags are on (`Source/Plugins/Whisper/CMakeLists.txt`) and gets `UiDrawSession` layout from `SmatchetCoreInterface`.
- **Lint.** `Source/Plugins/Whisper/` is not a strict zone (the strict plugin zone is `Source/Plugins/Mcp/`). `PluginHost.cpp` is under `Source/Core/src/`, not a strict zone; `McpPlugin.h` is, so S1 follows `Source/Plugins/Mcp/AGENTS.md`.
- **Stale glob.** `CORE_SOURCES` is glob-built; the build dir must be re-configured after S4. CI configures fresh.
- **Validation limit.** The ON path is Windows-only (WASAPI, Win32 hotkey). `ninja-test-linux` builds only the curated ImGui-free `SmatchetTsanTests` subset; `SmatchetTests` links ImGuiLib and is Windows-only (`tests/CMakeLists.txt` comments). Whether S1's doctest can join the Linux subset is unverified. S2-S4 are validated by the Windows CI lanes (Open question 5 may narrow this).
- **Non-goals:** the overlay (`SmatchetWhisperOverlayUi.cpp`, no plugin include); the two Whisper Scenarios; plugin-registered schema/nav rows; moving `DictationInsertionRouter_*`; an enforcing lint gate.

## Implementation handoff

One feature PR on one branch, four slices. Every slice must build with Whisper ON (`ninja-iter-msvc`) and OFF (`ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF -DSMATCHET_WITH_AI=OFF -DSMATCHET_WITH_MCP=OFF`). Run `cmake --build` and `scripts/dev/test-all.sh` once per slice.

**S1 — capability contract + MCP migration** (order: first)
- **Owner**: `Source/Core/include` and `PluginHost`, plus `Source/Plugins/Mcp` (read `Source/Plugins/Mcp/AGENTS.md` first).
- **Why one slice**: deleting the `IPlugin` virtual breaks `McpPlugin.h`'s override, so both must land atomically for MCP-ON builds.
- **Write set**: files 1-7 above.
- **Invariants**: enum values 1/2/3 are fixed; `QueryCapability` is the last vtable slot; `static_cast<Cap*>(this)` before the `void*` conversion; no `#if` in `IPlugin.h`; new headers are ImGui-free and GLFW-free.
- **Inline inventory**: literals `"ai_voice"`, `TryGetMcpStatusSnapshot` (`IPlugin.h:39`, `McpPlugin.h:29`, `PluginHost.cpp:89`), `GetMcpServerStatus` (`PluginHost.h:25`, `PluginHost.cpp:80`, `SmatchetMcpServerUi.cpp:223`, `ai_prefs_autosave_flow.test.cpp:410,436`).

**S2 — core UI seams** (order: after S1)
- **Owner**: `Source/Core/src/Ui` (read `Source/Core/src/Ui/AGENTS.md` and `docs/guides/imgui-draw-pattern.md`; both exist).
- **Write set**: files 8-12.
- **Invariants**: namespace stays `SmatchetPreferencesUiDetail`; a null `RuntimePluginHost()` means no plugin pages; `ZenMode` suppresses all banners in core. No behaviour change: no plugin implements the new capabilities yet.

**S3 — banner move** (order: after S2)
- **Owner**: `Source/Plugins/Whisper` (creates its leaf `AGENTS.md` and the `CONTEXT-MAP.md` row).
- **Why it spans core**: the core-side removals (`Source/Core/CMakeLists.txt` banner lines, `SmatchetUI.cpp` direct call and include) must land in the same commit as the add, or `BannerOwnedDownloader` / `banner::Render` get duplicate or missing symbols.
- **Write set**: files 13-21.
- **Invariants**: the downloader stays a function-local static owned by the banner TU; localization keys unchanged; the perf-scope literal is byte-identical.

**S4 — preferences page move** (order: last)
- **Owner**: `Source/Plugins/Whisper`, with `src/Ui` deletions in the same commit (the TU is globbed into `CORE_SOURCES`, so re-configure after the move).
- **Write set**: files 22-28.
- **Invariants**: section ids `ai_voice.dictation` and `ai_voice.diagnostics` and every `ai_voice.dictation.*` / `ai_voice.diagnostics.*` `ShowSetting` id unchanged (the schema stays in core); every `whisper.preferences.*` localization key unchanged; `WhisperPrefsTabState` stays a TU-local static; no `AppController.h` include; `#define ImGui SmatchetLocalizedImGui` stays. The whole-TU `#if defined(SMATCHET_WITH_WHISPER)` guard may be dropped because the plugin subtree is CMake-gated.
- **Done-check**: `git grep -nE '#include "(HotkeyParse|ModelCatalog|ModelDownloader|ModelDownloadPolicy|WavWriter|WhisperApiClient|WhisperApiKeyResolve|WhisperConsentGate|WhisperLocal|WhisperPlugin|WindowsAudioCapture|GlobalHotkey_Win32|SilenceTrim|WhisperSetupBanner)\.h"' -- Source/Core/src/Ui Source/Core/src/*.cpp` returns zero hits. Only the two Scenarios remain under `Source/Core`.

Each slice stays inside one subsystem except S3/S4, whose core-side removals must stay in the same commit as the add (reason above).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**: new `tests/Core/PluginCapabilities.test.cpp` in `SmatchetTests`: `QueryPluginCapability<T>` returns the right subobject under multiple inheritance; nullptr default; `SortPreferencesPages` order and Id tie-break; `GetMcpServerStatus` via a fake `IPluginMcpStatus`. It runs in `SmatchetTests` on Windows. Whether it can also join the Linux `SmatchetTsanTests` subset (`ninja-test-linux`) is unverified; the `GetMcpServerStatus` case needs `#if defined(SMATCHET_WITH_MCP)`.
- **Bucket A (build)**: feature-OFF compile (Whisper/AI/MCP off) on `windows-msvc-light`, `mobile-posix-core-check`, `mobile-android-ndk`, the sanitizer lanes with Whisper OFF, `packaging-graph-smoke` (DX12), and the ARM64 preset (`SMATCHET_WITH_WHISPER=OFF` in `CMakePresets.json`); feature-ON compile on `windows-msvc`. Lane names are from the architect report; not re-verified here.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**:
  - AI & Voice draws the Whisper "Voice dictation" and "Voice diagnostics" sections via the plugin: extend `tests/ui/funcsize_preferences_tabs.test.cpp` (S2 adds it, S4 relies on it).
  - Every Whisper schema row still reachable by the prefs filter: existing `tests/ui/prefs_schema_coverage.test.cpp`.
  - MCP status still surfaced: existing `tests/ui/ai_prefs_autosave_flow.test.cpp:410,436`.
  - Banner renders when `WhisperSetupCompleted=false`, hidden in Zen and after an answer, "No thanks" sets completed: new `tests/ui/whisper_setup_banner.test.cpp` (S3).
- **Bash-driver scenario / screenshot / sanitizer**: pixel parity of the moved page and banner is bucket C (`bucket-c-screenshot-diff`) if a prefs AI & Voice scenario exists; I did not find one (unverified). Without it, the visual-validation pause fires and the bucket-E items above are the coverage that lifts it. Hotkey hot-rebind, mic test and cloud/local E2E probes (real WASAPI, Win32 hook, network) stay manual until a UI-engine test with `WhisperPlugin::SetMockTranscription` and a fake capture seam exists; file that as a bucket-E follow-up.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target), anchored to the gated files in § Files to modify.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required. Not run in this drafting task (forbidden by the task rules).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. **The grill pass is still owed before this plan is final.** It was skipped in this drafting task by instruction.
- **Manual residue**: the hardware-dependent probes above. Deferred-automation action: the bucket-E follow-up named there, plus a `docs/self-improvement/categories/tooling.md` entry when this plan is accepted.

## Open questions

1. **User / owner**: should the two `Commands/Scenarios/Whisper*Scenario.cpp` TUs move into the plugin through a new `ScenarioProvider` capability? That would let `Source/Core/CMakeLists.txt` drop its PRIVATE include of `Source/Plugins/Whisper` entirely. Or do they stay as a sanctioned test-seam exception?
2. **User (cross-repo)**: add a blocking grep gate `no-plugin-include-in-core-ui` (a quote-include from `Source/Core/src/Ui/**` of any header basename under `Source/Plugins/*/`)? The lint driver lives in the `agent-layer` submodule, so this needs owner approval.
3. **User / Ui owner**: should the `ai_voice.dictation*` / `ai_voice.diagnostics*` schema rows also move behind `IPluginPreferencesPage` (for example a `const PrefsSectionDesc* Sections(size_t&)` method)? That makes the AI & Voice rail entry depend on runtime registration instead of `#if defined(SMATCHET_WITH_WHISPER)`, and changes the `PreferencesSchema.test.cpp` / `prefs_schema_coverage` assumptions.
4. **User**: is plugin page order `100` (core sections first, Assistant above Dictation as today) the intended IA, or should order be schema-declared?
5. **Probe**: does `ninja-test-linux` build with Whisper ON? The plugin CMake says the subtree is entered on any platform and `WindowsAudioCapture.cpp` stubs off-Windows. If yes, S3/S4 compile in a Linux container and the validation limit narrows to runtime behaviour.
6. **Perf owner**: after the move, should `scripts/dev/perf-scenario-map.json` get a `Source/Plugins/Whisper/` glob so the page stays mapped to a scenario?

Corrections to the architect report, found while checking it against the tree:

- The promoted chrome declarations are at `SmatchetPreferencesUi_detail.h:39-47` (namespace opens at line 26), not 27-46. The `DrawWhisperPreferencesTab` declaration is at line 342.
- The page also includes `SmatchetToast.h` and the core-private `SmatchetWhisperSetupBanner.h`; the report's header list omitted both.
- The two "verify" items (`IconsFontAwesome6.h`, `cpr`) are resolved: both already reach the plugin through `SmatchetCoreInterface`, so no extra PRIVATE include or link is needed. `SMATCHET_UI_PERF_SCOPE` is defined in the public `Source/Core/include/UiPerfMonitor.h`.
- The report's "Perf gate: PR-fast on `windows-msvc`" names no scenario. The mapped scenario `preferences-slider-drag` is not in the PR-fast set; `idle` is.
- The `MARKER_INVENTORY.md` regeneration was missing from the report's write set.
- The banner and overlay headers are core-private (`Source/Core/src/`, `Source/Core/src/Ui/`), not on the public include path.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. Not run in this drafting task; owed at finalisation. Known candidate: the debt entry `docs/self-improvement/categories/debt/2026-08-29-iplugin-mcp-accessor-dormant-watch.md` is archived by S1, not deferred.

- The overlay (`SmatchetWhisperOverlayUi.cpp`) into `OnDraw` — no plugin include today; follow-up plan only if wanted.
- The two Whisper Scenarios moving into the plugin — Open question 1.
- The Lua console plugin adopting `IPluginPreferencesPage` — no-action until it has a preferences page.
- Plugin-registered schema and nav rows — Open question 3.
- `DictationInsertionRouter_*` — no plugin include; no-action.
- An enforcing lint gate — Open question 2.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
