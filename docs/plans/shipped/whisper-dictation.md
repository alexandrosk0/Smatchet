# whisper-dictation — push-to-talk dictation with local + cloud transcription

## Status: Shipped end-to-end (Phase G — 2026-05-18)

All 7 phases shipped (merged into `develop`):

| Phase | PR | sha | Summary |
|---|---|---|---|
| Plan | #206 | `d08a0bb` | Design doc with 6-layer gating discipline |
| A | #207 | `5006fcd` | Plugin shell + bindings/stubs split + 7 config fields + `WHISPER=OFF` CI sentinel |
| B | #209 | `9565b9c` | Cloud transcription (OpenAI Whisper API) + WASAPI capture + key-fallback resolver |
| C | #212 | `b6f3511` | Local backend API (whisper.cpp behind `SMATCHET_WHISPER_LOCAL_BACKEND` sub-option) + model downloader + setup banner |
| D | #213 | `1dcbeef` | Generic ImGui InputText hook + 4 insertion targets + UTF-8-safe splice |
| E | #216 | `0149c19` | Push-to-talk hotkey (`RegisterHotKey` + `WH_KEYBOARD_LL`) + mic indicator + amplitude overlay |
| F | #219 | `d91d317` | Polished Preferences tab + 4 deferred config fields + hot-rebind + ModelCatalog medium entry + auto-send-on-punctuation |
| G | #223 | `bcffa37` | End-to-end `whisper-dictation-roundtrip` scenario + mock-transcription seams + `test-whisper-roundtrip.sh` auto-enrolled gate |

**Default user flow**: first launch → non-blocking banner ("Enable voice dictation?"). User picks Enable + size → ~150 MB model downloads with progress + cancel. Subsequent launches: hold `Ctrl+Alt+Space`, speak, release → text lands at end-of-buffer in whichever ImGui InputText was focused (AI Assistant chat / grid long-text editor / Command Palette / any focused InputText via the wrapper).

**CI matrix**: `Windows + MSYS2 UCRT64` and `Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)` jobs run on every PR — bindings/stubs drift catches itself.

## Followups + open items

Tracked outside this plan; this section consolidates pointers so future agents don't have to dig through Deviations + Verification.

### Open architectural decisions

- **`SMATCHET_WHISPER_LOCAL_BACKEND` default flip (OFF → ON?)** — **RESOLVED (default is ON where Whisper builds at all)**: the `option()` in `CMakeLists.txt` now defaults `ON` (~1.37 MB static delta accepted), so the locked "local default, cloud fallback" UX holds without a build-time opt-in on Standalone. DX12/Unreal builds are unaffected — they ship without Whisper entirely (`SMATCHET_WITH_WHISPER` OFF for DX12, see § Subsystem placement), so no local backend exists there regardless of this sub-option. *(Original entry, kept for history: backlog/agent-self-improvement/infra.md (2026-05-18, P2) posed three resolution paths — flip default ON, runtime-loaded DLL, or downgrade the UX decision.)*

### Open code gaps

- **Lua-authored `InputText` widgets bypass the dictation router** — backlog/agent-self-improvement/process.md (2026-05-18, P3). **Still open (2026-07-14; cited location stale)**: the original citation `AppController_LuaBindings.cpp:1816` predates the file's god-split — the Lua binding now lives at `AppController_LuaBindings.cpp` (`"input_text"` → `LuaDrawList::InputText`, ~line 238). The gap itself is unchanged: no `RegisterInputText` call exists anywhere in `Source/`, so Lua-spawned InputText widgets don't pick up dictation (built-in surfaces unaffected). Fix: route the `LuaDrawList::InputText` draw path through the `SmatchetLocalizedImGui` wrapper / `g_dictationRouter.RegisterInputText`. ~1 h.

### Bucket-E manual gaps (no ImGui Test Engine wiring yet)

1. **Real-mic end-to-end push-to-talk** — hold `Ctrl+Alt+Space`, speak, release; text appears in focused InputText. Requires audio hardware + a foreground build of `Smatchet.exe`. Recipe in Phase D/E Verification entries.
2. **Visual cue latency under load** — confirm mic indicator appears within 100 ms of hotkey press (Pillar 2 contract). Requires an eyeball test at 144 Hz.
3. **Hotkey rebind round-trip across app launches** — capture a new combo in Preferences, restart the app, confirm the new combo fires the global hook. Verifies persistence + plugin-OnStart re-registration.
4. **Visual eyeball polish of the four UI surfaces** — setup banner, Preferences Whisper tab, status-bar mic indicator, amplitude-meter overlay.
5. **Test connection button against real OpenAI API key** — verify the "Verified" / "HTTP 4xx" result paths in Preferences.
6. **Auto-send-on-punctuation flow** — focus AI Assistant chat, dictate a sentence ending in `.` / `!` / `?`, confirm Send fires.

All six are documented in the per-phase Verification entries; the new `whisper-dictation-roundtrip` scenario covers the press → schedule-worker → MainThreadDispatcher → InsertIntoFocusedInputText half of the pipeline without any of the above hardware/network dependencies.

### Explicit non-goals (deferred to future plans, NOT regressions)

- Real-time streaming partial results (`whisper.cpp` supports it; v2 of this plan)
- Cross-platform audio (Linux ALSA / macOS CoreAudio) — Windows-first ship
- Unreal / DX12 path — plugin gated OFF under `SMATCHET_EMBEDDED_IN_UNREAL`
- GPU acceleration (CUDA / Vulkan / Metal)
- Resampler upgrade — current integer-decimation downsampler is documented in Phase B deviations; future `libsamplerate` / `soxr` swap when accuracy matters

### Resolved (closed during the plan; kept here for audit-trail)

- Phase E's "hot-rebind deferred to Phase F follow-up" deviation — **RESOLVED by Phase F** via `WhisperPlugin::InstanceForUi()->ReregisterHotkey(...)`. Live hot-rebind works end-to-end without a process restart.
- `WhisperApiKey` vs `AiApiKey` shared-key concern — **RESOLVED** during planning (Phase B + Phase F): separate field with documented 5-row fallback table, no silent breakage when AI Assistant switches providers.
- Phase D agent-completion notification race — **RESOLVED** during Phase D: orchestrator took ownership of the agent's uncommitted work, shipped as PR #213; the agent's late duplicate PR #218 was closed.

## Origin (planning record, kept for posterity)

- **Phase at time of writing**: planning (this doc)
- **Originating request**: user — "Make a plan on how to re-add whisper. Check the git history"
- **Git history result**: zero whisper / voice / speech-to-text history in repo (`git log --all --grep`, deleted-file diff, branch + reflog scans all empty). Feature has never existed here despite the "re-add" framing. Plan written from scratch.
- **User-confirmed scope** (via AskUserQuestion, 2026-05-17):
  - Backend mode: **Both — local default, cloud fallback** (see Followups for the `SMATCHET_WHISPER_LOCAL_BACKEND` default-flip question).
  - Insertion targets: **all four** — focused ImGui InputText, AI Assistant chat box, grid long-text editor, Command Palette.
  - Build-time default: `SMATCHET_WITH_WHISPER=ON` (mirrors `SMATCHET_WITH_LUA_AUTOMATION` / `SMATCHET_WITH_MCP`).
  - Setup UX: **non-blocking banner at top of UI** (not modal).

## Goal

Push-to-talk dictation. User holds a configurable hotkey, speaks, releases → transcribed text lands in whichever ImGui input is focused. Local whisper.cpp by default for offline + zero-cost; OpenAI Whisper API as fallback for low-power devices or first-run before the local model is downloaded.

## Non-goals

- Always-listening / wake-word activation. Push-to-talk only.
- Speaker diarisation, language detection beyond Whisper's built-in.
- Real-time streaming transcription (interim partial results). Whisper.cpp does support it; defer until v2 — first cut transcribes the full clip on release.
- Voice commands as a primary UX. The Command Palette is one of the insertion targets, so saying "open settings" lands in the palette and fuzzy-matches — but a dedicated voice-command grammar is out.
- Cross-platform audio. Windows-first (WASAPI). Linux / macOS deferred.
- Unreal / DX12 build. Whisper plugin gated `OFF` under `SMATCHET_EMBEDDED_IN_UNREAL`.
- GPU acceleration of whisper.cpp. CPU-only first cut; CUDA / Vulkan optional follow-up.
- **Auto-downloading model files**. No model is ever fetched without explicit user consent — see § Optional + no-data-without-consent.
- **Bundling model files into the exe**. The Whisper plugin compiles in (binary cost ~tens of MB), but no `.bin` model ships with the installer. Models are 40-1500 MB and would dwarf the exe.

## Optional + no-data-without-consent

**Two-layer opt-out:**

| Layer | Mechanism | Effect |
|---|---|---|
| **Build-time** | `cmake -DSMATCHET_WITH_WHISPER=OFF` | Plugin code excluded entirely. Binary size delta = 0. For distributors / corporate IT who want a slim build. The router stub TU compiles in instead. |
| **Runtime** | First-run setup dialog + `WhisperSetupCompleted` / `WhisperSetupChoice` config fields | Plugin compiled but dormant. No mic access, no network call, no model download, no UI surface beyond Preferences. |

**First-run setup banner** — non-blocking banner pinned to the top of the main window when `WhisperSetupCompleted == false` AND `SMATCHET_WITH_WHISPER` is on. App is fully usable while the banner is visible; the banner persists across launches until the user picks one of the three actions. ImGui-rendered (no separate installer process — Smatchet ships as a self-contained exe). Three actions:

| Button | Effect |
|---|---|
| **"Enable voice dictation"** | Opens a model-size sub-picker (`ggml-tiny.en` 40 MB / `ggml-base.en` 150 MB recommended / `ggml-small.en` 500 MB). Selected model downloads on the spot via Pattern A worker with progress bar + cancel. On success: `WhisperEnabled = true`, `WhisperSetupCompleted = true`, `WhisperSetupChoice = "enabled"`. On cancel: dialog returns to the three-button state. On download failure: toast + retry button. |
| **"Decide later"** | `WhisperSetupCompleted = false` (unchanged — dialog will reappear next launch). |
| **"No thanks, don't use voice dictation"** | `WhisperSetupCompleted = true`, `WhisperSetupChoice = "disabled"`, `WhisperEnabled = false`. Dialog never appears again. Feature remains reversible via Preferences → AI → Whisper → "Enable voice dictation". |

**Consent invariants** (each enforced by code, not just convention):

1. **No mic access before consent**. `WindowsAudioCapture::Start()` first call gates on `WhisperEnabled == true`. Returns "consent required" if false; logs a warning. Doctest covers this.
2. **No network fetch before consent**. `ModelDownloader::Start()` gates on the user clicking the "Enable" button in the setup dialog OR the Preferences download button — both record an explicit consent timestamp. No silent re-downloads, no resume-after-restart without re-confirming.
3. **No cloud API call before consent**. `WhisperApiClient::Transcribe()` gates on `WhisperEnabled` AND a resolvable API key — see § fallback table. If gate fails, the call returns synchronously without touching the network.
4. **Privacy disclosure inline in the setup dialog**: bullet list under each button explains what data goes where (local: no network; cloud: audio uploaded to OpenAI; disabled: nothing). Sourced from `Locales/<lang>.json` so it's translatable.

**Reversibility**: any of the three choices can be changed later in Preferences → AI → Whisper. "Disabled" is sticky for the setup dialog (dialog doesn't reappear) but a single Preferences toggle flips it back. No "are you sure?" friction on the reversal.

## Architecture

Two-tier dispatch behind a single user-visible feature:

```
                              ┌─────────────────────────┐
       Hotkey held            │  WhisperPlugin (NEW)    │
       ──────────────────────▶│  - audio capture        │
                              │  - mode router          │
                              └────────────┬────────────┘
                                           │
                          ┌────────────────┴────────────────┐
                          ▼                                 ▼
                ┌──────────────────┐              ┌──────────────────┐
                │ WhisperLocal     │              │ WhisperApiClient │
                │ (whisper.cpp)    │              │ (OpenAI /v1/...) │
                └────────┬─────────┘              └────────┬─────────┘
                         │                                 │
                         └────────────────┬────────────────┘
                                          ▼
                              ┌─────────────────────────┐
                              │ DictationInsertionRouter│
                              │  - tracks active target │
                              └────────────┬────────────┘
                                           ▼
        ┌────────────────┬────────────────┬────────────────┐
        ▼                ▼                ▼                ▼
   Focused          AI Assistant      Grid long-text    Command
   InputText        chat box          editor            Palette
```

### Subsystem placement

- **`Plugins/Whisper/`** — new optional plugin, mirrors `Plugins/LuaConsole/` and `Plugins/Mcp/`. Gated by `SMATCHET_WITH_WHISPER` (default ON for Standalone, OFF for DX12 — same shape as `SMATCHET_WITH_MCP` / `SMATCHET_WITH_LUA_AUTOMATION`).
- **`Source_Core/include/IDictationHost.h`** — pure-virtual interface for the insertion router (matches `ILuaBindingHost` pattern from PR #144). Always-compiled header — declarations only, no Whisper deps.
- **`Source_Core/src/DictationInsertionRouter_Whisper.cpp`** — real impl (compiled when `SMATCHET_WITH_WHISPER=ON`).
- **`Source_Core/src/DictationInsertionRouter_Stubs.cpp`** — no-op impl (compiled when `SMATCHET_WITH_WHISPER=OFF`). Mirrors `AppController_LuaStubs.cpp` precedent — keeps call sites in `SmatchetUI.cpp` / `SmatchetAiAssistantUi.cpp` / `TicketFieldEditor.cpp` / `SmatchetCommandPaletteUi.cpp` free of `#if defined(...)` blocks.
- **`Plugins/Whisper/WhisperPlugin.cpp`** — plugin entry; owns audio capture + mode routing. All under `Plugins/Whisper/` is CMake-conditional, so no source-level ifdefs needed inside that subtree.
- **`Plugins/Whisper/WhisperLocal.cpp`** — whisper.cpp wrapper.
- **`Plugins/Whisper/WhisperApiClient.cpp`** — OpenAI `/v1/audio/transcriptions` client. Uses its **own** `cfg.WhisperApiKey` field (DPAPI-encrypted). Falls back to `cfg.AiApiKey` only when `WhisperApiKey` is empty AND the AI Assistant's selected provider is OpenAI — that gives the "set OpenAI key once" UX for users who happen to use OpenAI for both, without coupling the two features. Separate field is the right shape because: (a) AI Assistant can run on Anthropic / LM Studio / Ollama with an empty / non-OpenAI `AiApiKey`, in which case Whisper needs its own key; (b) users may want billing separation between chat and transcription; (c) revoking one key without affecting the other is a real-world need.
- **`Plugins/Whisper/WindowsAudioCapture.cpp`** — WASAPI ring-buffer capture, RAII for `IAudioClient` / `IAudioCaptureClient`.

## Conditional compilation (`SMATCHET_WITH_WHISPER`)

Mirror the `SMATCHET_WITH_LUA_AUTOMATION` precedent exactly. Six gating layers:

### Layer 1: CMake option + Unreal flip

```cmake
option(SMATCHET_WITH_WHISPER "Build with Whisper dictation plugin" ON)

# Unreal path keeps the plugin out — no WASAPI / whisper.cpp pollution.
if(SMATCHET_EMBEDDED_IN_UNREAL)
    set(SMATCHET_WITH_WHISPER OFF)
endif()
```

### Layer 2: Source-list conditional inclusion in `CMakeLists.txt`

Mirror lines 546-553 of the current `CMakeLists.txt` (the `AppController_LuaBindings.cpp` / `AppController_LuaStubs.cpp` swap):

```cmake
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Whisper.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Stubs.cpp")
if(SMATCHET_WITH_WHISPER)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Whisper.cpp")
else()
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Stubs.cpp")
endif()
```

### Layer 3: PUBLIC compile definition

```cmake
if(SMATCHET_WITH_WHISPER)
    target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_WITH_WHISPER=1)
endif()
```

### Layer 4: Bindings/stubs split (eliminates call-site ifdefs)

`DictationInsertionRouter_Whisper.cpp` and `DictationInsertionRouter_Stubs.cpp` expose the **same** symbols (`DictationInsertionRouter::RegisterInputText(...)`, `Insert(...)`, `IsRecording()`, etc.). The stubs return `false` / no-op. This means UI TUs call the router unconditionally:

```cpp
// SmatchetUI.cpp, TicketFieldEditor.cpp, etc. — NO #if guards needed:
g_dictationRouter.RegisterInputText(buf, cap, &cursor);
```

### Layer 5: Call-site ifdefs (used sparingly, only where stubs don't fit)

Per the Lua precedent (`AppController.cpp:369`, `BuiltinCommands_Debug.cpp:120`), `#if defined(SMATCHET_WITH_WHISPER)` is used at the **two** places the stubs pattern can't cover:

1. **Plugin instantiation** in `AppController::Initialize` — the plugin object itself only exists when the macro is set.
2. **Status-bar mic indicator** in `SmatchetUI.cpp` — probes plugin state that doesn't exist in stub-mode. Cleaner to ifdef than to fake.

Every other touched UI file uses the stub-backed router and stays ifdef-free.

### Layer 6: Plugin-internal subtree

Everything under `Plugins/Whisper/` is **CMake-conditional at the directory level** — `add_subdirectory(Plugins/Whisper)` only runs when `SMATCHET_WITH_WHISPER=ON`. So WASAPI / whisper.cpp / OpenAI client TUs need no source-level ifdefs; they don't get compiled in the off case at all.

### Per-file gating audit

| File | Gating mechanism |
|---|---|
| `Plugins/Whisper/*` | Layer 6 (subtree CMake-conditional) |
| `Source_Core/src/DictationInsertionRouter_Whisper.cpp` | Layer 2 (source-list conditional) |
| `Source_Core/src/DictationInsertionRouter_Stubs.cpp` | Layer 2 (source-list conditional — opposite branch) |
| `Source_Core/include/IDictationHost.h` | Always-compiled (declarations only, no Whisper deps) |
| `Source_Core/include/DictationInsertionRouter.h` | Always-compiled (declarations only) |
| `Source_Core/include/ConfigManager.h` + `.cpp` | Layer 5 — additive fields wrapped `#if defined(SMATCHET_WITH_WHISPER)` |
| `Source_Core/src/SmatchetUI.cpp` | Layer 5 — mic-active indicator only |
| `Source_Core/src/SmatchetAiAssistantUi.cpp` | Layer 4 — register call is stub-safe, no ifdef |
| `Source_Core/src/TicketFieldEditor.cpp` | Layer 4 — register call is stub-safe, no ifdef |
| `Source_Core/src/SmatchetCommandPaletteUi.cpp` | Layer 4 — register call is stub-safe, no ifdef |
| `Source_Core/include/SmatchetLocalizedImGui.h` | Layer 4 — InputText wrapper calls stub-safe router |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | Layer 5 — Whisper tab wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif` |
| `Source_Core/src/AppController.cpp` | Layer 5 — plugin instantiation wrapped `#if defined(SMATCHET_WITH_WHISPER)` |
| `Source_Core/src/SmatchetWhisperOverlayUi.cpp` | Layer 2 (source-list conditional — only added to `CORE_SOURCES` when `SMATCHET_WITH_WHISPER=ON`) |
| `Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp` | Layer 2 (source-list conditional — same as overlay UI) |
| `tests/Source_Core/DictationInsertionRouter.test.cpp` | Layer 1 — `if(SMATCHET_WITH_WHISPER AND SMATCHET_BUILD_TESTS)` in `tests/CMakeLists.txt` |
| `tests/Source_Core/WhisperApiPayload.test.cpp` | Same as above |
| `tests/Source_Core/WhisperModeRouter.test.cpp` | Same as above |
| `Locales/en.json` | No gating — translations are inert when feature is off; tiny disk cost |
| `docs/guides/perf-workflow.md`, `agents/*.md`, plan doc | No gating — documentation |

### Threading model — strict Pillar 1 + 2

Four threads involved:

1. **UI thread** — registers hotkey, reads "is recording?" flag for the visual cue, receives `PostToMainThread` callback to insert text. **Never** touches WASAPI / whisper.cpp / `cpr`.
2. **Hotkey thread** — Windows low-level keyboard hook (`SetWindowsHookEx WH_KEYBOARD_LL`). On press/release, posts a message to the audio thread.
3. **Audio capture thread** — WASAPI event-driven capture loop. On hotkey-release, finalises the clip + posts a transcription request to a worker via `AppController::LaunchBackgroundTask`.
4. **Worker thread (transcription)** — runs whisper.cpp inference OR `cpr::Post` to OpenAI. On completion, posts result back to UI via `MainThreadDispatcher::PostToMainThread([text]{ router.Insert(text); })`. Same Pattern A as PR #186 / #191.

Cancel atom (`std::shared_ptr<std::atomic<bool>>`) on each in-flight transcription so a second push-to-talk while the first is still transcribing cleanly drops the older result rather than racing.

### Mode router decision tree

Per-press, the plugin picks one backend:

| Condition | Backend |
|---|---|
| `WhisperMode == "local"` AND local model present on disk | Local |
| `WhisperMode == "cloud"` | Cloud |
| `WhisperMode == "auto"` (default) AND local model present | Local |
| `WhisperMode == "auto"` AND no local model BUT API key configured | Cloud |
| `WhisperMode == "auto"` AND no local model AND no API key | Show "Configure Whisper" toast; do not record |

Cloud-on-fallback: if local inference fails (model load error, OOM), the next press automatically routes cloud for that session and surfaces a warning.

### Config schema additions

New fields on `ConfigManager` (additive; no schema bump per AGENTS.md schema-version rule — these use `j.value()` defaults). Field declarations in `ConfigManager.h` and serialisation in `ConfigManager.cpp` are wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif` (gating layer 5):

**Phase A (minimum viable schema):**

- `WhisperEnabled : bool` — default `false` (opt-in; gates plugin runtime activation even when compiled in)
- `WhisperSetupCompleted : bool` — default `false`; flips to `true` once user has seen and answered the first-run setup dialog
- `WhisperSetupChoice : string` — default `""`; one of `"enabled"`, `"disabled"`, `""` (means "deferred — show dialog next launch"). Informational; the active gate is `WhisperEnabled`
- `WhisperMode : string` — `"local" | "cloud" | "auto"`, default `"auto"`
- `WhisperModel : string` — e.g. `"ggml-base.en"`, default `"ggml-base.en"`
- `WhisperHotkey : string` — default `"Ctrl+Alt+Space"` (push-to-talk)
- `WhisperApiKey : string (DPAPI)` — default empty; OpenAI key for `/v1/audio/transcriptions`

**API key fallback rule** (resolved in `WhisperApiClient::ResolveKey()`):

| `WhisperApiKey` | `AiProvider` | `AiApiKey` | Resolved key |
|---|---|---|---|
| set | — | — | `WhisperApiKey` (explicit always wins) |
| empty | `"openai"` | set | `AiApiKey` (convenience fallback — same OpenAI account) |
| empty | `"openai"` | empty | no key → toast "Configure Whisper API key in Preferences" |
| empty | `"anthropic"` | (irrelevant) | no key → same toast (Anthropic doesn't expose Whisper) |
| empty | `"ollama" / "lmstudio"` | (irrelevant) | no key → same toast |

The fallback exists for ergonomics (single-key UX when user happens to use OpenAI for both), but `WhisperApiKey` is the canonical field. AI Assistant changes never silently break Whisper.

**Phase F (deferred until UI tab lands):**

- `WhisperLanguage : string` — default `"en"`; `"auto"` for autodetect
- `WhisperTrim : bool` — default `true`; strip leading/trailing silence before insertion
- `WhisperMaxClipSec : int` — default `60`; hard cap at `600` (cost guard, see § Open questions)
- `WhisperAutoSendOnPunctuation : bool` — default `false`; AI Assistant chat box only

Model files stored at `<smatchet user data>/whisper/<model>.bin`; resolved via `ConfigManager::GetPlatformSharedUserDataDirectory` (same pattern AI keys use). The directory is created lazily on first model download; absence is normal.

### Insertion targets — `IDictationHost::Insert(const std::string& text)`

Each surface registers itself with the router when its widget becomes focused, unregisters on blur:

| Surface | Owner | Detection | Insert mechanism |
|---|---|---|---|
| Focused ImGui InputText | `DictationInsertionRouter` | `ImGui::GetActiveID()` matches a tracked input; capture the input's underlying `char* buf` + cursor via a centralised `RegisterInputText` helper called by every `SmatchetLocalizedImGui::InputText*` wrapper | Splice text at cursor, advance cursor, mark dirty |
| AI Assistant chat box | `SmatchetAiAssistantUi` | Always-tracked when panel open | Append to chat input buffer; if dictation ends with sentence-terminator, optionally auto-send (configurable) |
| Grid long-text editor | `TicketFieldEditor` | Tracked when `s_ActiveLongTextState.Active` | Splice into `Buffer` at cursor |
| Command Palette | `SmatchetCommandPaletteUi` | Tracked when palette open | Append to search buffer; trigger fuzzy re-match same frame |

Generic-focused-InputText case is the load-bearing one: it means **every** ImGui input in the app accepts dictation without per-call site changes. That requires wrapping `ImGui::InputText` / `InputTextMultiline` calls in `SmatchetLocalizedImGui` so the router sees every registration. We already have a localization wrapper (`#define ImGui SmatchetLocalizedImGui` at the top of UI TUs) — the dictation hook piggybacks on it.

### Visual cue (Pillar 2 contract)

- Mic-active indicator in the status bar within **100 ms** of hotkey press (target ~10 ms).
- A floating overlay shows live amplitude meter while recording (cheap — single ImGui draw per frame from a worker-written shared atom).
- Spinner replaces meter while transcribing.
- Cancelable via `Esc` while recording; cancel atom flips, no insertion happens.

## Phase plan

Each phase = one PR. Sequential — later phases depend on earlier ones.

### Phase A — Plugin shell + config schema + dictation router skeleton

**Owner**: command-system (config + router) + lua-binder pattern reuse.

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `Plugins/Whisper/CMakeLists.txt` | NEW | L6 (subtree CMake-conditional) |
| `Plugins/Whisper/WhisperPlugin.{h,cpp}` | NEW (stub `RegisterCommands`, no transcription) | L6 |
| `Source_Core/include/IDictationHost.h` | NEW (pure-virtual interface, declarations only) | always-compiled |
| `Source_Core/include/DictationInsertionRouter.h` | NEW (class decl, declarations only) | always-compiled |
| `Source_Core/src/DictationInsertionRouter_Whisper.cpp` | NEW (real impl) | L2 (source-list conditional) |
| `Source_Core/src/DictationInsertionRouter_Stubs.cpp` | NEW (no-op impl, same symbols) | L2 (opposite branch) |
| `Source_Core/include/ConfigManager.h` | MOD (4 fields wrapped `#if defined(SMATCHET_WITH_WHISPER)`) | L5 |
| `Source_Core/src/ConfigManager.cpp` | MOD (serialise/deserialise the 4 fields, same gate) | L5 |
| `Source_Core/src/AppController.cpp` | MOD (plugin instantiation wrapped `#if defined(SMATCHET_WITH_WHISPER)`) | L5 |
| `CMakeLists.txt` | MOD (add `SMATCHET_WITH_WHISPER` option + L1/L2/L3 wiring, mirror Lua block) | L1 + L2 + L3 |
| `tests/CMakeLists.txt` | MOD (`if(SMATCHET_WITH_WHISPER AND SMATCHET_BUILD_TESTS)` block, mirror Lua tests gate) | L1 |
| `tests/Source_Core/DictationInsertionRouter.test.cpp` | NEW (pure-helper doctest, tests stub-mode no-op + whisper-mode register/insert) | L1 gate via parent |

**Verification**:

- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 build picks up stubs TU).
- `cmake -DSMATCHET_WITH_WHISPER=OFF --build ...` — Standalone-with-Whisper-off compiles green using stubs TU.
- `cmake --build --preset ninja-test-msvc && ctest` — router unit tests pass for both on and off configurations.
- `Smatchet.exe cmd whisper.status` — returns `{"enabled": false, "mode": "auto", "model_present": false}` (whisper-on build); command not registered at all (whisper-off build).
- No runtime visual changes in either configuration.

### Phase B — Cloud-only transcription (OpenAI Whisper API)

Smallest path to end-to-end working dictation. Validates audio capture + API client + insertion router without dragging in whisper.cpp.

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `Plugins/Whisper/WhisperApiClient.{h,cpp}` | NEW (`cpr::Post` multipart to `/v1/audio/transcriptions`, key resolved via `cfg.WhisperApiKey` with `cfg.AiApiKey` fallback per § fallback table) | L6 |
| `tests/Source_Core/WhisperApiKeyResolve.test.cpp` | NEW (pure: 5-row fallback decision table) | L1 |
| `Plugins/Whisper/WindowsAudioCapture.{h,cpp}` | NEW (WASAPI mic capture, 16 kHz mono PCM int16) | L6 |
| `Plugins/Whisper/WhisperPlugin.cpp` | MOD (wire audio + cloud path; register `whisper.transcribe-once <wav-path>` CLI) | L6 |
| `Source_Core/src/DictationInsertionRouter_Whisper.cpp` | MOD (route transcription request through `app.LaunchBackgroundTask` + `MainThreadDispatcher::PostToMainThread`) | L2 |
| `Source_Core/src/DictationInsertionRouter_Stubs.cpp` | MOD (add the new method signature as a no-op so stubs stay sync'd — mirror `AppController_LuaStubs.cpp` discipline) | L2 |
| `tests/Source_Core/WhisperApiPayload.test.cpp` | NEW (pure: build multipart body, parse OpenAI response JSON) | L1 |
| `tests/Source_Core/WavWriter.test.cpp` | NEW (pure: WAV header writer for the capture sink) | L1 |

**Verification**:

- `Smatchet.exe cmd whisper.transcribe-once --file fixtures/hello-world.wav --mode cloud` returns `{"text": "...", "elapsed_ms": ...}`. Fixture WAV bundled under `tests/fixtures/`.
- `code-review` pillar-2 sniff: zero sync `cpr::Post` reaching `ImGui::*`-frame stack. The plugin enforces dispatch via `LaunchBackgroundTask`.
- `bash scripts/dev/test-grid-edit-perf-postfix.sh` — perf gate green (no regression).
- Manual: open Smatchet, hold `Ctrl+Alt+Space`, speak "open settings", release; "open settings" appears in the Command Palette and fuzzy-matches.

### Phase C — Local whisper.cpp + model download + **first-run setup dialog**

The setup dialog lands here (not earlier) because it requires the model-download path to be functional. Users running A→B builds enable Whisper manually via a hidden `whisper.dev-enable` CLI command; the public setup wizard appears once Phase C ships.

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `CMakeLists.txt` | MOD (FetchContent `ggerganov/whisper.cpp` at a pinned tag, guarded `if(SMATCHET_WITH_WHISPER)`) | L1 |
| `Plugins/Whisper/WhisperLocal.{h,cpp}` | NEW (wraps `whisper_init_from_file_with_params` + `whisper_full`; `whisper_context*` under `std::unique_ptr`) | L6 |
| `Plugins/Whisper/WhisperPlugin.cpp` | MOD (mode router selects local vs cloud per § Mode router decision tree) | L6 |
| `Plugins/Whisper/ModelDownloader.{h,cpp}` | NEW (Pattern A worker for downloading model from huggingface; resumable, SHA-256 verified; explicit consent timestamp recorded per § Consent invariants) | L6 |
| `Source_Core/src/SmatchetWhisperSetupBanner.cpp` | NEW (non-blocking top-of-window banner; three actions + model-size sub-picker on Enable; ImGui-rendered; visible while `cfg.WhisperSetupCompleted == false`) | L2 (source-list conditional — only added when `SMATCHET_WITH_WHISPER=ON`) |
| `Source_Core/src/SmatchetUI.cpp` | MOD (invoke `DrawWhisperSetupBanner(...)` from main draw loop when `!cfg.WhisperSetupCompleted`; wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`) | L5 |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | MOD (Whisper tab with "Re-run setup", "Download model", model-size picker, current-choice readout, all wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`) | L5 |
| `Locales/en.json` | MOD (strings for the setup dialog: title, three button labels, privacy bullets, model-size descriptions) | none |
| `tests/Source_Core/WhisperModeRouter.test.cpp` | NEW (pure: mode decision tree) | L1 |
| `tests/Source_Core/WhisperConsentGate.test.cpp` | NEW (pure: the 4 consent invariants — no mic / no network / no API / no model fetch without `WhisperEnabled == true` AND `WhisperSetupCompleted == true`) | L1 |

**Setup banner spec** — pinned to the top of the main window, non-blocking. App is fully usable while visible. Approximate layout:

```
┌─────────────────────────────────────────────────────────────┐
│ 🎤  Enable voice dictation? Push-to-talk transcribes into   │
│     any text field. Optional, off by default. No audio      │
│     leaves your machine when local model is used.           │
│     [ Enable ▾ ]  [ Decide later ]  [ No thanks ]           │
└─────────────────────────────────────────────────────────────┘
```

Clicking **Enable ▾** expands an inline picker:

```
┌─────────────────────────────────────────────────────────────┐
│ 🎤  Choose speech model:                                    │
│     ○ Smaller, faster (40 MB)                               │
│     ◉ Recommended (150 MB)                                  │
│     ○ Higher accuracy (500 MB)                              │
│     [ Download + enable ]  [ Cancel ]                       │
└─────────────────────────────────────────────────────────────┘
```

On **Download + enable**: model download begins via Pattern A worker with inline progress bar in the banner (no separate modal). On success: banner disappears; first push-to-talk works without further setup. On **Cancel** inside the picker: returns to the three-action banner. On download failure: banner shows error + retry button.

On **Decide later**: banner stays visible (no state change). Same banner re-shows every launch until the user picks Enable or No-thanks.

On **No thanks**: banner closes; `WhisperEnabled = false` and `WhisperSetupCompleted = true`; banner never reappears. Reversible via Preferences → AI → Whisper → "Enable voice dictation".

**Banner placement**: full-width strip immediately under the menu bar, above the dock space. Height ~52 px when collapsed (default state), ~120 px when model-size picker is expanded, ~70 px during active download (progress bar + cancel). Color: subtle accent (Smatchet theme `Frame` color), not alarming red.

**Verification**:

- `Smatchet.exe cmd whisper.transcribe-once --file fixtures/hello-world.wav --mode local` works after `Smatchet.exe cmd whisper.download-model --name ggml-base.en` finishes.
- Build size delta: document the binary growth from linking whisper.cpp + ggml; flag if > 50 MB.
- Pillar 1: a 10 s clip transcribes in < 2 s on a modern CPU (base.en model); confirms whisper.cpp invocation doesn't block UI thread (it's on the worker, so trivially compliant).
- `code-review` Pillar 3 sniff: no raw `new`/`delete` in the whisper.cpp wrapper; `whisper_context*` lifetime under `std::unique_ptr` with custom deleter.

### Phase D — Generic focused-InputText hook + the four target surfaces

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `Source_Core/include/SmatchetLocalizedImGui.h` | MOD (`InputText` / `InputTextMultiline` wrappers call stub-safe `g_dictationRouter.RegisterInputText(...)`; **no `#if` needed** — stub returns no-op) | L4 |
| `Source_Core/src/SmatchetAiAssistantUi.cpp` | MOD (explicit register on panel open via stub-safe router) | L4 |
| `Source_Core/src/TicketFieldEditor.cpp` | MOD (register when `s_ActiveLongTextState.Active`, via stub-safe router) | L4 |
| `Source_Core/src/SmatchetCommandPaletteUi.cpp` | MOD (register when palette open, via stub-safe router) | L4 |
| `Source_Core/src/DictationInsertionRouter_Whisper.cpp` | MOD (full insert-at-cursor logic with multi-byte UTF-8 safety) | L2 |
| `Source_Core/src/DictationInsertionRouter_Stubs.cpp` | MOD (matching no-op signatures — keep stubs sync'd) | L2 |
| `tests/Source_Core/DictationInsertionRouter.test.cpp` | MOD (extend coverage to UTF-8 splicing + cursor advance) | L1 |

**Verification**:

- New scenario `scenario.whisper-dictation-roundtrip` (per the existing scenario authoring pattern from PR #201): synthesises an audio fixture → triggers transcription via CLI → asserts the test text appears in a registered InputText buffer.
- Manual: hold hotkey while typing in any of the four target surfaces; transcribed text inserts at cursor without losing pre-typed content.

### Phase E — Push-to-talk hotkey + visual cue

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `Plugins/Whisper/GlobalHotkey_Win32.{h,cpp}` | NEW (`RegisterHotKey` for in-focus, `SetWindowsHookEx WH_KEYBOARD_LL` for global) | L6 |
| `Plugins/Whisper/WhisperPlugin.cpp` | MOD (wire hotkey thread; debounce; ignore key-repeat) | L6 |
| `Source_Core/src/SmatchetUI.cpp` | MOD (mic-active indicator in status bar, wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif` — probes plugin state which doesn't exist in stub-mode) | L5 |
| `Source_Core/src/SmatchetWhisperOverlayUi.cpp` | NEW (floating amplitude meter overlay; cheap) | L2 (source-list conditional — entire TU only compiled when `SMATCHET_WITH_WHISPER=ON`) |
| `Locales/en.json` | MOD (strings for `whisper.statusBar.recording`, `whisper.overlay.cancel`, etc.) | none (inert when feature off) |

**Verification**:

- Pillar 2: from hotkey-press to visible mic indicator < 100 ms (measured via a new `perf_temp:` scope on the press → first-frame-with-indicator boundary).
- `Esc` cancels in-flight recording; no insertion happens; status returns to idle.
- Hotkey rebindable via Preferences → AI → Whisper → Hotkey capture widget.

### Phase F — Settings UI + model management UX

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `Source_Core/src/SmatchetPreferencesUi.cpp` | MOD (new "Whisper" sub-tab under AI tab, wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`) | L5 |
| `Source_Core/include/ConfigManager.h` + `Source_Core/src/ConfigManager.cpp` | MOD (4 Phase-F fields wrapped `#if defined(SMATCHET_WITH_WHISPER)`) | L5 |
| `Plugins/Whisper/ModelCatalog.{h,cpp}` | NEW (pure-helper list of available models with sizes + URLs) | L6 |
| `tests/Source_Core/ModelCatalog.test.cpp` | NEW | L1 |

**Verification**:

- UI screenshot diff: new Whisper tab matches mock.
- `Smatchet.exe cmd whisper.list-models` shows the catalog.
- Download button shows progress; cancel button works mid-download.

### Phase G — Tests, perf gate, bucket-E coverage

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `scripts/dev/test-whisper-roundtrip.sh` | NEW (auto-enrolled by `test-all.sh`; runs cloud `whisper.transcribe-once` path; **bash-level guard** at top: `command -v Smatchet.exe && Smatchet.exe cmd whisper.status 2>&1 \| grep -q "enabled" \|\| exit 0` — skips silently when feature is off) | self-guard |
| `Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp` | NEW (scenario implementation) | L2 (source-list conditional — only added when `SMATCHET_WITH_WHISPER=ON`) |
| `Source_Core/src/AppController.cpp` | MOD (scenario `RegisterFactory` line wrapped `#if defined(SMATCHET_WITH_WHISPER)` — mirror existing factory block) | L5 |
| `tests/fixtures/hello-world.wav` | NEW (16 kHz mono 1 s clip, ~32 KB) | none (test asset; cheap to keep) |
| `tests/_mocks/openai-whisper-response.json` | NEW (recorded API response for offline test) | none (test asset) |
| `agents/perf-detective.md` | MOD if needed (pre-flight `scenario.list` already covers whisper scenario after Phase A) | none (docs) |

**Verification**:

- `bash scripts/dev/test-all.sh` — all gates including the new whisper roundtrip pass.
- `Smatchet.exe cmd scenario.run whisper-dictation-roundtrip` — scenario passes.

## Reuse from existing infrastructure

| Need | Existing primitive |
|---|---|
| Worker dispatch | `AppController::LaunchBackgroundTask` (PR #186) |
| Worker → UI hand-off | `MainThreadDispatcher::PostToMainThread` (PR #163) |
| API key storage | `ConfigManager::AiApiKey` already DPAPI-encrypted (Phase A AI work) |
| HTTP POST | `cpr` (already linked) |
| JSON parse | `nlohmann::json` (already linked) |
| Cancel atom | `std::shared_ptr<std::atomic<bool>>` per PR #191 installer-download path |
| In-flight set / dedup | `IconUrlFetchInFlightSet` pattern from PR #191 |
| Plugin gate macro | `SMATCHET_WITH_MCP` / `SMATCHET_WITH_LUA_AUTOMATION` shape |
| Pure-helper TU split | `TicketFieldEditorLongTextPure` pattern from PR #196 |
| CLI command registration | `RegisterCommand({...})` in `BuiltinCommands` |
| Scenario authoring | `Source_Core/src/Commands/Scenarios/<Name>Scenario.cpp` (PR #201) |
| Hotkey UI tab | `SmatchetPreferencesUi.cpp` existing AI tab |

Net new infrastructure required: WASAPI capture wrapper, whisper.cpp wrapper, global-hotkey thread. Everything else is composition of existing patterns.

## Pillar compliance audit

| Pillar | Plan compliance |
|---|---|
| **1. ≤ 6.94 ms mean UI** | Audio capture + transcription on worker threads; UI thread sees only the indicator draw + final insertion callback. Indicator draw is ~10 µs. No regression expected. |
| **2. No sync I/O on UI thread** | WASAPI capture and `cpr::Post` and whisper.cpp inference all on workers. Push-to-talk hotkey thread is a Win32 hook thread, never reaches UI. Visual cue contract met (< 100 ms indicator). Cancelable via Esc. |
| **3. Never crash** | RAII wrappers around `IAudioClient` / `whisper_context*` / `HHOOK`. WASAPI fail-mode = log + disable hotkey, app keeps running. Whisper.cpp model-load failure → fallback to cloud, never throws into ImGui. Graceful degradation: assertions in dev, `LOG_ERROR` + safe default in ship. |
| **4. Accessibility (aspirational)** | Dictation IS an accessibility feature — keyboard-only users get an alternative input modality. Hotkey rebindable. Visual cue is high-contrast. No regression to keyboard-nav. Voice-only blind users still need screen-reader support (out of scope, tracked in pillar-4 backlog). |

## Open questions / risks

1. **Build size** — whisper.cpp + ggml is heavy. `Smatchet.exe` already 367 MB unstripped iter build; adding whisper.cpp static-linked could push it past 400 MB. Mitigation: ship as a separate DLL loaded at runtime when `WhisperEnabled=true`, avoiding the size hit for users who never enable it. Decision: defer to Phase C — measure first, refactor if > 50 MB delta.
2. **First-run model download UX** — `ggml-base.en.bin` is ~150 MB. Needs progress bar, cancelable, resumable. Mitigation: ship `ggml-tiny.en.bin` (~40 MB) as the default model; let users opt into larger models for accuracy. Decision: model size selector in Phase F.
3. **Generic InputText hooking** — splicing into `ImGui::InputText` via a global wrapper is invasive. If any third-party ImGui include in a plugin uses the raw `ImGui::InputText` (bypassing `SmatchetLocalizedImGui`), dictation won't work there. Mitigation: lint check (clang-tidy custom rule or grep gate) that flags raw `ImGui::InputText` in first-party code; document the rule in `AGENTS.md` § Project rules.
4. **Hotkey conflict** — `Ctrl+Alt+Space` might collide with system / IDE shortcuts. Mitigation: hotkey rebindable in Phase E; ship a sane default but warn on conflict-detect failure.
5. **Cloud cost surprise** — Whisper API is ~$0.006 / minute. A user accidentally holding the hotkey for an hour costs them ~$0.36. Mitigation: max-clip-length config (default 60 s); hard cap at 600 s with a warning toast.
6. **Privacy** — cloud path uploads voice audio to OpenAI. Surface this clearly in the Preferences UI; default mode `auto` prefers local; explicit "cloud" mode requires a one-time consent dialog on first use.
7. **DX12 / Unreal path** — WASAPI works under Unreal-on-Windows, but the plugin is gated OFF for `SMATCHET_EMBEDDED_IN_UNREAL` to avoid header pollution. Decision: defer Unreal support; add a follow-up backlog entry if a user requests it.
8. **Latency target** — local whisper.cpp on `ggml-base.en` does a 10 s clip in ~1-2 s on a modern CPU. Cloud API roundtrip is ~1-3 s. Both meet a "feels responsive" bar. Streaming partial results (v2) would drop perceived latency further but isn't critical for v1.

## CI matrix — both gating states must stay green

Lua has a known failure mode: `AppController_LuaBindings.cpp` and `AppController_LuaStubs.cpp` drift apart over time when one is edited without the other (different signatures, dropped methods). The current CI build implicitly catches Lua drift because the DX12 target sets `SMATCHET_WITH_LUA_AUTOMATION=OFF` and CI builds both standalone (ON) and DX12 (OFF).

For Whisper we want the same coverage — every PR must build green in **both** `SMATCHET_WITH_WHISPER=ON` and `SMATCHET_WITH_WHISPER=OFF`. The DX12 target gives us OFF coverage for free (mirroring Lua's setup), but to lock it in, Phase A's CI change adds a `with-whisper-off` job to `.github/workflows/build-and-test.yml`:

```yaml
- name: Build standalone with SMATCHET_WITH_WHISPER=OFF
  run: cmake -DSMATCHET_WITH_WHISPER=OFF --preset ninja-iter-msvc && cmake --build --preset ninja-iter-msvc
```

This catches stub drift the moment it lands — same regression-prevention shape as the existing dual-target build for DX12.

## Locking

Per the new git-ref system (since 2026-05-17 / PR #194/#198/#200/#202):

```bash
cat > /tmp/whisper-write-set <<'EOF'
Plugins/Whisper/
Source_Core/include/IDictationHost.h
Source_Core/include/DictationInsertionRouter.h
Source_Core/src/DictationInsertionRouter_Whisper.cpp
Source_Core/src/DictationInsertionRouter_Stubs.cpp
Source_Core/include/ConfigManager.h
Source_Core/src/ConfigManager.cpp
Source_Core/src/AppController.cpp
Source_Core/src/SmatchetPreferencesUi.cpp
Source_Core/src/SmatchetAiAssistantUi.cpp
Source_Core/src/TicketFieldEditor.cpp
Source_Core/src/SmatchetCommandPaletteUi.cpp
Source_Core/include/SmatchetLocalizedImGui.h
Source_Core/src/SmatchetUI.cpp
Source_Core/src/SmatchetWhisperOverlayUi.cpp
Source_Core/src/SmatchetWhisperSetupBanner.cpp
Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp
CMakeLists.txt
tests/CMakeLists.txt
Locales/en.json
tests/Source_Core/DictationInsertionRouter.test.cpp
tests/Source_Core/WhisperApiPayload.test.cpp
tests/Source_Core/WhisperApiKeyResolve.test.cpp
tests/Source_Core/WavWriter.test.cpp
tests/Source_Core/WhisperModeRouter.test.cpp
tests/Source_Core/WhisperConsentGate.test.cpp
tests/Source_Core/ModelCatalog.test.cpp
tests/fixtures/hello-world.wav
tests/_mocks/openai-whisper-response.json
scripts/dev/test-whisper-roundtrip.sh
.github/workflows/build-and-test.yml
EOF

AGENT_ID=orchestrator \
LOCK_BRANCH=feat/whisper-dictation \
LOCK_PLAN=docs/plans/shipped/whisper-dictation.md \
bash scripts/dev/lock-claim.sh whisper-dictation /tmp/whisper-write-set
```

Each phase PR body must include `lock-slug: whisper-dictation` so `.github/workflows/lock-cleanup.yml` auto-releases on final merge. For per-phase locking, use a phased slug (`whisper-dictation-phase-a`, etc.) and release each phase independently — preferred for this multi-PR work.

## Implementation log

- Phase D — generic ImGui InputText wrapper hook + four insertion targets wired. Adds `extern DictationInsertionRouter g_dictationRouter;` to `Source_Core/include/DictationInsertionRouter.h` (defined exactly once in `_Whisper.cpp` and `_Stubs.cpp`). Extends `Source_Core/include/SmatchetLocalizedImGui.h` with a `HookDictationOnLastItem(buf, buf_size)` helper invoked from `InputText` / `InputTextMultiline` / `InputTextWithHint` after each underlying ImGui call — auto-registers focused buffers, unregisters on blur. Fills in the real `Insert(...)` + `InsertIntoFocusedInputText(...)` body in `DictationInsertionRouter_Whisper.cpp` with full splice-at-cursor logic (in-place `memmove` shift, end-of-content fallback when cursor is null, capacity-bounded truncation with UTF-8 boundary safety via `Utf8SafeTruncate`). Explicit registration wired in three surfaces — `SmatchetAiAssistantUi.cpp` registers `s_inputCharBuf` (hoisted to namespace scope) on panel-open and unregisters on close / failed-Begin, `TicketFieldEditor.cpp` registers `s_ActiveLongTextState.Buffer` on `OpenLongTextEditor` and unregisters on `CloseLongTextEditor`, `Source_Core/src/Commands/CommandPaletteUi.cpp` (note: actual path; design doc earlier said `SmatchetCommandPaletteUi.cpp`) registers `filterBuf_` on `Open()` and unregisters on `Close()`. Extended `tests/Source_Core/DictationInsertionRouter.test.cpp` with 10 new cases — empty / non-empty / mid-cursor splices, cursor advance chain, capacity truncation, UTF-8 codepoint boundary integrity, idempotent re-register, `InsertIntoFocusedInputText` happy / no-target paths, `g_dictationRouter` global link sentinel. Stub-mode test extended for `InsertIntoFocusedInputText` no-op + `g_dictationRouter` link symbol. Net effect: every Smatchet UI surface that flows through the localized wrapper picks up dictation automatically; the four core surfaces have belt+suspenders explicit registration so they survive widget-focus-cycle edge cases.
- Phase A — plugin shell + config schema + dictation router skeleton (PR pending). Adds `SMATCHET_WITH_WHISPER` CMake option (default ON, OFF under `SMATCHET_EMBEDDED_IN_UNREAL`), source-list bindings/stubs split for `DictationInsertionRouter_{Whisper,Stubs}.cpp`, `PUBLIC` define on `SmatchetCoreInterface`, `add_subdirectory(Plugins/Whisper)` + standalone link, `whisper.status` CLI command (`{enabled, mode, model_present, setup_completed}`), 7 additive config fields wrapped `#if defined(SMATCHET_WITH_WHISPER)` (`WhisperEnabled`, `WhisperSetupCompleted`, `WhisperSetupChoice`, `WhisperMode`, `WhisperModel`, `WhisperHotkey`, `WhisperApiKey` — DPAPI-encrypted mirroring `AiApiKey`), doctest at `tests/Source_Core/DictationInsertionRouter.test.cpp` exercising register/unregister round-trip + multi-buffer independence + `IsRecording()` default false + stub-mode no-op contract, CI sentinel job `windows-msys2-ucrt64-no-whisper` enforcing stub-drift detection.
- Phase B — cloud transcription via OpenAI Whisper API (PR pending). Adds `Plugins/Whisper/WhisperApiKeyResolve.{h,cpp}` (pure 5-row fallback decision table), `WavWriter.{h,cpp}` (pure RIFF/WAVE int16 encoder, mono+stereo, length-stable header), `WhisperApiClient.{h,cpp}` (`cpr::Post` multipart to `https://api.openai.com/v1/audio/transcriptions`, Bearer auth, response-parse pure-helper split, `AiErrorRedact` reused for 4xx body sanitisation), `WindowsAudioCapture.{h,cpp}` (WASAPI event-driven shared-mode capture, RAII unique_ptr around `IMMDeviceEnumerator` / `IMMDevice` / `IAudioClient` / `IAudioCaptureClient` / `WAVEFORMATEX` / `HANDLE` / `ComScope`, 200 ms buffer, int16/float32 → int16 mono downmix + integer decimation to 16 kHz), `whisper.transcribe-once [--file] [--seconds N] [--mode cloud|auto]` CLI command (synchronous handler — runs on CLI thread, not UI thread; comment block documents Pattern A wrap-up for Phase E hotkey path), `whisper.status` extended with `api_key_resolved` bool surfaced by the resolve helper. `DictationInsertionRouter::InsertIntoFocusedInputText(const std::string&)` signature added on both gating states (real impl + stubs) — Phase D wires it. New doctest TUs: `WhisperApiKeyResolve.test.cpp` (all 5 rows + case-sensitivity guard), `WavWriter.test.cpp` (header byte-layout + stereo block align + zero payload + invalid inputs + LE int16 round-trip), `WhisperApiPayload.test.cpp` (`ParseWhisperResponse` happy / verbose-json / empty / malformed / non-object / missing field / wrong-type / empty-string-text + endpoint URL). Static fixtures: `tests/fixtures/whisper/hello-world.wav` (1 s silence, 32044 bytes), `tests/_mocks/openai-whisper-response.json`.
- Phase E — push-to-talk hotkey + visual cue (PR #214). Adds `Plugins/Whisper/HotkeyParse.{h,cpp}` (pure descriptor parser — `Ctrl+Alt+Space` style → `(mods, vk)` pair byte-compatible with Win32 `RegisterHotKey`; canonical-order `Stringify` for round-trip), `Plugins/Whisper/GlobalHotkey_Win32.{h,cpp}` (dedicated message-pump thread; `SetWindowsHookEx WH_KEYBOARD_LL` for global capture + `RegisterHotKey` on a message-only window for in-focus dispatch; rising-edge atomic `pressed_` debounces auto-repeat; 2 s startup handshake so `Register` reports the OS install error synchronously). `DictationInsertionRouter` extended with `std::atomic<bool> recording_` + `std::atomic<float> lastPeakAmplitude_` and the `SetRecording / SetLastPeakAmplitude / GetLastPeakAmplitude` accessor trio (both TUs); the UI thread polls these lock-free for the indicator + overlay, the WhisperPlugin hook thread + WASAPI worker write them. `WindowsAudioCapture` gets a public `GetLastPeakAmplitude()` + per-chunk peak update inside the WASAPI drain loop (one branch per frame, no extra allocation). `WhisperPlugin.cpp` grows a `PhaseEState` struct (hotkey listener + capture instance + `captureActive` + `transcribeInFlight` atoms), registers `whisper.simulate-press` + `whisper.simulate-release` test-helper commands, extends `whisper.status` with `is_recording` + `hotkey_registered`, and wires the on-press / on-release workers — press starts WASAPI capture + flips `g_dictationRouter.SetRecording(true)`; release stops capture, drains PCM, dispatches transcription via `app->LaunchBackgroundTask`, and posts the result into the focused InputText via `mainThreadDispatcher.PostToMainThread`. `Source_Core/src/SmatchetWhisperOverlayUi.{h,cpp}` (new TU, source-list-conditional with the same shape as the setup banner) draws a top-right floating amplitude meter + Cancel button while recording. `Source_Core/src/SmatchetUI_MainMenu.cpp` gains a red `● REC` label in the menu bar's right-edge area (`#if defined(SMATCHET_WITH_WHISPER)` gated). `Source_Core/src/SmatchetPreferencesUi.cpp` replaces the Phase C readonly hotkey display with a "Click to rebind" capture widget — polls supported ImGui keys frame-by-frame, snapshots the modifier state, calls `Stringify` + `MarkPrefsDirty`. Localization keys (`whisper.statusBar.*`, `whisper.overlay.*`, `whisper.preferences.hotkeyRebind*`) added to `SmatchetLocalization.cpp` (en + fr). New doctest `tests/Source_Core/HotkeyParse.test.cpp` — 18 cases covering happy path, all 4 modifiers solo + alias modifiers, case-insensitivity, whitespace tolerance, F1..F24, digits, named keys, Stringify canonical order, round-trip, and error cases (empty / bare-modifier / unknown token / duplicate modifier / two non-modifier keys / stray plus).
- Phase F — Preferences Whisper tab polish + 4 deferred config fields + hot rebind + model catalog 4th entry + silence trim + auto-send-on-punctuation (PR pending). Adds `Plugins/Whisper/SilenceTrim.{h,cpp}` — pure C++14 helper exposing `TrimLeadingTrailingSilence` (peak-relative 100 ms-window gate) + `CapClipSamples` (truncate-to-N). Adds 4 additive `TrackerConfig` fields wrapped `#if defined(SMATCHET_WITH_WHISPER)`: `WhisperLanguage = "en"` (forwarded to whisper.cpp `whisper_full_params.language` + OpenAI multipart `language` part; `"auto"` / empty omit the field for autodetect), `WhisperTrim = true` (drives `TrimLeadingTrailingSilence` before transcription), `WhisperMaxClipSec = 60` (post-trim cap; clamped 0..600 on load — 0 disables), `WhisperAutoSendOnPunctuation = false` (when true + splice target is the AI Assistant input + text ends with `.`/`!`/`?`, fires the AI Send action). Extends `ModelCatalog` with a 4th entry `ggml-medium.en` (~1.5 GB, highest accuracy, CPU-heavy) surfaced only through Preferences (banner filters it out). `WhisperApiClient::Transcribe` + `WhisperLocal::Transcribe` get language-aware overloads; legacy entry points delegate. `WhisperPlugin::ReregisterHotkey(descriptor, outError)` enables live hot-rebind from Preferences without a process restart; `WhisperPlugin::InstanceForUi()` is the new singleton accessor the UI consumes. `RunTranscriptionPipeline_Worker` applies trim + cap + language before dispatching; `RunHotkeyRelease_Worker`'s UI-thread post-insertion callback checks `IsFocusedTargetAiAssistant()` + `EndsWithSentencePunctuation` + cfg toggle and fires `TriggerAiAssistantSend()`. `DictationInsertionRouter` gains `RegisterAiAssistantInputText`, `IsFocusedTargetAiAssistant`, `SetAiAssistantSendCallback`, `TriggerAiAssistantSend` on both gating TUs (stubs mirror with no-ops). `SmatchetAiAssistantUi.cpp` swaps its router register call to the new AI-flavour variant and wires a static atomic auto-send flag the next-frame draw observes + invokes the existing `dispatchSend()`. `SmatchetPreferencesUi.cpp` Whisper tab grows: live hot-rebind callsite (calls `WhisperPlugin::InstanceForUi()->ReregisterHotkey(...)` inline post-capture), "Test connection" button (worker-thread GET to `/v1/models` with the resolved key + `MainThreadDispatcher` result post-back), language Combo (16 ISO codes + `auto`), trim Checkbox, max-clip InputInt (clamped 0..600), auto-send Checkbox, "Re-run setup banner" debug button (flips `WhisperSetupCompleted=false`). `SmatchetWhisperSetupBanner.cpp` filters `ggml-medium.en` out of the radio loop. Localization keys added (en + fr) for every new label / error / hint. New doctest TUs: `tests/Source_Core/SilenceTrim.test.cpp` (8 cases — empty / pure-silence / no-silence / leading+trailing strip / zero-rate guard / `CapClipSamples` 0-disables / truncate / pass-through), `tests/Source_Core/WhisperPrefsFields.test.cpp` (4 cases — defaults / round-trip / clamp / missing-keys). Existing `ModelCatalog.test.cpp` updated for the new 4-entry layout + URL prefix invariant.
- Phase G — automated end-to-end test harness + regression gate. Adds `Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp` (new scenario gated layer 2 — source-list conditional in `CMakeLists.txt` alongside the overlay + banner, factory `RegisterFactory` site in `Source_Core/src/AppController.cpp` wrapped `#if defined(SMATCHET_WITH_WHISPER)`). Adds two test-injection seams on the WhisperPlugin layer: `WhisperPlugin::SetMockTranscription({text, delay})` / `ClearMockTranscription()` / `MockTranscriptionActive()` / `CurrentMockTranscription()` short-circuit the hotkey press/release worker pair — press skips WASAPI capture and just flips the recording flag, release schedules a `LaunchBackgroundTask` that sleeps for the requested delay and then posts `g_dictationRouter.InsertIntoFocusedInputText(text)` via `MainThreadDispatcher`. Adds a lower-level `WhisperApiClient::SetMockResponse(text)` / `ClearMockResponse()` / `MockResponseActive()` seam at the cloud client itself (short-circuits before HTTP) for any future test that wants to drive the API-client branch without burning credits — production code never sets it. The scenario flow: OnStart registers a static 256-byte test buffer with `g_dictationRouter`, arms the mock, and dispatches `whisper.simulate-press`; OnFrame dispatches `whisper.simulate-release` at frame 4 then polls the registered buffer until the mock text lands; IsDone returns true on assertion match or at frame 300; OnFinish returns `{passed, expected_text, observed_text, state, press_frame, release_frame, delay_ms, frame_limit}` and unregisters + clears the mock. Adds `scripts/dev/test-whisper-roundtrip.sh` (auto-enrolled by `test-all.sh`) — spawns Smatchet via `--spawn`, runs the scenario, parses the JSON envelope, asserts `data.passed == true`. Self-guard: probes `commands.list` for `whisper.status` and SKIPs cleanly when the binary was built with `SMATCHET_WITH_WHISPER=OFF`. Fixtures already in place from Phase B (`tests/fixtures/whisper/hello-world.wav` + `tests/_mocks/openai-whisper-response.json`).
- Phase C — local whisper.cpp wrapper + ModelDownloader + ModelCatalog + WhisperConsentGate + first-run setup banner + Preferences Whisper tab. Adds `Plugins/Whisper/WhisperLocal.{h,cpp}` (RAII `std::unique_ptr<whisper_context, whisper_free>`, sync `Transcribe(float)` + int16 overload, CPU-only threads = `hardware_concurrency()/2`, gated by sub-option `SMATCHET_WHISPER_LOCAL_BACKEND` — OFF default, see Deviations), `Plugins/Whisper/ModelCatalog.{h,cpp}` (3-entry static catalog with SHA-256 hashes pinned to the ggerganov huggingface mirror; `Find` + `All` + `IsModelPresent` pure helpers), `Plugins/Whisper/ModelDownloader.{h,cpp}` (Pattern A worker via `AppController::LaunchBackgroundTask`, resumable via `<dest>.partial` + HTTP `Range: bytes=<sz>-`, streaming SHA-256 verification via Win32 `BCryptHash(BCRYPT_SHA256_ALGORITHM)` so no new third-party dep is needed, cancel atom polled between chunks, atomic rename onto final path post-verify), `Plugins/Whisper/WhisperConsentGate.{h,cpp}` (3 predicates funnelling the 4 consent invariants — `CanCaptureMic` + `CanDownloadModel` + `CanCallCloudApi`, `NowEpochSec` helper for fake-clock tests), `Source_Core/src/SmatchetWhisperSetupBanner.{h,cpp}` (pinned ImGui banner under the menu bar, 52→120→70 px phase heights, three-button collapse → inline model-size picker → inline progress bar; auto-dismisses on `State::Complete`). `WhisperPlugin.cpp` extended with `whisper.download-model --name <id>`, `whisper.model-progress`, `whisper.cancel-download` CLI commands + local-mode branch in `whisper.transcribe-once` (auto routes per § Mode router decision tree). `whisper.status` extended with `local_backend` (ON/OFF) reflecting the sub-option state. `ConfigManager.{h,cpp}` adds `WhisperConsentTimestampSec : std::int64_t` (additive, wrapped `#if defined(SMATCHET_WITH_WHISPER)`). `SmatchetUI.cpp` calls the banner inside `#if defined(SMATCHET_WITH_WHISPER) ... #endif` immediately after the main menu bar. `SmatchetPreferencesUi.cpp` adds a "Whisper" tab with enable toggle / mode selector / model picker + Download button / hotkey readonly display / API key passworded input / 3-bullet privacy disclosure. New doctest TUs: `ModelCatalog.test.cpp` (5 cases — entry count, schema, Find hit/miss, IsModelPresent edge cases), `WhisperConsentGate.test.cpp` (4 test cases × multiple subcases — predicate states, freshness window, backwards clock, zero clock), `WhisperModeRouter.test.cpp` (5 cases for cloud / local-present / local-missing / auto-present / auto-missing). `Plugins/Whisper/CMakeLists.txt` links `bcrypt` for SHA-256, conditionally links `whisper` library + sets `SMATCHET_WHISPER_LOCAL_BACKEND=1` define when sub-option is ON. Root `CMakeLists.txt` adds the FetchContent declaration for `ggerganov/whisper.cpp@v1.7.4` inside `if(SMATCHET_WITH_WHISPER AND SMATCHET_WHISPER_LOCAL_BACKEND)`. Localization keys `whisper.banner.*` + `whisper.modelPicker.*` + `whisper.preferences.*` added to `SmatchetLocalization.cpp` (en + fr).

## Deviations from plan

- **Phase D — Command Palette TU path**: plan packet listed `Source_Core/src/SmatchetCommandPaletteUi.cpp` in the write set, but the file actually lives at `Source_Core/src/Commands/CommandPaletteUi.cpp` (per the command-system subsystem layout). Phase D edits the real path; the plan-doc table will keep both for audit-trail purposes, but the cross-reference is corrected in the implementation log entry above.
- **Phase D — `s_inputCharBuf` hoisted from function-static to namespace-static in `SmatchetAiAssistantUi.cpp`**: the panel-level explicit register / unregister requires reaching the buffer from `SmatchetDrawAiAssistantPanel` (panel-lifecycle owner) while it was originally a `static std::array<char, kInputBufCap>` inside `DrawInputAndButtons` (one stack-frame deeper). Hoisting to anonymous-namespace scope at the top of the TU is the minimal change — same lifetime, same single-instance contract, exposes the buf pointer for the dictation router without adding a getter or a singleton. The seed flag was renamed `s_seeded` → `s_inputCharBufSeeded` for clarity in the new context.
- **Phase D — explicit cursor pointer is `nullptr` everywhere**: plan packet recipe showed `g_dictationRouter.RegisterInputText(buf, cap, &cursor)` for the wrapper hook. ImGui's C++14 `InputText` surface doesn't expose a per-widget byte cursor without an `ImGuiInputTextCallback` wire-up, which the four target surfaces don't use. Phase D passes `nullptr` for the cursor in all four explicit registrations and the wrapper hook; the router splices at end-of-buffer in that mode, which is correct for the three lifecycle-bound surfaces (a chat input / palette filter / long-text editor that's just been opened all have the cursor at end-of-content). Tighter splice-at-cursor for the multiline editor lands when an `ImGuiInputTextCallback` wire-up is added (Phase E or later).
- **Phase D — `Insert(...)` body filled in alongside `InsertIntoFocusedInputText(...)`**: plan packet § 3 only named `InsertIntoFocusedInputText` for the full splice logic; the older `Insert(...)` was a Phase A scaffold logging + dropping. Both top-level entry points now share the same splice path (factored manually rather than extracted to a private helper because the doctest needs to exercise both signatures distinctly; an extraction would have to leak through the public header). Identical UTF-8 + cursor semantics; the only divergence is that `Insert` picks `entries_.front()` (a deterministic single-target fall-through useful for tests + future single-target callers) while `InsertIntoFocusedInputText` reserves the right to consult an `ImGui::GetActiveID()`-derived map once the focus-tracking work lands.

- **Plugin instantiation site**: plan packet named `Source_Core/src/AppController.cpp` as the write set entry. Established precedent (Mcp + LuaConsole) registers plugins from `Target_Standalone/main.cpp` via `PluginHost::Register(std::make_unique<...>())`, with the DX12 path doing the same from `Source_Core/src/SmatchetImGuiHost.cpp`. Phase A instantiates `WhisperPlugin` in `main.cpp` (DX12 path skipped — Whisper is gated OFF under `SMATCHET_EMBEDDED_IN_UNREAL`). Keeps the existing plugin-host contract; avoids inventing a parallel plugin lifetime inside `AppController`.
- **`add_subdirectory(Plugins/Whisper)` vs. inline target**: plan packet bullet 1 said "add_subdirectory only when option is ON" but McpPlugin / LuaConsolePlugin are declared inline in the root `CMakeLists.txt`. Phase A uses `add_subdirectory` per the packet's explicit instruction and Plugins/Whisper/CMakeLists.txt write set entry — gives Phase B+ a cleaner home for whisper.cpp / cpr / WASAPI deps without polluting the root file.
- **Phase B — synchronous `whisper.transcribe-once` handler vs. Pattern A dispatch**: plan packet § 5 said "Dispatch transcription via `app.LaunchBackgroundTask(...) + MainThreadDispatcher::PostToMainThread(...)`". CLI commands already run on a non-UI thread (`ai.send-once` precedent — `BuiltinCommands_Ai.cpp:362-540`), so wrapping the synchronous `cpr::Post` in `LaunchBackgroundTask` would just be a thread hop for no benefit. Phase B keeps the handler synchronous and documents (in `WhisperApiClient.h` + `WhisperPlugin.cpp` comment block) that the Phase E hotkey path — which IS UI-frame reachable — must use `LaunchBackgroundTask` + `PostToMainThread`. Same Pattern A target; just delayed to the surface that needs it.
- **Phase B — `InsertIntoFocusedInputText` lives on the concrete router, not on `IDictationHost`**: plan packet § 6 named the signature without specifying which type owns it. Adding the method to the pure-virtual interface would force every test mock to grow a no-op override; Phase D will already touch the interface for the focus-tracking work. Phase B lands the method on `DictationInsertionRouter` itself with a note in `IDictationHost.h` documenting the asymmetry. Stubs TU mirrors the signature so drift between `_Whisper.cpp` + `_Stubs.cpp` stays linker-detectable per the existing CI matrix.
- **Phase B — WAV resampling**: plan packet § 1 said "16 kHz mono PCM int16" without specifying how to handle the WASAPI mix-engine default (float32, device sample rate). Phase B implements an integer-decimation downsampler (drop one sample every `sourceRate / 16000` frames) plus a float32 → int16 clamping mixer. Trade-off: 44.1 kHz sources land at ~16.04 kHz which Whisper is robust to (its tokenizer pre-resamples), but a future Phase C local path will likely upgrade to a real resampler (`libsamplerate` / `soxr`). Documented inline in `WindowsAudioCapture.cpp`.
- **Phase C — whisper.cpp FetchContent gated behind `SMATCHET_WHISPER_LOCAL_BACKEND` sub-option (default OFF)**: the plan packet said the FetchContent block must land in this PR. We added the block at the documented pin (`v1.7.4`, confirmed available via `git ls-remote --tags`), but gate the actual fetch + link behind a new sub-option that defaults OFF. Rationale: open question #1 in the plan flagged the ~50 MB binary-size risk + Phase H DLL refactor; the same paragraph also called out MinGW UCRT build risk for whisper.cpp + ggml. Shipping `SMATCHET_WHISPER_LOCAL_BACKEND=ON` by default would conflate Phase C's API-surface delivery with the size/build investigation Phase H is already on the hook for. The chosen approach: `WhisperLocal.{h,cpp}` compiles unconditionally (the API surface is what the banner + Preferences + mode router need to reference), but the inference body is `#if SMATCHET_WHISPER_LOCAL_BACKEND`-gated. With the sub-option OFF, `LoadModel` returns false with "local backend not built — set SMATCHET_WHISPER_LOCAL_BACKEND=ON" and the mode router falls back to cloud per § Mode router decision tree. Flipping the sub-option ON exercises the real fetch + link; Phase H decides whether to promote it to default ON, stay opt-in, or refactor to a DLL.
- **Phase C — banner-owned ModelDownloader shared with Preferences**: the plan said the banner kicks off the download. Once the user also has the "Download" button on the Preferences Whisper tab (also Phase C), a fetch started from one surface must continue to show progress on the other. Achieved via `SmatchetWhisperSetupBanner::BannerOwnedDownloader()` returning a reference to the same static `ModelDownloader` Preferences reads. The plugin-owned CLI downloader is a separate instance to avoid a banner re-render interrupting a CLI poll on the same model.
- **Phase C — WhisperApiClient consent gate lives at the CLI handler, not inside Transcribe**: plan packet § 4 said `WhisperApiClient::Transcribe(...) calls CanCallCloudApi(cfg)`. Pushing the consent check into `Transcribe` would force every test mock (and the future hotkey path) to plumb a TrackerConfig into a function that today only takes the already-resolved API key. Phase C funnels the gate through the caller (`whisper.transcribe-once` handler reads the live config, checks the gate, then calls Transcribe). Same enforcement, smaller surface change.
- **Phase C — Win32 BCrypt for SHA-256 instead of a new third-party hash dep**: the plan said "SHA-256 verify". Adding a hash library (e.g. cryptopp, picosha2) would introduce build / link / size overhead the plugin doesn't need — Smatchet's Whisper plugin subtree is already Windows-only (WASAPI gate), so the streaming BCrypt API (`BCryptCreateHash` → `BCryptHashData` per chunk → `BCryptFinishHash` with `BCRYPT_SHA256_ALGORITHM`) is free. Linking `bcrypt` is one line in `Plugins/Whisper/CMakeLists.txt`. Non-Windows builds get an empty-digest stub (always fails verify) but cannot reach the downloader anyway.
- **Phase C — `Locales/en.json` strings folded into `SmatchetLocalization.cpp` instead of a new asset file**: the plan packet listed `Locales/en.json` as a write set entry. The existing Smatchet localization layer is a hard-coded `TranslationEntry kEntries[]` array in `Source_Core/src/SmatchetLocalization.cpp` keyed by an `SmatchetLocalization::T(key, fallback)` lookup; there is no JSON loader on the runtime path. Phase C adds the `whisper.banner.*` + `whisper.modelPicker.*` + `whisper.preferences.*` keys directly to that table (en + fr), matching the existing pattern. Introducing a JSON asset loader is its own work-stream, not blocking the banner.
- **Phase E — `Locales/en.json` strings continue to fold into `SmatchetLocalization.cpp`**: same rationale as Phase C — there is no `Locales/` directory and no JSON loader on the runtime path. The Phase E packet's write-set entry `Locales/en.json` is satisfied by the equivalent entries in `SmatchetLocalization.cpp` § kEntries (en + fr columns).
- **Phase E — `IsTrackerTransportErrorText` / hot-rebind of the live hook deferred**: the Preferences "Click to rebind" flow persists the new descriptor to `cfg.WhisperHotkey` + `MarkPrefsDirty(d)`, but does NOT call `phaseE_->hotkey.Unregister()` + re-`Register()` in the same frame. Reaching into the plugin from a UI TU would either widen the `IPlugin` contract or add a `WhisperPlugin*` accessor to `AppController`; either is bigger than Phase E warrants. The new descriptor is picked up at the next plugin OnStop / OnStart cycle (process restart). A Phase F follow-up entry tracks the hot-rebind work — `docs/backlog/agent-self-improvement/process.md` will pick it up when surfaced.
- **Phase E — `IsRecording` setter not on `IDictationHost`**: mirrors the Phase B asymmetry for `InsertIntoFocusedInputText` — the new setters (`SetRecording`, `SetLastPeakAmplitude`, `GetLastPeakAmplitude`) live on the concrete `DictationInsertionRouter` so the host abstraction stays narrow. Stubs TU mirrors the signatures so the OFF build links the same symbols (drift caught by the existing CI matrix).
- **Phase E — `RegisterHotKey` + low-level keyboard hook BOTH installed, not either-or**: the design doc bullet implied a runtime choice between in-focus + global. The Win32 reality: `RegisterHotKey` only fires for the foreground app; `WH_KEYBOARD_LL` sees everything including the foreground app. To get the same "press from anywhere" semantics in both states we install both — the LL hook fires onPress + onRelease, and the `WM_HOTKEY` handler (when Smatchet is foreground) is a no-op since the LL path already covered the press. Simpler than runtime-selecting one of them and exposes the same surface either way.
- **Phase E — hot-rebind of the live hook deferred + descriptor display does NOT auto-refresh until plugin restart**: see the "hot-rebind deferred" deviation above. Users editing the hotkey in Preferences see the new descriptor reflected in the UI immediately (next-frame seed via `s_hotkeyDisplay`), but the OS-level listener still binds the previous descriptor until the next launch. The Preferences UI does not surface this gap to the user today — flagging as a Phase F polish item.
- **Phase F — `Locales/en.json` strings continue to fold into `SmatchetLocalization.cpp`**: same rationale as Phase C / Phase E. The Phase F packet's `Locales/en.json` write-set entry is satisfied by the equivalent entries appended to `SmatchetLocalization.cpp` § kEntries (en + fr columns).
- **Phase F — `IsFocusedTargetAiAssistant` + AI Assistant register variant land on the concrete `DictationInsertionRouter`, not on `IDictationHost`**: mirrors the Phase B / Phase E asymmetry. Pushing them onto the pure-virtual interface would force every mock + the stubs TU to grow no-op overrides for a Phase-F-only feature; the concrete-class surface stays bound to the Whisper subsystem. Stubs TU mirrors the new signatures so OFF-build link drift is still caught by the CI matrix.
- **Phase F — auto-send-on-punctuation Send hand-off via a static atomic flag, not a captured-lambda dispatch**: the dictation router's UI-thread callback fires from inside `MainThreadDispatcher::PostToMainThread` (ImGui state mid-flux during dispatcher drain). Invoking `dispatchSend()` directly from the callback risks reaching into ImGui input-text state from a non-frame context. The chosen approach: the callback flips `s_pendingAutoSend` (atomic). The next AI-assistant panel draw observes the flag AFTER the InputText frame call, then invokes the existing `dispatchSend()` lambda — exactly the same path a user pressing Enter takes. One-frame deferral; no ImGui re-entry hazard.
- **Phase F — `WhisperPlugin::InstanceForUi()` singleton instead of widening `IPlugin` or `AppController`**: the Preferences hot-rebind needs a `WhisperPlugin*` from `SmatchetPreferencesUi.cpp`. Adding `FindPluginById(const char*)` to `PluginHost` would touch every host call site for one client; adding `WhisperPlugin* GetWhisperPlugin()` to `AppController` would couple it to a plugin's concrete type. The Phase F path: a process-static `g_whisperPluginInstance` pointer set in `WhisperPlugin::OnStart` and cleared in `OnStop`, exposed via `WhisperPlugin::InstanceForUi()`. Plugin-local; nullable; safe because OnStart / OnStop / UI access are strictly main-thread sequenced.
- **Phase G — mock-transcription seam lives on `WhisperPlugin` (plugin-level), not on `WindowsAudioCapture` (capture-level)**: an alternative design would have been a mock-capture interface that returns a canned PCM buffer + a mock `WhisperApiClient::Transcribe` returning the canned text, exercising more of the production code path. The chosen path short-circuits inside `RunHotkeyPress_Worker` / `RunHotkeyRelease_Worker` themselves. Trade-off: the scenario covers the press → schedule-worker → MainThreadDispatcher → InsertIntoFocusedInputText half of the pipeline (which is the "ImGui never freezes" Pillar-2 surface that's hard to verify by hand), but skips the WASAPI drain + mode-router + WhisperApiClient `cpr::Post` + JSON parse half (already covered by unit doctests + the existing `whisper.transcribe-once --file` smoke CLI path). The lower-level `WhisperApiClient::SetMockResponse` seam is added as the escape hatch for any future test that wants to exercise the cloud client path without burning credits — kept on the same module to avoid sprinkling test seams across the codebase.
- **Phase G — scenario uses static file-scope test buffer instead of allocating per run**: the dictation router stores raw `char*` pointers from `RegisterInputText`. A scenario-instance-owned `std::vector<char>` would work but introduces a "buffer disappears at scenario end" risk if `UnregisterInputText` is somehow skipped (cancel path, exception). Static storage means the router can never end up holding a dangling pointer — the buffer outlives the scenario. The 256-byte cost is negligible. Scenario zeroes the buffer in OnStart so back-to-back runs see clean state.
- **Phase G — scenario asserts via polled buffer compare, not via `MainThreadDispatcher::WaitForDrain` blocking helper**: ImGui frame-loop scenarios are designed to run at frame cadence; the runner's per-frame `Tick` already polls. Adding a blocking wait in `OnFrame` would deadlock the frame loop that's responsible for draining the dispatcher. The polling shape is the same one `cell-edit-burst` and the rest of the scenarios use.

## Verification

- Phase A:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 picks up the stubs TU; Standalone the real router).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode standalone compiles green.
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes including the new `DictationInsertionRouter.test.cpp` cases.
  - `Smatchet.exe cmd whisper.status` — returns `{"enabled": false, "mode": "auto", "model_present": false, "setup_completed": false}` on a fresh profile.
  - `bash scripts/dev/test-all.sh` — full sweep green.
  - CI job `windows-msys2-ucrt64-no-whisper` — added in the same PR; sentinel for stubs/bindings drift.
- Phase B:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps `SMATCHET_WITH_WHISPER=OFF`, never visits the new Plugins/Whisper TUs).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode build green; `InsertIntoFocusedInputText` stub linked.
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes; new test cases — `WhisperApiKeyResolve` (6), `WavWriter` (6), `WhisperApiPayload` (8) — all green.
  - `Smatchet.exe cmd whisper.status` — returns `{enabled, mode, model_present, setup_completed, api_key_resolved}` on a fresh profile; `api_key_resolved=false` until the user supplies a key.
  - `Smatchet.exe cmd whisper.transcribe-once --file tests/fixtures/whisper/hello-world.wav --mode cloud` — without a real API key, fails with `no API key available - set WhisperApiKey or AiApiKey (provider=openai)`. With a fake key, exits via the HTTP-transport / HTTP-401 branch in `WhisperApiClient::Transcribe` (transport-level, not key-resolution-level — proves the key-resolution path is no longer the blocker). End-to-end with a real key is documented as manual residue (bucket-E gap).
  - `bash scripts/dev/test-all.sh` — full sweep green.
- Phase C:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps `SMATCHET_WITH_WHISPER=OFF`; Standalone picks up the banner TU + ModelCatalog + ModelDownloader + WhisperLocal API surface).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode build green; no Plugins/Whisper TU compiled.
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes; new test cases — `ModelCatalog` (5), `WhisperConsentGate` (4 × multi-subcase), `WhisperModeRouter` (5) — all green.
  - `Smatchet.exe cmd whisper.status` — returns `{enabled, mode, model_present, setup_completed, api_key_resolved, local_backend}` on a fresh profile; `local_backend = OFF` until `SMATCHET_WHISPER_LOCAL_BACKEND=ON` is rebuilt.
  - `Smatchet.exe cmd whisper.download-model --name ggml-tiny.en` — manual gate: after stamping `WhisperConsentTimestampSec` via the banner / Preferences (or directly editing the config + Save for a smoke test), starts a worker; `whisper.model-progress` reports `{state: Downloading, bytes_received, bytes_expected}`; final rename happens only when the streaming SHA-256 matches the catalog. Network gate.
  - `Smatchet.exe cmd whisper.cancel-download` — returns `{cancelled: true}`; the worker's next chunk write returns false; `<dest>.partial` is preserved for resume.
  - `bash scripts/dev/test-all.sh` — full sweep green.
- Phase D:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps the stubs router TU; Standalone the real router with `g_dictationRouter` defined once).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode standalone compiles green; the four target surfaces' explicit `g_dictationRouter.{Register,Unregister}InputText(...)` calls resolve to no-ops through the stubs TU.
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes; 10 new doctest cases in `DictationInsertionRouter.test.cpp` cover empty / non-empty / mid-cursor splices, cursor advance chain, capacity truncation, UTF-8 codepoint boundary integrity, idempotent re-register, `InsertIntoFocusedInputText` happy / no-target paths, and the `g_dictationRouter` global link sentinel (both gating states).
  - `Smatchet.exe cmd whisper.status` — unchanged contract (Phase D doesn't surface CLI changes); still returns the Phase C field set on a fresh profile.
  - `bash scripts/dev/test-all.sh` — full sweep green.
  - Manual residue (bucket-E rig wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`) but no mic-hardware mock): end-to-end mic-capture → transcription → text appears in focused InputText on each of the four target surfaces (AI Assistant chat input, long-text field editor modal, Command Palette filter, any focused InputText routed via the wrapper).
- Phase E:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps `SMATCHET_WITH_WHISPER=OFF`; Standalone picks up the new HotkeyParse + GlobalHotkey_Win32 + Overlay TUs + Phase E surface on the router).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode build green; mic indicator + overlay drop out cleanly (call sites are `#if defined(SMATCHET_WITH_WHISPER)` gated; the new `SetRecording / SetLastPeakAmplitude / GetLastPeakAmplitude` symbols exist on the stubs TU so the router header surface is identical across gating states).
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes; new `HotkeyParse.test.cpp` adds 18 cases.
  - `Smatchet.exe cmd whisper.status` — returns the extended Phase E blob with `is_recording` (bool, false until a hotkey press) + `hotkey_registered` (bool, true once `GlobalHotkey_Win32::Register` succeeds in `WhisperPlugin::OnStart`).
  - `Smatchet.exe cmd whisper.simulate-press` then `Smatchet.exe cmd whisper.simulate-release` — exercise the press / release worker paths from the CLI without a hardware key. Without a real mic the captured PCM is empty + the pipeline returns "0 PCM samples captured (release within < 1 chunk)" cleanly. The `is_recording` flag flips true after press + back to false after release (poll `whisper.status` between to observe).
  - `bash scripts/dev/test-all.sh` — full sweep green.
  - Manual residue (bucket-E rig wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`) but no mic-hardware mock): (1) end-to-end push-to-talk with a real mic — hold hotkey, speak, release, see text appear in focused InputText. Recipe: launch `build/ninja-iter-msvc/Smatchet.exe`, enable Whisper via Preferences (sets `WhisperEnabled` + `WhisperSetupCompleted` + `WhisperConsentTimestampSec`), focus the AI Assistant chat input, hold `Ctrl+Alt+Space`, speak, release. (2) Visual cue latency < 100 ms from hotkey press to mic indicator visible — eyeball test at 144 Hz. (3) Hotkey rebind UX in Preferences — eyeball check that the "Click to rebind" button captures Ctrl+Alt+F8 (etc.) and the new descriptor persists across launch.
- Phase F:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps `SMATCHET_WITH_WHISPER=OFF`; Standalone picks up `SilenceTrim.cpp`, the 4-entry catalog, the language overloads on `WhisperApiClient` / `WhisperLocal`, the new router AI-Assistant methods, and the Preferences Phase F rows).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode build green; the new router signatures (`RegisterAiAssistantInputText`, `IsFocusedTargetAiAssistant`, `SetAiAssistantSendCallback`, `TriggerAiAssistantSend`) link cleanly through the stubs TU.
  - `cmake --build --preset ninja-test-msvc && cd build/ninja-test-msvc && ctest` — `smatchet_tests` passes; new + extended cases: `SilenceTrim` (8), `WhisperPrefsFields` (4), `ModelCatalog` (updated to 4-entry layout + URL prefix invariant).
  - `Smatchet.exe cmd whisper.status` — unchanged contract; the Phase F config-field additions are observable via `Smatchet.exe cmd config.show whisper_language` / `whisper_trim` / `whisper_max_clip_sec` / `whisper_auto_send_on_punctuation` (or by re-reading `smatchet_config.json` after a Save).
  - `bash scripts/dev/test-all.sh` — 111 pass / 10 known env-only failures (matches the baseline; no new failures introduced).
  - Manual residue (bucket-E gap; covered by Phase G + the existing manual recipes): (1) Preferences Whisper tab visual eyeball — language Combo, trim Checkbox, max-clip InputInt with clamp behaviour at 600, auto-send Checkbox, Test connection button, Re-run setup banner button. (2) Hotkey hot-rebind end-to-end — capture Ctrl+Alt+F8 (etc.) and verify the global hook fires on the NEW combo immediately, without restarting Smatchet. (3) Test connection button against a real OpenAI API key — Connected vs HTTP 401 reporting. (4) Auto-send-on-punctuation flow — focus the AI Assistant input, press the hotkey, dictate "hello world.", release; the AI Assistant Send action fires.
- Phase G:
  - `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 keeps `SMATCHET_WITH_WHISPER=OFF`; Standalone picks up `WhisperDictationScenario.cpp` via the source-list conditional alongside the overlay + banner).
  - `cmake --preset ninja-iter-msvc -DSMATCHET_WITH_WHISPER=OFF && cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — stubs-mode build green; `WhisperDictationScenario.cpp` is excluded from CORE_SOURCES + the `RegisterFactory` call is `#if defined(SMATCHET_WITH_WHISPER)` gated so there's no dangling `MakeWhisperDictationScenario` extern.
  - `Smatchet.exe cmd scenario.list` — includes `whisper-dictation-roundtrip` in the alphabetised list on the ON build; absent on the OFF build.
  - `Smatchet.exe cmd scenario.run --name=whisper-dictation-roundtrip --spawn` — completes with `{ok: true, data: {passed: true, expected_text: "the quick brown fox jumps over the lazy dog", observed_text: "the quick brown fox jumps over the lazy dog", state: "Asserted", ...}}`; exit code 0.
  - `bash scripts/dev/test-whisper-roundtrip.sh` — runs the scenario, parses the JSON, exits 0 against an ON build.
  - `bash scripts/dev/test-whisper-roundtrip.sh` against an OFF build — SKIPs cleanly (exit 0 + `SKIP: whisper.status not registered in this exe`).
  - `bash scripts/dev/test-all.sh` — full sweep green, including the new whisper-roundtrip runner.
  - Manual residue (bucket-E rig wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`) but no mic-hardware mock): (1) Real-mic end-to-end — `Ctrl+Alt+Space` hold + speak + release on a focused InputText; text appears (covered by recipes in Phase D + Phase E manual residue). (2) Visual eyeball polish of the four UI surfaces — setup banner, Preferences tab, mic indicator, amplitude overlay. (3) Hotkey rebind round-trip across app launches with a real keyboard combo — descriptor persists in `smatchet_config.json` and the new global hook fires on the new combo after restart.
