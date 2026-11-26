#ifndef SMATCHET_UI_TOOLBAR_UI_H
#define SMATCHET_UI_TOOLBAR_UI_H

// Customizable icon toolbar (Total-Commander-style button bar) rendered directly below
// the main menu bar. Buttons invoke a registered command or run a Lua snippet; a small
// set of ui.* pseudo-actions are handled in-layer (open palette / settings / editor).
// Customization is via a modal editor + right-click context menus; layout persists via
// ConfigManager. See docs/plans/shipped/customizable-icon-toolbar.md.
// Header stays ImGui-free (Core dual-target rule) — only std + the pure config model
// and the icon picker (also ImGui-free in its header).

#include <cstddef>
#include <string>
#include <vector>

#include "Config/ToolbarConfig.h"
#include "TextMatchPure.h"
#include "Ui/DialogScopeState.h"
#include "Ui/SmatchetIconPickerUi.h"

class AppController;
struct TrackerConfig;

namespace smatchet {
namespace ui {

/** What asked for the command: it picks the wording of the notice and the log prefix. */
enum class ArgsTextOrigin { ToolbarButton, Shortcut };

/** Runs the registry command `commandId` with args given as JSON text; empty text means {}.
    Text that is not a JSON object is refused: a warning toast and a log line, and the command
    is not run. Running it with {} in their place would do something other than what the button
    or shortcut was set up to do. While that toast is still on screen a repeat of the same press
    is refused without a second one. UI thread only. */
void DispatchCommandWithArgsText(AppController& app, const std::string& commandId, const std::string& argsJson,
                                 ArgsTextOrigin origin);

} // namespace ui
} // namespace smatchet

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

#if defined(SMATCHET_BUILD_UI_TESTS)
    /** Bucket-E seams: draw only the Customize Toolbar modal (no bar), and read what it edits. */
    void DrawEditorOnlyForTest(AppController& app, TrackerConfig& cfg) { RenderEditor(app, cfg); }
    const std::vector<ToolbarButton>& EditorButtonsForTest() const { return editBuf_.Buttons; }
    int EditorSelectedForTest() const { return selected_; }
#endif

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
        bool submit = false; // Ctrl+Enter this frame (smatchet::ui::DialogReadKeys): Save, as the button does
    };
    // Section helpers for the editor draw body. Each keeps its own paired ImGui begin and end
    // calls balanced internally; the editor orchestrator only sequences them in order. See the
    // section-helper pattern in docs/guides/imgui-draw-pattern.md.
    void SyncEditorOpenRequest(AppController& app, TrackerConfig& cfg);
    EditorCtx DrawEditorScopeSelector();
    void DrawEditorActionRow(EditorCtx& ctx);
    // The editor's move / remove of the selected button, shared by its Up / Down / Delete buttons and the
    // list keys (ui.list.move_up / move_down / remove). Each is a no-op without a selection or room to move.
    bool MoveSelectedButton(std::vector<ToolbarButton>& buttons, int delta);
    bool RemoveSelectedButton(std::vector<ToolbarButton>& buttons);
    void DrawEditorButtonList(EditorCtx& ctx);
    void DrawEditorFieldEditor(EditorCtx& ctx);
    void DrawEditorCommandPicker(ToolbarButton& b);
    void DrawEditorArgsField(ToolbarButton& b);
    // Status (a toolbar_editor::ArgsJsonStatus) of button `i`'s args text. Cached per index: the text is
    // parsed again only when it differs from the text the cached status was computed for.
    int ArgsStatusAt(std::size_t i, const std::string& text);
    // Index of the first Command button whose args are not usable, or -1.
    int FirstInvalidArgsIndex(const std::vector<ToolbarButton>& buttons);
    void DrawEditorFooter(AppController& app, EditorCtx& ctx, TrackerConfig& cfg);
    void DispatchButton(AppController& app, const ToolbarButton& b);
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
    int pendingListFocus_ = -1;  // row the list's keyboard cursor moves to next frame; -1 = none

    // Keys (smatchet::ui dialog contract, audit #120 and the key half of #121): the selected row of the button
    // list takes the keyboard on open (Add command, when the list is empty), Ctrl+Enter saves and Esc cancels.
    // With the caret in a field and an edit made, the first Esc only leaves the field. A new Command button
    // puts the caret in the command filter, a new Lua button in its tooltip. Per instance: a test host draws
    // its own editor next to the app's, and a shared state would be reset each frame by the closed one.
    smatchet::ui::DialogScopeState editorDialog_;
    bool caretToNewButton_ = false;
    ToolbarConfig editorOpenedGlobal_;               // the lists the editor opened with: Esc closes in one
    std::vector<ToolbarButton> editorOpenedTracker_; // press while nothing differs from them

    // Editor working state (populated when the editor opens).
    EditScope editScope_ = EditScope::Global;   // active scope in the editor
    ToolbarConfig editBuf_;                     // edited copy of the global toolbar; committed to cfg.Toolbar on Save
    std::vector<ToolbarButton> trackerEditBuf_; // edited copy of the active backend's ToolbarAppend (Tracker scope)
    std::string editTrackerType_;               // backend display name captured on open ("Jira"/"Plane"/"GitHub")
    std::string editTrackerKey_;                // normalized views-bucket key for that backend
    bool editHasTracker_ = false;               // a backend was connected when the editor opened
    std::vector<std::string> cmdNames_;         // registry command names for the command picker
    std::vector<std::string> cmdHaystacks_;     // "name title" per cmdNames_ entry: what the filter matches
    std::vector<int> cmdFiltered_;              // ranked indices into cmdNames_ for the current filter text
    std::vector<std::vector<smatchet::textmatch::ByteRange>> cmdRanges_; // per cmdFiltered_ row: name bytes to mark
    smatchet::textmatch::Scratch cmdScratch_;                            // the matcher's work buffers for cmdRanges_
    std::string cmdFilterKey_;                                           // filter text cmdFiltered_ was built for
    bool cmdFilterValid_ = false; // false forces a rebuild (set when the editor opens)
    char cmdSearch_[64] = {0};    // command-picker filter text
    struct ArgsCheckEntry {
        std::string Text;      // args text the status was computed for
        int Status = 0;        // toolbar_editor::ArgsJsonStatus
        bool Computed = false; // false until the first lookup
    };
    std::vector<ArgsCheckEntry> argsCheck_; // per button index of the edited scope; cleared when the editor opens

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
