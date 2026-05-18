#pragma once

// Concrete declaration for the dictation insertion router. Always-compiled
// header (declarations only) — the implementation TU is chosen at CMake time:
//   - DictationInsertionRouter_Whisper.cpp when SMATCHET_WITH_WHISPER=ON
//   - DictationInsertionRouter_Stubs.cpp   when SMATCHET_WITH_WHISPER=OFF
// Both TUs export the exact same symbols; the CI matrix builds both gating
// states (see § CI matrix in docs/design/whisper-dictation.md) so drift
// between them is caught at link time, not at runtime.

#include "IDictationHost.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class DictationInsertionRouter : public IDictationHost {
  public:
    DictationInsertionRouter();
    ~DictationInsertionRouter() override;

    DictationInsertionRouter(const DictationInsertionRouter&) = delete;
    DictationInsertionRouter& operator=(const DictationInsertionRouter&) = delete;

    void RegisterInputText(char* buf, std::size_t cap, int* cursor) override;
    void UnregisterInputText(char* buf) override;
    void Insert(const std::string& text) override;
    bool IsRecording() const override;

    /// UI-thread-only entry point: splice `text` into whichever registered
    /// InputText buffer currently has ImGui focus. No-op when no registered
    /// InputText is focused. Phase B adds the signature so Phase D surfaces
    /// (focused InputText, AI Assistant chat, grid long-text editor, Command
    /// Palette) can wire up without re-touching the router header; full
    /// splice-at-cursor + ImGui::GetActiveID() probing lands in Phase D.
    void InsertIntoFocusedInputText(const std::string& text);

    /// Count of currently-registered InputText buffers. Test-only convenience.
    std::size_t RegisteredCountForTest() const;

    /// Live mic toggle. Set by the WhisperPlugin hotkey hook thread on
    /// press / release; read by the UI thread (status-bar indicator + overlay
    /// amplitude meter). Backed by `std::atomic<bool>` so the hook thread can
    /// flip the flag without a mutex round-trip — the UI never blocks on
    /// `IsRecording()`. Mirrors the SetLastPeakAmplitude / GetLastPeakAmplitude
    /// pair below — the producer is always the capture thread, the consumer
    /// is always the UI thread.
    void SetRecording(bool active);

    /// Worker -> UI hand-off for the per-frame amplitude meter. The capture
    /// thread writes the peak |sample| / 32768.0 seen in the last drained
    /// chunk; the overlay reads on every frame. `std::atomic<float>` is
    /// lock-free on every Smatchet host (Win32 / MSYS2 UCRT), so this never
    /// reaches the WASAPI mutex from the UI thread.
    void SetLastPeakAmplitude(float peak0to1);

    /// Last peak amplitude reported by the capture thread, clamped to [0, 1].
    /// Returns 0 when no capture has run since the last `Reset` / startup.
    float GetLastPeakAmplitude() const;

    /// Register `buf` as the AI Assistant chat-input buffer. Same shape as
    /// `RegisterInputText` but additionally records the buf pointer in an
    /// `aiAssistantBuf_` slot so `IsFocusedTargetAiAssistant` can answer
    /// truthfully. Phase F — wires the auto-send-on-punctuation flow without
    /// teaching the generic InputText hook about the AI Assistant.
    void RegisterAiAssistantInputText(char* buf, std::size_t cap, int* cursor);

    /// Returns true when the splice target the next `InsertIntoFocusedInputText`
    /// would use is the AI Assistant chat input. Today the router picks the
    /// first registered entry, so this returns whether that entry's buffer
    /// pointer matches the registered AI Assistant buffer. Cheap; safe to
    /// call after every insertion on the UI thread.
    bool IsFocusedTargetAiAssistant() const;

    /// Register a one-time send callback the router fires on auto-send. The
    /// callback runs on the UI thread (caller is responsible for being on
    /// the UI thread before invoking via the worker → MainThreadDispatcher
    /// path). Stored under the same mutex as `entries_`; copying a
    /// `std::function` is cheap.
    void SetAiAssistantSendCallback(std::function<void()> cb);

    /// Trigger the AI Assistant send callback, if one is registered. Used by
    /// the post-insertion auto-send-on-punctuation path. No-op when no
    /// callback was registered (e.g. AI panel never opened this session).
    void TriggerAiAssistantSend();

  private:
    struct Entry {
        char* Buf = nullptr;
        std::size_t Cap = 0;
        int* Cursor = nullptr;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;

    // Phase F — AI Assistant identification + auto-send hook. The router stores
    // the AI Assistant's char-buffer pointer separately from the generic
    // entries list; equality on the buf pointer is the cheap "is the focused
    // splice target the AI Assistant?" answer.
    char* aiAssistantBuf_ = nullptr;
    std::function<void()> aiAssistantSendCb_;

    // Set/read across threads — no mutex round-trip on the UI poll path.
    std::atomic<bool> recording_{false};
    std::atomic<float> lastPeakAmplitude_{0.0f};
};

/// Process-wide router instance. Defined exactly once — in
/// DictationInsertionRouter_Whisper.cpp when SMATCHET_WITH_WHISPER=ON, or in
/// DictationInsertionRouter_Stubs.cpp when OFF. UI call sites (the
/// SmatchetLocalizedImGui::InputText wrapper plus the four explicit
/// registration sites — AI Assistant input, long-text editor buffer, Command
/// Palette filter, focused-InputText catch-all) reach the router through this
/// symbol; the stubs TU exports the same name as a no-op shell so call sites
/// need zero per-callsite `#if defined(SMATCHET_WITH_WHISPER)` guards.
extern DictationInsertionRouter g_dictationRouter;
