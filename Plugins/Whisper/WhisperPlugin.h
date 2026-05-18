#pragma once

// WhisperPlugin — push-to-talk dictation plugin. See
// docs/design/whisper-dictation.md for the full plan.
//
// Phase A: shell only. The plugin registers a single `whisper.status` CLI
// command that reports the static feature flags. No audio capture, no
// transcription, no UI surface — Phases B (cloud client) / C (local +
// model download) / D (insertion targets) / E (hotkey + visual cue) layer
// onto this skeleton without re-plumbing the plugin host contract.
//
// Phase E: the plugin owns a `GlobalHotkey_Win32` listener + a
// `WindowsAudioCapture` instance + a worker-side recording state machine.
// onPress dispatches capture-start + UI indicator on; onRelease dispatches
// capture-stop + transcription + (eventually) Insert. The hotkey hook
// callbacks are worker-thread entry points — they post UI-thread state via
// `AppController::mainThreadDispatcher.PostToMainThread` and never touch
// ImGui directly.
//
// Entire Plugins/Whisper/ subtree is CMake-conditional (Layer 6 gating); the
// translation unit only exists when SMATCHET_WITH_WHISPER=ON. UI / call-site
// callers reach the dictation router through IDictationHost, which has a
// stub implementation for the OFF build.

#include "IPlugin.h"

#include <memory>

class WhisperPlugin : public IPlugin {
  public:
    WhisperPlugin();
    ~WhisperPlugin() override;

    const char* Id() const override { return "whisper"; }

    void OnStart(AppController& app) override;
    void OnStop() override;

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
