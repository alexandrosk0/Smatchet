# whisper-dictation — push-to-talk dictation with local + cloud transcription

## Status

- **Phase**: planning (this doc)
- **Originating request**: user — "Make a plan on how to re-add whisper. Check the git history"
- **Git history result**: zero whisper / voice / speech-to-text history in repo (`git log --all --grep`, deleted-file diff, branch + reflog scans all empty). Feature has never existed here despite the "re-add" framing. Plan written from scratch.
- **User-confirmed scope** (via AskUserQuestion, 2026-05-17):
  - Backend mode: **Both — local default, cloud fallback**.
  - Insertion targets: **all four** — focused ImGui InputText, AI Assistant chat box, grid long-text editor, Command Palette.

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
| `docs/PERF_WORKFLOW.md`, `agents/*.md`, plan doc | No gating — documentation

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

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target green (DX12 build picks up stubs TU).
- `cmake -DSMATCHET_WITH_WHISPER=OFF --build ...` — Standalone-with-Whisper-off compiles green using stubs TU.
- `cmake --build --preset ninja-test-msys2 && ctest` — router unit tests pass for both on and off configurations.
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

### Phase C — Local whisper.cpp integration

**Write set + gating shape**:

| File | Action | Gating layer |
|---|---|---|
| `CMakeLists.txt` | MOD (FetchContent `ggerganov/whisper.cpp` at a pinned tag, guarded `if(SMATCHET_WITH_WHISPER)`) | L1 |
| `Plugins/Whisper/WhisperLocal.{h,cpp}` | NEW (wraps `whisper_init_from_file_with_params` + `whisper_full`; `whisper_context*` under `std::unique_ptr`) | L6 |
| `Plugins/Whisper/WhisperPlugin.cpp` | MOD (mode router selects local vs cloud per § Mode router decision tree) | L6 |
| `Plugins/Whisper/ModelDownloader.{h,cpp}` | NEW (Pattern A worker for downloading `ggml-base.en.bin` from huggingface; resumable, SHA-256 verified) | L6 |
| `Source_Core/src/SmatchetPreferencesUi.cpp` | MOD (Whisper tab with "Download model" button + progress bar, wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`) | L5 |
| `tests/Source_Core/WhisperModeRouter.test.cpp` | NEW (pure: mode decision tree) | L1 |

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
  run: cmake -DSMATCHET_WITH_WHISPER=OFF --preset ninja-iter-msys2 && cmake --build --preset ninja-iter-msys2
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
Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp
CMakeLists.txt
tests/CMakeLists.txt
Locales/en.json
tests/Source_Core/DictationInsertionRouter.test.cpp
tests/Source_Core/WhisperApiPayload.test.cpp
tests/Source_Core/WhisperApiKeyResolve.test.cpp
tests/Source_Core/WavWriter.test.cpp
tests/Source_Core/WhisperModeRouter.test.cpp
tests/Source_Core/ModelCatalog.test.cpp
tests/fixtures/hello-world.wav
tests/_mocks/openai-whisper-response.json
scripts/dev/test-whisper-roundtrip.sh
.github/workflows/build-and-test.yml
EOF

AGENT_ID=orchestrator \
LOCK_BRANCH=feat/whisper-dictation \
LOCK_PLAN=docs/design/whisper-dictation.md \
bash scripts/dev/lock-claim.sh whisper-dictation /tmp/whisper-write-set
```

Each phase PR body must include `lock-slug: whisper-dictation` so `.github/workflows/lock-cleanup.yml` auto-releases on final merge. For per-phase locking, use a phased slug (`whisper-dictation-phase-a`, etc.) and release each phase independently — preferred for this multi-PR work.

## Implementation log

_(populated as phases ship)_

## Deviations from plan

_(populated as decisions diverge from the plan above; each item one-line rationale)_

## Verification

_(populated per phase: what was tested + result — passed / failed / not-run)_
