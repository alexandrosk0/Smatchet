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
// Entire Plugins/Whisper/ subtree is CMake-conditional (Layer 6 gating); the
// translation unit only exists when SMATCHET_WITH_WHISPER=ON. UI / call-site
// callers reach the dictation router through IDictationHost, which has a
// stub implementation for the OFF build.

#include "IPlugin.h"

class WhisperPlugin : public IPlugin {
  public:
    WhisperPlugin();
    ~WhisperPlugin() override;

    const char* Id() const override { return "whisper"; }

    void OnStart(AppController& app) override;
    void OnStop() override;
};
