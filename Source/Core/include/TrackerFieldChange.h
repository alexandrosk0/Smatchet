#pragma once

// TrackerFieldChange — one dated, attributed field change an issue's tracker history reports. Backend-agnostic:
// a concrete client maps its own history shape to this (ITrackerIssueReader::FetchIssueFieldChanges).

#include <string>

struct TrackerFieldChange {
    std::string issueKey;
    std::string fieldId;    ///< Backend field id when the history names one; may be empty.
    std::string fieldLabel; ///< The name the history gives the field, e.g. "status" or "Sprint".
    std::string fromValue;
    std::string toValue;
    std::string author;    ///< Display name, empty when the history does not say.
    std::string changedAt; ///< ISO-8601 timestamp as the tracker wrote it.
};
