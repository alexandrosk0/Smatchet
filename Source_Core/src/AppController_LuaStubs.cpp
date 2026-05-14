#include "AppController.h"

void AppController::InitLua() {}

// `ListLuaScriptFiles` is defined unconditionally in AppController.cpp; do NOT redeclare it
// here or the linker reports a multiple-definition error in the no-Lua build (pre-existing
// quirk; the Scripts/ enumeration does not actually depend on Lua being compiled in).

void AppController::RunLuaSetupScript(const std::string& /*scriptPath*/) {}

std::vector<std::string> AppController::GetLuaTicketActionNames() const {
    return {};
}

void AppController::ExecuteLuaTicketAction(const std::string& /*name*/, const std::string& /*issueId*/) {}

std::vector<std::string> AppController::GetLuaGlobalActionNames() const {
    return {};
}

void AppController::ExecuteLuaGlobalAction(const std::string& /*name*/) {}

bool AppController::ExecuteLuaConsoleSnippet(const std::string& /*code*/, std::string& outError,
                                             std::string& outResultSummary) {
    outError = "Lua automation disabled";
    outResultSummary.clear();
    return false;
}

bool AppController::TryGetFieldIconMapTarget(const std::string& /*fieldId*/, const TrackerField* /*field*/,
                                             const std::string& /*rawValue*/, std::string& /*outPathOrUrl*/) const {
    return false;
}

bool AppController::TryRenderCachedLuaField(const std::string& /*fieldId*/, const CachedTicket& /*ticket*/,
                                            const std::string& /*rawValue*/, float /*availWidth*/,
                                            const TrackerField* /*fieldMeta*/, bool /*allowEdits*/) {
    return false;
}

void AppController::NotifyLuaTicketDataChanged() {}

void AppController::LuaUiInvalidateWindowBind(const std::string& /*name*/) {}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& /*fieldId*/,
                                                      const std::string& /*luaFnName*/,
                                                      std::string& outError) {
    outError = "Lua automation disabled";
    return false;
}

bool AppController::ScenarioRegisterLuaCachedProvider(const std::string& /*fieldId*/,
                                                      const std::string& /*luaFnName*/,
                                                      const std::vector<std::string>& /*extraScripts*/,
                                                      std::string& outError) {
    outError = "Lua automation disabled";
    return false;
}

void AppController::ScenarioUnregisterLuaCachedProvider(const std::string& /*fieldId*/) {}

void AppController::RunAutoScript(const std::string& /*scriptPath*/, const std::vector<std::string>& /*selectedIds*/) {}
void AppController::RunFlatScriptAsync(const std::string& /*scriptPath*/) {}
