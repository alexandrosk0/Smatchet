#pragma once

#include "SmatchetResult.h"

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
// Ok(events) on a successful read (an empty vector when there is no audit file yet or no
// events within maxEvents); Err(reason) when the read itself fails (filesystem/parse-lock
// exception). Distinguishing "no events" from "read error" is the point of the Result shape.
Result<std::vector<nlohmann::json>> ReadRecentEvents(std::size_t maxEvents);

} // namespace BackendAuditTrail
