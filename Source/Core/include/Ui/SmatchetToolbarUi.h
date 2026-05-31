#ifndef SMATCHET_UI_TOOLBAR_UI_H
#define SMATCHET_UI_TOOLBAR_UI_H

// Customizable icon toolbar (Total-Commander-style button bar) rendered directly below
// the main menu bar. Buttons invoke a registered command or run a Lua snippet; a small
// set of ui.* pseudo-actions are handled in-layer (open palette / settings / editor).
// Customization is via a modal editor + right-click context menus; layout persists via
// ConfigManager. See docs/plans/active/customizable-icon-toolbar.md.
//
// Header stays ImGui-free (Core dual-target rule) — only std + the pure config model
// and the icon picker (also ImGui-free in its header).

#include <string>
#include <vector>

#include "Config/ToolbarConfig.h"
#include "Ui/SmatchetIconPickerUi.h"

class AppController;
struct TrackerConfig;

class SmatchetToolbarUi {
  public:
    /** Render the toolbar strip (when visible) plus any open editor/picker modals. */
    void Draw(AppController& app, TrackerConfig& cfg);

    /** Open the Customize Toolbar editor on the next Draw() (View menu / pseudo-action). */
    void OpenEditor();

  private:
    void RenderBar(AppController& app, TrackerConfig& cfg);
    void RenderEditor(AppController& app, TrackerConfig& cfg);
    void DispatchButton(AppController& app, TrackerConfig& cfg, const ToolbarButton& b);

    SmatchetIconPickerUi iconPicker_;
    bool requestEditorOpen_ = false;
    int selected_ = -1;  // selected button index in the editor

    // Editor working state (populated when the editor opens).
    ToolbarConfig editBuf_;             // edited copy; committed to cfg.Toolbar on Save
    std::vector<std::string> cmdNames_;  // registry command names for the command picker
    char cmdSearch_[64] = {0};           // command-picker filter text
};

#endif  // SMATCHET_UI_TOOLBAR_UI_H
