#ifndef SMATCHET_UI_TOOLBAR_UI_H
#define SMATCHET_UI_TOOLBAR_UI_H

// Customizable icon toolbar (Total-Commander-style button bar) rendered directly below
// the main menu bar. Buttons invoke a registered command or run a Lua snippet; a small
// set of ui.* pseudo-actions are handled in-layer (open palette / settings / editor).
// Customization is via a modal editor + right-click context menus; layout persists via
// ConfigManager. See docs/plans/shipped/customizable-icon-toolbar.md.
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

    /** Poll-and-clear the latched "Set shortcut…" request: returns the command id whose
     *  right-click menu fired the item since the last call (empty = none) and writes the
     *  clicked button's ArgsJson into argsJsonOut — only when a request is returned, so the
     *  caller's default survives an empty poll. SmatchetUI polls this each frame to drive the
     *  shared quick-bind modal — keeps the toolbar ImGui-free and avoids threading the modal
     *  through Draw()'s cfg-only signature. */
    std::string TakeQuickBindRequest(std::string& argsJsonOut);

  private:
    // Which list the Customize editor mutates: the shared toolbar or the active backend's append.
    enum class EditScope { Global, Tracker };

    void RenderBar(AppController& app, TrackerConfig& cfg);
    void RenderButtonContextMenu(TrackerConfig& cfg, int src);
    void RenderEditor(AppController& app, TrackerConfig& cfg);

    // Per-frame shared state for the Customize editor's section helpers, captured once after the
    // scope radio buttons resolve which list (`buttons`) is being edited.
    struct EditorCtx {
        std::vector<ToolbarButton>& buttons;
        bool trackerScope;
        bool faLoaded;
    };
    // Section helpers for the editor draw body. Each keeps its own paired ImGui begin and end
    // calls balanced internally; the editor orchestrator only sequences them in order. See the
    // section-helper pattern in docs/guides/imgui-draw-pattern.md.
    void SyncEditorOpenRequest(AppController& app, TrackerConfig& cfg);
    EditorCtx DrawEditorScopeSelector();
    void DrawEditorActionRow(EditorCtx& ctx);
    void DrawEditorButtonList(EditorCtx& ctx);
    void DrawEditorFieldEditor(EditorCtx& ctx);
    void DrawEditorFooter(AppController& app, EditorCtx& ctx, TrackerConfig& cfg);
    void DispatchButton(AppController& app, TrackerConfig& cfg, const ToolbarButton& b);
    // Reload the active backend's appended buttons from disk into trackerAppendCache_, but only
    // when the active backend key changed (or the cache was invalidated) — keeps RenderBar off the
    // disk on the per-frame path. A null backend yields an empty cache.
    void RefreshTrackerAppendCache(AppController& app);

    SmatchetIconPickerUi iconPicker_;
    // One-shot latch set by the per-button "Set shortcut…" context-menu item; drained by
    // TakeQuickBindRequest(). Empty id = no pending request. The paired ArgsJson carries the
    // clicked button's command args so a parametrized button binds its real (commandId, args)
    // identity rather than the default "{}" variant.
    std::string requestQuickBindCommandId_;
    std::string requestQuickBindArgsJson_ = "{}";
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
    // #611 site #7: the disk read for the append cache is loaded OFF the UI thread. While a load
    // is in flight the bar renders the previous/empty append for the frame(s) until it lands; a
    // rapid backend re-switch supersedes an older in-flight load (last-key-wins via the key check).
    bool trackerAppendLoadInFlight_ = false;
    std::string trackerAppendLoadInFlightKey_;
};

#endif // SMATCHET_UI_TOOLBAR_UI_H
