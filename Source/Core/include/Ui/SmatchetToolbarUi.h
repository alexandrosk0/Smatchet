#ifndef SMATCHET_UI_TOOLBAR_UI_H
#define SMATCHET_UI_TOOLBAR_UI_H

// Customizable icon toolbar (Total-Commander-style button bar) rendered directly below
// the main menu bar. Buttons invoke a registered command or run a Lua snippet; a small
// set of ui.* pseudo-actions are handled in-layer (open palette / settings / editor).
// Customization is via a modal editor + right-click context menus; layout persists via
// ConfigManager. See docs/plans/active/customizable-icon-toolbar.md.
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
    // Which list the Customize editor mutates: the shared toolbar or the active backend's append.
    enum class EditScope { Global, Tracker };

    void RenderBar(AppController& app, TrackerConfig& cfg);
    void RenderButtonContextMenu(TrackerConfig& cfg, int src);
    void RenderEditor(AppController& app, TrackerConfig& cfg);
    void DispatchButton(AppController& app, TrackerConfig& cfg, const ToolbarButton& b);
    // Reload the active backend's appended buttons from disk into trackerAppendCache_, but only
    // when the active backend key changed (or the cache was invalidated) — keeps RenderBar off the
    // disk on the per-frame path. A null backend yields an empty cache.
    void RefreshTrackerAppendCache(AppController& app);

    SmatchetIconPickerUi iconPicker_;
    bool requestEditorOpen_ = false;
    int requestEditSelect_ = -1; // button index to preselect when the editor next opens; -1 = default
    int selected_ = -1;          // selected button index in the editor

    // Editor working state (populated when the editor opens).
    EditScope editScope_ = EditScope::Global;   // active scope in the editor
    ToolbarConfig editBuf_;                     // edited copy of the global toolbar; committed to cfg.Toolbar on Save
    std::vector<ToolbarButton> trackerEditBuf_; // edited copy of the active backend's ToolbarAppend (Tracker scope)
    std::string editTrackerType_;               // backend display name captured on open ("Jira"/"Plane"/"GitHub")
    std::string editTrackerKey_;                // normalized views-bucket key for that backend
    bool editHasTracker_ = false;               // a backend was connected when the editor opened
    std::vector<std::string> cmdNames_;         // registry command names for the command picker
    char cmdSearch_[64] = {0};                  // command-picker filter text

    // RenderBar cache of the active backend's ToolbarAppend (refreshed only on backend change).
    std::vector<ToolbarButton> trackerAppendCache_;
    std::string trackerAppendCacheKey_;     // backend key the cache was loaded for ("" = none)
    bool trackerAppendCacheLoaded_ = false; // false forces a reload on the next RenderBar
};

#endif // SMATCHET_UI_TOOLBAR_UI_H
