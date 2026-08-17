#pragma once

#include "IPlugin.h"
#include "LuaConsole.h"
#include "TextEditor.h"
#include <future>
#include <string>
#include <vector>

/// Outcome of an off-thread script read. File-scope rather than nested in LuaConsolePlugin so the
/// anonymous-namespace reader in the .cpp can name it; a private nested enum would be unreachable
/// from a free function.
/// Only `NotFound` (no file at that path) may seed the "-- New file" stub and arm Save.
/// `ReadFailed` means a real file is there and we could not read it — a share violation, an AV
/// or indexer holding the handle, an ACL denial, a network-share hiccup, or a truncated read —
/// and stubbing over that is exactly the #2042 data loss (Save then truncates the real script).
/// `OpenFailed` is the worker's "could not open, cause unknown" — PollScriptLoad narrows it to
/// NotFound or ReadFailed on the UI thread, where the enumerated script list is available.
enum class LuaScriptReadStatus { Ok, NotFound, OpenFailed, ReadFailed, TooLarge };

class LuaConsolePlugin : public IPlugin {
  public:
    /// Drains an in-flight script read before the members go away. A std::async future's
    /// destructor already blocks until its task completes, so tearing the plugin down mid-read
    /// (hot-reload, shutdown) would stall the destroying thread with no explanation; draining
    /// here makes that wait explicit, bounded in reporting, and diagnosable via LOG_WARN.
    /// Detaching the task instead is not an option (`no-detach` is an absolute lint rule, and the
    /// task's result feeds members that are being destroyed).
    ~LuaConsolePlugin();

    const char* Id() const override { return "lua_console"; }
    void OnEarlyInit(AppController& app) override;
    void OnDraw(AppController& app) override;

  private:
    LuaConsole console_;
    TextEditor luaEditor_;
    TextEditor::LanguageDefinition luaLangWithApi_;
    bool luaLangReady_ = false;

    std::vector<std::string> scriptList_;
    /// Identity of the selected script (its list name/path). Source of truth for
    /// the selection so a background re-sort of `scriptList_` can never retarget
    /// a save at an unrelated file (DR11). The combo re-derives its display index
    /// from this name each frame via ResolveSelectedScriptIndex.
    std::string selectedScriptName_;
    /// Pending selection while the unsaved-switch modal is open. Tracked by name
    /// (not index) because a refresh can reorder the list before the user picks.
    std::string pendingScriptName_;
    std::string diskSnapshot_;
    /// Identity of the script whose content the editor buffer actually holds; empty whenever the
    /// buffer is the blank placeholder installed for an in-flight read. This is the buffer's
    /// PROVENANCE, distinct from `selectedScriptName_` (the user's current target): the two can
    /// diverge with no read in flight and no refusal latched — a result dropped by the
    /// name-mismatch bail in PollScriptLoad leaves the editor blank, and a later
    /// SyncSelectionToList auto-selects an unrelated script (#2042). SaveCurrentScript writes only
    /// when the two agree, so a buffer that never loaded can never be written over a real file.
    std::string editorContentName_;

    /// Payload of one off-thread script read (Pillar 2, Issue #1925). `name` is echoed back so
    /// the poll can drop a result whose selection moved while the read was in flight.
    struct ScriptLoadResult {
        std::string name;
        std::string content;
        /// Defaults to the SAFE status, not the stub one: a result that never came back from the
        /// worker must never look like "no file here, seed the placeholder". See
        /// LuaScriptReadStatus for why only NotFound may stub.
        LuaScriptReadStatus status = LuaScriptReadStatus::ReadFailed;
    };
    std::future<ScriptLoadResult> scriptLoadFuture_;
    /// A script read is running on the std::async worker. Also gates SaveCurrentScript: the editor
    /// is blank during the in-flight window, so a save would truncate the file on disk.
    bool scriptLoadInFlight_ = false;
    /// Selection made WHILE a read was in flight. Re-kicked from PollScriptLoad rather than
    /// re-assigning `scriptLoadFuture_` inline — move-assigning over a live std::async future
    /// blocks the render thread until the old task's shared state is released.
    std::string queuedLoadName_;
    /// Script name whose "Reload from disk" press is still waiting on its off-thread read; empty
    /// when no reload is outstanding. Held by NAME, not a bool: the read lands frames later, so
    /// the outcome banner must be reported by PollScriptLoad, and a selection change made in the
    /// meantime must not mislabel an unrelated load as the user's reload.
    std::string reloadNoticeName_;
    /// Non-empty when the editor is blank for a script that was NOT read successfully — refused
    /// (over the size cap), a worker throw, or a launch that never started. Gates SaveCurrentScript
    /// for the same reason `scriptLoadInFlight_` does: writing the blank buffer back would truncate
    /// a file that has real content. A string rather than a bool because the three causes need
    /// different guidance ("too large, use an external editor" vs "reopen it"), and a wrong message
    /// here points the user at the wrong remedy. Cleared on every fresh kick.
    std::string scriptLoadRefusedReason_;

    /// Clamped height for `BeginChild` (script pane) this frame.
    float luaAutomationScriptPaneHeightPx_ = 0.f;
    /// User/persisted intent; not overwritten when early-frame `maxScriptH` is tiny (docked layout settling).
    float luaAutomationScriptPaneUserPx_ = 0.f;
    float scriptPaneHeightBeforeToolsTab_ = 0.f;
    bool onToolsTabLastFrame_ = false;
    /// Avoid ListLuaScriptFiles/directory_iterator every frame (Windows can fault under heavy enumeration).
    bool onScriptsTabLastFrame_ = false;
    /// ImGui::GetTime() of last directory scan; used to cap refresh rate if tab visibility flickers.
    double lastScriptListRefreshImGuiTime_ = -1.0e30;
    bool runInBackground_ = false;

    /// Lua window tab selection (Scripts=0, Tools=1). Previously `static int s_tabSel` in OnDraw —
    /// a file-static that would corrupt state across hot-reload or multiple plugin instances.
    int tabSel_ = 0;
    /// Set when the scripting editor tab needs focus on the next draw.
    bool pendingSelectScriptsTab_ = false;

    void EnsureLuaLanguageDef();
    void RefreshScriptList(const AppController& app, bool forceRescan = false);
    /// Reconcile `selectedScriptName_` with the current `scriptList_`. Keeps the
    /// name when its file is still present; defaults to the first entry only when
    /// nothing has been selected yet; clears the selection when a tracked file
    /// vanished (so a save cannot fall through to an unrelated file -- DR11).
    void SyncSelectionToList();
    /// Blank the editor and kick the selected script's read onto a worker (Pillar 2: no file I/O
    /// on the render thread). Returns false only for the synchronous validation failures (no
    /// selection / unresolvable path); a read error surfaces later via PollScriptLoad.
    bool StartLoadSelectedScriptIntoEditor(const AppController& app, std::string& outErr);
    /// Launch the worker for `name`. Assumes no read is already in flight.
    void KickScriptLoad(const AppController& app, const std::string& name);
    /// Per-frame non-blocking poll of `scriptLoadFuture_`; applies the text once ready. Called at
    /// the top of OnDraw, before the window-hidden early-out, so a read kicked from OnEarlyInit
    /// (or while the window was closed) still lands.
    void PollScriptLoad(const AppController& app);
    bool SaveCurrentScript(const AppController& app, std::string& outErr);
    void ApplyErrorMarkersFromMessage(const std::string& errMsg);
    void ClearErrorMarkers();
    void DrawAutocompletePopup();
    static int TryParseLuaErrorLine(const std::string& err);
    static bool IsHooksFile(const std::string& rel);

    /// Per-frame layout values shared across the OnDraw section helpers. Holds
    /// references to the owning AppController plus the split-pane height constants
    /// computed once at the top of the frame; never re-fetched per helper.
    struct DrawCtx {
        AppController& app;
        bool onToolsTab;
        float splitStackTotalH;
        float kSplitGrabH;
        float kMinLogH;
        float kMinScriptH;
        float kToolsLogH;
    };

    bool BeginLuaWindow(bool wantFocus);
    void UpdateSplitLayout(DrawCtx& ctx);
    void DrawScriptPane(DrawCtx& ctx);
    void DrawScriptsTab(DrawCtx& ctx, const std::string& curName);
    void DrawRunButtonRow(DrawCtx& ctx, const std::string& curName);
    void DrawRegisteredActionsRow(DrawCtx& ctx);
    void DrawEditorAndAutocomplete();
    void DrawToolsTab(DrawCtx& ctx);
    void DrawSplitterAndConsole(DrawCtx& ctx);
};
