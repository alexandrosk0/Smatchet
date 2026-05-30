#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "TrackerFieldSchema.h"

struct IssueDraft;

class ITrackerIssueMutations {
  public:
    virtual ~ITrackerIssueMutations() = default;

    virtual bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) = 0;

    virtual bool UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values, std::string& outError) = 0;

    virtual bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                                   nlohmann::json& outPayload, std::string& outError) = 0;

    virtual bool BuildCreatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildCreatePayload is not supported by this backend.";
        return false;
    }

    virtual bool BuildUpdatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildUpdatePayload is not supported by this backend.";
        return false;
    }

    virtual std::string CreateIssue(const nlohmann::json& /*fields*/, std::string& outError) {
        outError = "CreateIssue is not supported by this backend.";
        return {};
    }

    virtual bool AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/,
                                    std::vector<std::pair<std::string, std::string>>& /*outFailures*/,
                                    std::string& outError) {
        outError = "AttachFilesToIssue is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueToSprint(const std::string& /*issueKey*/, const std::string& /*sprintId*/,
                                  std::string& outError) {
        outError = "AddIssueToSprint is not supported by this backend.";
        return false;
    }
};
