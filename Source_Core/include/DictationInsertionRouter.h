#pragma once

// Concrete declaration for the dictation insertion router. Always-compiled
// header (declarations only) — the implementation TU is chosen at CMake time:
//   - DictationInsertionRouter_Whisper.cpp when SMATCHET_WITH_WHISPER=ON
//   - DictationInsertionRouter_Stubs.cpp   when SMATCHET_WITH_WHISPER=OFF
// Both TUs export the exact same symbols; the CI matrix builds both gating
// states (see § CI matrix in docs/design/whisper-dictation.md) so drift
// between them is caught at link time, not at runtime.

#include "IDictationHost.h"

#include <cstddef>
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

  private:
    struct Entry {
        char* Buf = nullptr;
        std::size_t Cap = 0;
        int* Cursor = nullptr;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};
