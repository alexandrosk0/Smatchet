#include "AppController.h"

void AppController::InitLua() {}

void AppController::RunLuaSetupScript(const std::string& /*scriptPath*/) {}

bool AppController::TryLuaFieldDisplay(const std::string& /*fieldId*/, const CachedTicket& /*ticket*/,
                                       const std::string& /*rawValue*/, float /*availWidth*/,
                                       const TrackerField* /*fieldMeta*/) {
    return false;
}

void AppController::RunAutoScript(const std::string& /*scriptPath*/) {}
