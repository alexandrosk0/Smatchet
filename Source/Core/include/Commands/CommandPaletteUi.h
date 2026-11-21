#ifndef SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H
#define SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H

// Ctrl+Shift+P command palette modal.
// See docs/plans/shipped/command-system-plan.md §"Command Palette".
// This header includes only imgui.h — no GLFW / OpenGL / Win32 — so it
// compiles into both SmatchetStandalone and SmatchetCore_DX12 (Unreal).

#include "Commands/Command.h"
#include "TextMatchPure.h"

#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

class AppController;
struct Keybinding; // global-namespace, from Config/KeybindingsConfig.h (borrowed by pointer)

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

    /// Re-arm keyboard focus on the filter input for the next Draw. Open() already
    /// does this, but the auto-focus is otherwise tied to the palette WINDOW
    /// appearing: once another window (the first-run Preferences surface, a modal)
    /// takes focus afterwards, the input goes inactive and never regains focus on
    /// its own. Idempotent, and a no-op while the palette is closed.
    void FocusFilterInput() { focusFilterPending_ = true; }

    /// Restrict the palette to commands whose id starts with `prefix`, shown to the
    /// user as `label` (e.g. "Views") — the "Open View..." funnel pre-scopes with this
    /// instead of stuffing the literal `view.toggle.` id into the search box (P2-M7).
    /// Cleared by Open()/Close(); the user types WITHIN the scope.
    void SetCategoryPrefix(const char* prefix, const char* label);

    /// Read the current filter buffer (NUL-terminated). Test-observability accessor
    /// for the inline typing -> filter path (bucket-E coverage); zero-cost inline,
    /// not on any per-frame/render path. See tests/ui/command_palette_inline_typing.test.cpp.
    const char* FilterText() const { return filterBuf_; }

    /// Index of the highlighted row in the filtered list. Test-observability accessor.
    int SelectedIndex() const { return selected_; }

    /// The listed rows, best match first: their count, the command id of row `i` ("" past the end) and how
    /// many marked runs the row's title draws. Test-observability accessors.
    int FilteredCount() const;
    const char* FilteredName(int i) const;
    std::size_t FilteredTitleRangeCount(int i) const;

    /// The line shown for the last failed dispatch ("Could not run: ..."), kept until the
    /// next input; empty when the last run succeeded. Test-observability accessor.
    const std::string& DispatchError() const { return dispatchError_; }

    /// True while the required-argument form is showing. Test-observability accessor.
    bool IsArgFormOpen() const { return argFormStep_ >= 0; }

    /// Call once per frame from SmatchetUI::Draw. Handles Ctrl+Shift+P detection,
    /// renders the BeginPopupModal, and dispatches the selected command.
    void Draw(AppController& app);

    /// Borrow the active keybinding table for the frame so rows can surface their bound
    /// combo. SmatchetUI re-points this each Draw from d.cfg.Keybindings.Bindings; the
    /// pointer is not owned and must outlive the next Draw. Null = no combo surfacing.
    void SetKeybindings(const std::vector<Keybinding>* binds) { keybindings_ = binds; }

    /// Poll-and-clear the latched "Set shortcut…" request: the command id whose row
    /// right-click menu fired the item since the last call (empty = none). SmatchetUI
    /// polls this each frame to drive the shared quick-bind modal.
    std::string TakeQuickBindRequest();

  private:
    bool open_ = false;
    /// Pending "focus the filter input on the next Draw" latch (see FocusFilterInput()).
    bool focusFilterPending_ = false;

    /// Borrowed (non-owning) keybinding table for combo surfacing; re-pointed each frame.
    const std::vector<Keybinding>* keybindings_ = nullptr;
    /// One-shot latch set by a row's "Set shortcut…" item; drained by TakeQuickBindRequest().
    std::string requestQuickBindCommandId_;

    /// Text filter buffer.
    char filterBuf_[256] = {};

    /// Optional command-id prefix scope + its human label (see SetCategoryPrefix).
    std::string categoryPrefix_;
    std::string categoryLabel_;

    /// Index into the current filtered list.
    int selected_ = 0;
    /// One-shot "bring the selected row into view" latch, set only when the selection
    /// moves by keyboard or the list is re-ranked. Scrolling to the selection every
    /// frame would fight the mouse wheel and the scrollbar.
    bool scrollToSelected_ = false;

    /// The localized line for the last failed dispatch, formatted once when it fails
    /// (empty = no failure to show). A failed run keeps the palette and any argument
    /// form open so the input can be corrected and re-run.
    std::string dispatchError_;

    /// If a required-arg form is active, holds the param index being collected.
    int argFormStep_ = -1;
    /// One-shot "focus the first argument field" latch: set when the form opens and
    /// after a failed run, so the caret is where the correction goes.
    bool argFocusPending_ = false;
    /// Per-param input buffers (one per ParamSpec, allocated when arg form opens).
    std::vector<std::vector<char>> argFormBufs_;
    /// The command being arg-prompted.
    Command argFormCmd_;

    /// #1703 deferred-dispatch latch. dispatchSelected() / drawArgForm()'s "Run"
    /// run INSIDE the palette's ImGui::Begin("##cmdpalette")/End() scope;
    /// dispatching a command there corrupts the ImGui window stack when the
    /// command touches it (a mid-frame Begin/End imbalance — Pillar-3 abort,
    /// seed 424242). Instead they latch here and Draw() drains the latch at
    /// top-of-frame, OUTSIDE the palette's Begin/End — mirroring the ViewState
    /// top-of-Draw deferral latch mandated by Source/Core/src/Ui/AGENTS.md.
    /// Args are a shared_ptr<json> (null = empty-object) to keep json_fwd-only
    /// header hygiene (see Command.h).
    bool hasPendingDispatch_ = false;
    std::string pendingDispatchName_;
    std::shared_ptr<nlohmann::json> pendingDispatchArgs_;
    bool pendingDispatchDestructive_ = false;

    /// Filtered command list for the current frame.
    std::vector<Command> filtered_;

    /// What matched the query in each `filtered_` row (parallel to it; empty ranges with no query).
    struct RowRanges {
        std::vector<textmatch::ByteRange> Title;
        std::vector<textmatch::ByteRange> Summary;
        bool Exact = false; // every word occurred as written
    };
    std::vector<RowRanges> filteredRanges_;

    /// Recent commands listed first with no query, and boosted under one.
    static constexpr std::size_t kRecentCount = 8;

    /// Populate `filtered_` from the registry based on `filterBuf_`.
    void rebuildFiltered(AppController& app);

    /// `filtered_` / `filteredRanges_` for a non-empty query, ranked (consumes `all`).
    void rankByQuery(const textmatch::Query& query, std::vector<Command>& all, const std::vector<std::string>& recents);

    /// One row's title and summary with the matched letters marked, at the row's text origin.
    void drawRowText(int row, const std::string& primary, const std::string& summary, const ImVec2& pos) const;

    /// Dispatch the currently-selected command (or show arg form for required params).
    void dispatchSelected();

    /// Drain the #1703 deferred-dispatch latch: run the latched command, then Close()
    /// on success or keep the palette open with dispatchError_ set on failure.
    /// Called at the top of Draw(), before the palette's Begin(), so any command
    /// that touches the ImGui window stack runs at a clean nesting level.
    void drainPendingDispatch(AppController& app);

    /// Render the required-args input form for argFormCmd_.
    void drawArgForm();

    /// Render dispatchError_ as wrapped error text; no-op when there is none.
    void drawDispatchError() const;

    /// Up/Down selection + Enter/Escape dispatch for the command list (split out of Draw for
    /// function-size compliance; runs inside the active palette Begin scope).
    void handleListNavigation(const ImGuiIO& io);

    /// Scrollable command-list child window (owns its own BeginChild/EndChild pair).
    void drawCommandList(const ImGuiIO& io);
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_COMMAND_PALETTE_UI_H
