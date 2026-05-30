#pragma once

// Pure-virtual contract for the dictation insertion router. Always-compiled —
// no Whisper deps, no SMATCHET_WITH_WHISPER gate. UI translation units register
// their focused InputText buffer through this interface so any of the four
// dictation insertion targets (focused InputText, AI Assistant chat box, grid
// long-text editor, Command Palette) can receive transcribed text without
// per-call-site ifdefs.
// Two concrete implementations live under Source/Core/src/ and are swapped via
// CMake (mirror of the AppController_LuaBindings.cpp / AppController_LuaStubs.cpp
// split):
//   - DictationInsertionRouter_Whisper.cpp — real registry; tracks buffers
//     and (in future phases) splices transcribed text at cursor.
//   - DictationInsertionRouter_Stubs.cpp   — same symbols, all no-ops.
// IsRecording() returns false in stub mode and in router-default state; once
// the audio-capture path lands (Phase B+) the real impl reflects live state.

#include <cstddef>
#include <string>

class IDictationHost {
  public:
    virtual ~IDictationHost() = default;

    /// Register a focused ImGui InputText buffer with the router. `buf` and
    /// `cursor` must remain valid for the lifetime of the focused widget;
    /// UnregisterInputText is called on blur or widget destruction. `cap` is
    /// the buffer capacity in bytes (matches ImGui's char* / size convention).
    /// Passing a null `buf` or zero `cap` is a no-op. Re-registering an
    /// already-tracked `buf` updates `cap` / `cursor` in place — never duplicates.
    virtual void RegisterInputText(char* buf, std::size_t cap, int* cursor) = 0;

    /// Drop the registration for `buf`. No-op when `buf` is null or unknown.
    virtual void UnregisterInputText(char* buf) = 0;

    /// Splice transcribed text into the active target at cursor. Phase A is a
    /// minimal scaffold — full UTF-8-safe insertion logic lands with Phase D.
    virtual void Insert(const std::string& text) = 0;

    /// Live mic state. False in stub-mode + router-default state; true between
    /// Whisper-plugin Start / Stop boundaries (Phase E).
    virtual bool IsRecording() const = 0;
};

// Note: DictationInsertionRouter (concrete type in DictationInsertionRouter.h)
// also exposes InsertIntoFocusedInputText(const std::string&) for the
// UI-thread post-transcription path. That method is not on IDictationHost
// because the host abstraction is a Phase D extension point — Phase B keeps
// the interface stable while still landing the concrete signature on the
// router class so call sites can wire up.
