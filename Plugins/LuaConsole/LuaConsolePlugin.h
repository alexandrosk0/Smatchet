#pragma once

#include "IPlugin.h"
#include "LuaConsole.h"

class LuaConsolePlugin : public IPlugin {
  public:
    const char* Id() const override { return "lua_console"; }
    void OnEarlyInit(AppController& app) override;
    void OnDraw(AppController& app) override;

  private:
    LuaConsole console_;
};
