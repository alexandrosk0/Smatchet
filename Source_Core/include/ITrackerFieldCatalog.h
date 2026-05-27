#pragma once
#include <string>
#include <unordered_map>
#include "TrackerFieldSchema.h"

struct TrackerConfig;

class ITrackerFieldCatalog {
  public:
    virtual ~ITrackerFieldCatalog() = default;

    virtual bool FetchFieldCatalog(const TrackerConfig& /*cfg*/, const std::string& /*projectKey*/,
                                   TrackerFieldCatalogResult& /*outCatalog*/, std::string& outError) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                    std::unordered_map<std::string, bool>& /*outFieldIdCanEdit*/,
                                    std::string& outError) {
        outError = "FetchIssueEditMeta is not supported by this backend.";
        return false;
    }
};
