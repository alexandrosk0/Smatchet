#pragma once

#include "IPlugin.h"
#include "LuaConsole.h"
#include <vector>

class LuaConsolePlugin : public IPlugin {
  public:
    const char* Id() const override { return "lua_console"; }
    void OnEarlyInit(AppController& app) override;
    void OnDraw(AppController& app) override;

  private:
    LuaConsole console_;
    std::vector<char> automationEditorBuffer_;
    bool automationEditorLoaded_ = false;

    std::vector<char> hooksEditorBuffer_;
    bool hooksEditorLoaded_ = false;

    void LoadAutomationEditorFile(AppController& app);
    void SaveAutomationEditorFile(AppController& app);
    void LoadHooksEditorFile(AppController& app);
    void SaveHooksEditorFile(AppController& app);
};






