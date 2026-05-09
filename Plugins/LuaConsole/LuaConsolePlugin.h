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
    int selectedScriptIndex_ = 0;
    int pendingScriptIndex_ = -1;
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

    void EnsureLuaLanguageDef();
    void RefreshScriptList(const AppController& app, bool forceRescan = false);
    bool LoadSelectedScriptIntoEditor(const AppController& app, std::string& outErr);
    bool SaveCurrentScript(const AppController& app, std::string& outErr);
    void ApplyErrorMarkersFromMessage(const std::string& errMsg);
    void ClearErrorMarkers();
    void DrawAutocompletePopup();
    static int TryParseLuaErrorLine(const std::string& err);
    static bool IsHooksFile(const std::string& rel);
};
