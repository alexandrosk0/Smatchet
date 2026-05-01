#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace BackendAuditTrail {

struct AuditEvent {
    std::string Action;
    std::string Source;
    std::string IssueKey;
    std::string OperationId;
    bool Success = false;
    std::string Error;
    nlohmann::json Data = nlohmann::json::object();
    std::string Phase = "result";
};

std::string GetAuditFilePath();
std::string MakeOperationId(const std::string& prefix);
nlohmann::json RedactJson(const nlohmann::json& value);
std::string RedactText(const std::string& key, const std::string& value);
nlohmann::json MakeFieldDiffUnknownBefore(const nlohmann::json& fields);
void AppendBegin(const std::string& action, const std::string& source, const std::string& issueKey,
                 const std::string& operationId, const nlohmann::json& data = nlohmann::json::object());
void AppendResult(const std::string& action, const std::string& source, const std::string& issueKey,
                  const std::string& operationId, bool success, const std::string& error,
                  const nlohmann::json& data = nlohmann::json::object());
void AppendEvent(const AuditEvent& event);
std::vector<nlohmann::json> ReadRecentEvents(std::size_t maxEvents, std::string* outError = nullptr);

} // namespace BackendAuditTrail
