#pragma once

#include "IPlugin.h"
#include "LuaConsole.h"
#include "TextEditor.h"
#include <string>
#include <vector>

class LuaConsolePlugin : public IPlugin {
  public:
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
    bool LoadSelectedScriptIntoEditor(const AppController& app, std::string& outErr);
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
