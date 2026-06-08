#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "SmatchetResult.h"
#include "TrackerError.h"
#include "TrackerFieldSchema.h"

struct IssueDraft;

class ITrackerIssueMutations {
  public:
    virtual ~ITrackerIssueMutations() = default;

    virtual TrackerError UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields) = 0;

    virtual TrackerError UpdateField(const std::string& issueId, const TrackerField& field,
                                     const std::vector<std::string>& values) = 0;

    virtual Result<nlohmann::json, TrackerError> BuildFieldPayload(const TrackerField& field,
                                                                   const std::vector<std::string>& values) = 0;

    virtual Result<nlohmann::json, TrackerError> BuildCreatePayload(const IssueDraft& /*draft*/,
                                                                    const std::vector<TrackerField>& /*catalog*/) {
        return Result<nlohmann::json, TrackerError>::Err(
            TrackerErrorInvalidRequest("BuildCreatePayload is not supported by this backend."));
    }

    virtual Result<nlohmann::json, TrackerError> BuildUpdatePayload(const IssueDraft& /*draft*/,
                                                                    const std::vector<TrackerField>& /*catalog*/) {
        return Result<nlohmann::json, TrackerError>::Err(
            TrackerErrorInvalidRequest("BuildUpdatePayload is not supported by this backend."));
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

    virtual TrackerError AddIssueToSprint(const std::string& /*issueKey*/, const std::string& /*sprintId*/) {
        return TrackerErrorInvalidRequest("AddIssueToSprint is not supported by this backend.");
    }
};
