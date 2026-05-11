#include "AppController.h"

void AppController::InitLua() {}

std::vector<std::string> AppController::ListLuaScriptFiles() const {
    return {};
}

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

bool AppController::TryLuaFieldDisplay(const std::string& /*fieldId*/, const CachedTicket& /*ticket*/,
                                       const std::string& /*rawValue*/, float /*availWidth*/,
                                       const TrackerField* /*fieldMeta*/) {
    return false;
}

void AppController::RunAutoScript(const std::string& /*scriptPath*/, const std::vector<std::string>& /*selectedIds*/) {}
void AppController::RunFlatScriptAsync(const std::string& /*scriptPath*/) {}






