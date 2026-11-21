#pragma once

#include <climits>
#include <cstddef>
#include <cstring>
#include <string>

/// Keyboard equivalents for widget-local pointer gestures: the one-shot requests the ui.list.* and
/// ui.splitter.* commands post, and the focused widget that owns the gesture takes. No ImGui here; the
/// UI-thread instance and the focus test live in Ui/GestureKeys.h.
///
/// A request lives for the frame it was posted in and the next one. A keybinding fires inside the frame
/// (SmatchetUI::dispatchKeybindings) and an MCP or Lua dispatch lands in the dispatcher drain at the top of
/// it, so the widgets drawn later that frame see either. The grace frame covers a widget drawn before the
/// dispatch in frame order. A request no focused widget takes expires, so a key pressed with no list or
/// splitter focused does nothing.
namespace smatchet {
namespace ui {

enum class GestureKey : int {
    ListMoveUp = 0, ///< ui.list.move_up: move the selected row up one place
    ListMoveDown,   ///< ui.list.move_down: move the selected row down one place
    ListRemove,     ///< ui.list.remove: remove the selected row, as the list's own Delete / Remove button does
    SplitterGrow,   ///< ui.splitter.grow: move the divider toward the bottom / right (first pane grows)
    SplitterShrink, ///< ui.splitter.shrink: move the divider toward the top / left (first pane shrinks)
    Count
};

class GestureKeyLatch {
  public:
    GestureKeyLatch() { Clear(); }

    /// Records a request for `key` in `frame`. A second post replaces the first: one press, one move.
    void Post(GestureKey key, int frame) {
        const std::size_t i = Index(key);
        if (i < kCount) {
            postedFrame_[i] = frame;
        }
    }

    /// True once for a request posted in `frame` or `frame - 1`, which it consumes. A request from an
    /// earlier frame is dropped, and so is one stamped later than `frame` (a frame counter reset).
    bool Take(GestureKey key, int frame) {
        const std::size_t i = Index(key);
        if (i >= kCount || postedFrame_[i] == kNone) {
            return false;
        }
        const int posted = postedFrame_[i];
        postedFrame_[i] = kNone;
        return posted == frame || (frame != INT_MIN && posted == frame - 1);
    }

    /// Whether a live request for `key` is waiting in `frame`, without taking it.
    bool Pending(GestureKey key, int frame) const {
        const std::size_t i = Index(key);
        if (i >= kCount || postedFrame_[i] == kNone) {
            return false;
        }
        const int posted = postedFrame_[i];
        return posted == frame || (frame != INT_MIN && posted == frame - 1);
    }

    void Clear() {
        for (std::size_t i = 0; i < kCount; ++i) {
            postedFrame_[i] = kNone;
        }
    }

  private:
    static constexpr std::size_t kCount = static_cast<std::size_t>(GestureKey::Count);
    static constexpr int kNone = INT_MIN;

    static std::size_t Index(GestureKey key) {
        const int k = static_cast<int>(key);
        return k < 0 ? kCount : static_cast<std::size_t>(k);
    }

    int postedFrame_[kCount];
};

/// The commands that act only on the focused widget (ui.list.*, ui.splitter.*). A dialog that mutes global
/// shortcuts while it is open keeps these live: they reach nothing outside the widget that has the keyboard,
/// and the lists and splitters they serve sit inside dialogs (Customize Toolbar, the long-text editor).
inline bool IsFocusScopedGestureCommand(const char* commandId) {
    return commandId != nullptr &&
           (std::strncmp(commandId, "ui.list.", 8) == 0 || std::strncmp(commandId, "ui.splitter.", 12) == 0);
}

inline bool IsFocusScopedGestureCommand(const std::string& commandId) {
    return IsFocusScopedGestureCommand(commandId.c_str());
}

} // namespace ui
} // namespace smatchet
