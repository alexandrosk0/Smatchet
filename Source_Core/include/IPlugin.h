#pragma once

class AppController;

/** Single plugin contract; same vtable shape for future DLL loading. */
class IPlugin {
  public:
    virtual ~IPlugin() = default;

    virtual const char* Id() const = 0;

    /** Before AppController::Initialize (e.g. register Lua log sinks). */
    virtual void OnEarlyInit(AppController&) {}

    /** After Initialize; start threads, etc. */
    virtual void OnStart(AppController&) {}

    /** Same frame as main UI (ImGui). */
    virtual void OnDraw(AppController&) {}

    /** Clean shutdown before ImGui teardown. */
    virtual void OnStop() {}
};
