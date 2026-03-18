#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "LocalCacheManager.h" // For CachedTicket struct

class ITrackerClient {
public:
    virtual ~ITrackerClient() = default;
    
    // Pure virtual function: Every backend MUST implement this
    virtual std::vector<CachedTicket> FetchIssues() = 0;

    // Update one or more Jira field values on an issue.
    // `fields` is the object payload under Jira's "fields" root.
    virtual bool UpdateIssueFields(const std::string& issueId,
                                   const nlohmann::json& fields,
                                   std::string& outError) = 0;
};