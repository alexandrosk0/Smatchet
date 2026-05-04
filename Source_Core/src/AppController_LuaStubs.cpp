#include "AppController.h"

void AppController::InitLua() {}

void AppController::RunLuaSetupScript(const std::string& /*scriptPath*/) {}

std::vector<std::string> AppController::GetLuaTicketActionNames() const {
    return {};
}

bool AppController::ExecuteLuaTicketAction(const std::string& /*name*/, const std::string& /*issueId*/,
                                           std::string& outError) {
    outError = "Lua automation disabled";
    return false;
}

void AppController::AddAiContext(const std::string& text) {
    aiContext_.push_back(text);
}

const std::vector<std::string>& AppController::GetAiContext() const {
    return aiContext_;
}

void AppController::ClearAiContext() {
    aiContext_.clear();
}

void AppController::PromptAi(const std::string& message) {
    if (aiPromptHandler_) {
        aiPromptHandler_(message);
    }
}

void AppController::SetAiPromptHandler(std::function<void(const std::string&)> handler) {
    aiPromptHandler_ = std::move(handler);
}

std::vector<std::string> AppController::GetLuaGlobalActionNames() const {
    return {};
}

bool AppController::ExecuteLuaGlobalAction(const std::string& /*name*/, std::string& outError) {
    outError = "Lua automation disabled";
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
