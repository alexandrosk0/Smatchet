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

    virtual Result<std::string, TrackerError> CreateIssue(const nlohmann::json& /*fields*/) {
        return Result<std::string, TrackerError>::Err(
            TrackerErrorInvalidRequest("CreateIssue is not supported by this backend."));
    }

    // Ok payload = the per-file failures list (possibly empty on full success — partial success is
    // NOT an error, plan landmine L3); Err = a hard failure that aborted the whole attach.
    virtual Result<std::vector<std::pair<std::string, std::string>>, TrackerError>
    AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/) {
        return Result<std::vector<std::pair<std::string, std::string>>, TrackerError>::Err(
            TrackerErrorInvalidRequest("AttachFilesToIssue is not supported by this backend."));
    }

    virtual TrackerError AddIssueToSprint(const std::string& /*issueKey*/, const std::string& /*sprintId*/) {
        return TrackerErrorInvalidRequest("AddIssueToSprint is not supported by this backend.");
    }
};
