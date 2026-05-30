#ifndef SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H
#define SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H

// Ctrl+Shift+P command palette modal.
// See docs/plans/shipped/command-system-plan.md §"Command Palette".
// This header includes only imgui.h — no GLFW / OpenGL / Win32 — so it
// compiles into both SmatchetStandalone and SmatchetCore_DX12 (Unreal).

#include "Commands/Command.h"

#include <string>
#include <vector>

#include "imgui.h"

class AppController;

namespace smatchet {
namespace cmd {

class CommandPaletteUi {
  public:
    /// Open/close the palette programmatically (also toggled by Ctrl+Shift+P in Draw).
    void Open();
    void Close();
    bool IsOpen() const { return open_; }

    /// Replace the filter text without changing open/closed state.
    /// Used by the inline Command Palette input field in the menu bar
    /// so typing into the bar opens + pre-filters the palette.
    void SetFilterText(const char* query);

    /// Call once per frame from SmatchetUI::Draw. Handles Ctrl+Shift+P detection,
    /// renders the BeginPopupModal, and dispatches the selected command.
    void Draw(AppController& app);

  private:
    bool open_ = false;

    /// Text filter buffer.
    char filterBuf_[256] = {};

    /// Index into the current filtered list.
    int selected_ = 0;

    /// If a required-arg form is active, holds the param index being collected.
    int argFormStep_ = -1;
    /// Per-param input buffers (one per ParamSpec, allocated when arg form opens).
    std::vector<std::vector<char>> argFormBufs_;
    /// The command being arg-prompted.
    Command argFormCmd_;

    /// Filtered command list for the current frame.
    std::vector<Command> filtered_;

    /// Populate `filtered_` from the registry based on `filterBuf_`.
    void rebuildFiltered(AppController& app);

    /// Dispatch the currently-selected command (or show arg form for required params).
    void dispatchSelected(AppController& app);

    /// Render the required-args input form for argFormCmd_.
    void drawArgForm(AppController& app);
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H
