#pragma once

// WhisperPlugin — push-to-talk dictation plugin. See
// docs/plans/shipped/whisper-dictation.md for the full plan.
// Phase A: shell only. The plugin registers a single `whisper.status` CLI
// command that reports the static feature flags. No audio capture, no
// transcription, no UI surface — Phases B (cloud client) / C (local +
// model download) / D (insertion targets) / E (hotkey + visual cue) layer
// onto this skeleton without re-plumbing the plugin host contract.
// Phase E: the plugin owns a `GlobalHotkey_Win32` listener + a
// `WindowsAudioCapture` instance + a worker-side recording state machine.
// onPress dispatches capture-start + UI indicator on; onRelease dispatches
// capture-stop + transcription + (eventually) Insert. The hotkey hook
// callbacks are worker-thread entry points — they post UI-thread state via
// `AppController::PostToMainThread` and never touch
// ImGui directly.
// Entire Source/Plugins/Whisper/ subtree is CMake-conditional (Layer 6 gating); the
// translation unit only exists when SMATCHET_WITH_WHISPER=ON. UI / call-site
// callers reach the dictation router through IDictationHost, which has a
// stub implementation for the OFF build.

#include "IPlugin.h"

#include <chrono>
#include <memory>
#include <string>

class WhisperPlugin : public IPlugin {
  public:
    WhisperPlugin();
    ~WhisperPlugin() override;

    const char* Id() const override { return "whisper"; }

    void OnStart(AppController& app) override;
    void OnStop() override;

    /// Test-injection seam — when set, the hotkey press/release path bypasses
    /// the real WASAPI capture loop and the cloud / local transcription
    /// backends entirely. On release, the plugin spawns a worker that sleeps
    /// for `delay` then posts `g_dictationRouter.InsertIntoFocusedInputText(text)`
    /// onto the UI thread via `MainThreadDispatcher`. Production code never
    /// sets this — it exists for the `whisper-dictation-roundtrip` scenario
    /// (Source/Core/src/Commands/Scenarios/WhisperDictationScenario.cpp) so
    /// regression tests can exercise the press → capture → transcribe → insert
    /// pipeline end-to-end without mic hardware or network credits. Process-
    /// wide singleton (one mock at a time); thread-safe (mutex-guarded copy).
    struct MockTranscription {
        std::string text;
        std::chrono::milliseconds delay{50};
    };
    static void SetMockTranscription(const MockTranscription& mock);
    static void ClearMockTranscription();
    static bool MockTranscriptionActive();
    static MockTranscription CurrentMockTranscription();

    /// Phase F — hot-rebind the global push-to-talk hotkey at runtime.
    /// `descriptor` is a HotkeyParse-compatible string (e.g. "Ctrl+Alt+Space");
    /// the existing hook is unregistered first, then the new descriptor is
    /// parsed + re-registered. Returns false (and sets `outError`) on parse
    /// failure / register failure; the previous hook stays unregistered in
    /// that case so a bad rebind leaves the user without push-to-talk rather
    /// than firing on the stale combo. Safe to call from the UI thread
    /// (Preferences); the underlying Win32 hook re-installation is fast.
    bool ReregisterHotkey(const std::string& descriptor, std::string& outError);

    /// Phase F — process-wide accessor used by the Preferences UI to call
    /// ReregisterHotkey without threading the plugin pointer through
    /// AppController. Returns the live instance set during OnStart, or
    /// nullptr when the plugin is not currently constructed (gating-OFF
    /// build, OnStop already ran, or before OnStart). The stubs TU does not
    /// link this symbol; UI call sites guard with `#if defined(SMATCHET_WITH_WHISPER)`.
    static WhisperPlugin* InstanceForUi();

    // Phase E state — global hotkey + capture lifetime + worker-callback
    // bookkeeping. Held behind a forward-declared struct so this header stays
    // free of Win32 + WASAPI includes; the definition lives in WhisperPlugin.cpp.
    // Public so the anon-namespace worker helpers in the .cpp can name the
    // type when reaching members; consumer code outside the plugin TU has no
    // reason to touch it.
    struct PhaseEState;

  private:
    std::unique_ptr<PhaseEState> phaseE_;
};
