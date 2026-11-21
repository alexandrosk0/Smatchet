#pragma once

// The per-dialog state of the dialog scope (Ui/SmatchetDialogScope.h), in its own ImGui-free header so a
// class whose header stays ImGui-free can hold one per instance.

namespace smatchet {
namespace ui {

/// Per-dialog state, owned by the caller: a file-scope static next to the dialog, or a member when the
/// class that draws the dialog can have more than one instance (a static shared by two instances would be
/// reset by the one whose dialog is closed, every frame).
struct DialogScopeState {
    bool Open = false;                   ///< BeginDialog returned true last time it was called
    bool FocusPending = false;           ///< the opening focus has not been placed yet
    bool OwnedKeyboardLastFrame = false; ///< for ResolveEscape: the Esc that closed a popup above stays there
    bool FieldActiveLastFrame = false;   ///< a field of the dialog held the caret at the end of last frame
};

} // namespace ui
} // namespace smatchet
