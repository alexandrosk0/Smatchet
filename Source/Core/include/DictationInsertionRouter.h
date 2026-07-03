#pragma once

// Concrete declaration for the dictation insertion router. Always-compiled
// header (declarations only) — the implementation TU is chosen at CMake time:
//   - DictationInsertionRouter_Whisper.cpp when SMATCHET_WITH_WHISPER=ON
//   - DictationInsertionRouter_Stubs.cpp   when SMATCHET_WITH_WHISPER=OFF
// Both TUs export the exact same symbols; the CI matrix builds both gating
// states (see § CI matrix in docs/plans/shipped/whisper-dictation.md) so drift
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

    /// Like `RegisterInputText` but additionally records the ImGui item id
    /// (`ImGui::GetItemID()` for the just-drawn widget). Phase F+ uses the id
    /// so `InsertIntoFocusedInputText` can pick the *actually-focused* entry
    /// via `ImGui::GetActiveID()` instead of unconditionally splicing into
    /// the first registered one. `itemId` is `unsigned int` to avoid pulling
    /// `imgui.h` into this header — values come from `ImGui::GetItemID()`
    /// which returns `ImGuiID` (a `typedef unsigned int`).
    void RegisterInputTextWithItemId(char* buf, std::size_t cap, int* cursor, unsigned int itemId);

    /// UI-thread variant of `InsertIntoFocusedInputText` that takes the
    /// caller's snapshot of `ImGui::GetActiveID()`. Prefers the registered
    /// entry whose `ItemId` matches `activeId` and falls back to the first
    /// registered entry when no match exists (the legacy contract).
    void InsertIntoFocusedInputText(const std::string& text, unsigned int activeId);

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

    /// Reflects whether a transcription worker is currently in flight (i.e.
    /// the user has released the hotkey, audio is queued, but text has not
    /// yet been spliced). UI uses this to draw a "Transcribing..." indicator
    /// next to the existing "REC" red text — closes the visual gap between
    /// recording stop and text insertion (multiple seconds for local-model
    /// inference). Backed by `std::atomic<bool>` so workers can flip it
    /// without taking the entries mutex.
    bool IsTranscribing() const;

    /// Set/clear the transcription-in-flight flag. Producer is the
    /// WhisperPlugin release worker (sets true on dispatch, false on
    /// post-back); consumer is the UI thread polling for the indicator.
    void SetTranscribing(bool active);

    /// Test-and-clear an ImGui item id that needs its `ImGuiInputTextState`
    /// reloaded from `buf` on the next frame. Set by
    /// `InsertIntoFocusedInputText` whenever a splice lands while the target
    /// widget is currently the active ImGui item — ImGui caches the buffer
    /// contents in `state->TextA` and ignores the external `buf` unless
    /// `state->WantReloadUserBuf` is true (see imgui_widgets.cpp:4821), so a
    /// splice into a focused InputText would otherwise be silently overwritten
    /// when InputText runs later in the same frame. The AI Assistant draw
    /// (and any other caller hosting a focused InputText splice target) drains
    /// this on every frame and, when non-zero, calls
    /// `ImGui::GetInputTextState(id)->ReloadUserBufAndMoveToEnd()` BEFORE the
    /// InputText call. Returns 0 when no reload is pending.
    unsigned int ConsumePendingReloadItemId();

  private:
    struct Entry {
        char* Buf = nullptr;
        std::size_t Cap = 0;
        int* Cursor = nullptr;
        // ImGui::GetItemID() of the widget that registered this buf, or 0 for
        // entries registered through the legacy `RegisterInputText(buf, cap, cursor)`
        // call. `InsertIntoFocusedInputText(text, activeId)` prefers a matching
        // ItemId over the first-registered fallback.
        unsigned int ItemId = 0;
    };

    /// Splice `text` into entry `e` at its cursor (or end), UTF-8-safe and capacity-bounded,
    /// then arm pendingReloadItemId_ so the focused InputText reloads. Caller holds mutex_.
    void SpliceTextIntoEntry(Entry& e, const std::string& text, bool usedShadowTarget);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;

    // "Sticky shadow" of the most-recently-focused widget. The wrapper hook
    // in SmatchetLocalizedImGui calls UnregisterInputText every frame the
    // widget is blurred — that's correct for the per-frame entries_ vector
    // but causes silent text-drop when a multi-second transcription pipeline
    // finishes AFTER the wrapper has already blurred the widget. The shadow
    // mirrors the last register so InsertIntoFocusedInputText can fall back
    // to it when entries_ is empty / activeId no longer matches. Cleared
    // only when the shadow's buf is the one being explicitly unregistered
    // (panel-close paths) so dangling-pointer aliasing is impossible.
    //
    // CPP_CODE_AUDIT.md #21: this invariant only holds because every current
    // registration site (AI Assistant chat input, long-text editor buffer,
    // Command Palette filter, focused-InputText catch-all) uses a buffer whose
    // lifetime is process-static. If a future caller ever registers a non-static
    // (heap/stack) buffer WITHOUT calling UnregisterInputText before that buffer
    // is freed, the shadow fallback can splice into freed memory. Accepted as
    // latent/unreachable rather than fixed with a validity-token mechanism
    // (would require an IDictationHost interface change for a path zero current
    // callers exercise) — see docs/plans/active/cpp-code-audit-remediation.md
    // § Deviations. Any new non-static-buffer registration site MUST call
    // UnregisterInputText in its owner's destructor, not just on blur.
    char* shadowBuf_ = nullptr;
    std::size_t shadowCap_ = 0;
    int* shadowCursor_ = nullptr;
    unsigned int shadowItemId_ = 0;

    // Phase F — AI Assistant identification + auto-send hook. The router stores
    // the AI Assistant's char-buffer pointer separately from the generic
    // entries list; equality on the buf pointer is the cheap "is the focused
    // splice target the AI Assistant?" answer.
    char* aiAssistantBuf_ = nullptr;
    std::function<void()> aiAssistantSendCb_;

    // Set/read across threads — no mutex round-trip on the UI poll path.
    std::atomic<bool> recording_{false};
    std::atomic<bool> transcribing_{false};
    std::atomic<float> lastPeakAmplitude_{0.0f};

    // Set by `InsertIntoFocusedInputText` when a splice lands into a buf whose
    // owning widget is currently active in ImGui (the typical Whisper-dictation
    // path — the AI Assistant chat input is focused while the user holds the
    // hotkey). Drained by `ConsumePendingReloadItemId` on the UI thread the
    // following frame so the InputText draw can call
    // `ReloadUserBufAndMoveToEnd()` and pick up the splice. Atomic so the
    // splice path (which already holds entries mutex_) can set it without an
    // additional lock; drain is single-reader on the UI thread.
    std::atomic<unsigned int> pendingReloadItemId_{0u};
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
