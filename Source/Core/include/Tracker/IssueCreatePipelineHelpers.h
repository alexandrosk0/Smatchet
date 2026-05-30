#ifndef SMATCHET_ISSUE_CREATE_PIPELINE_HELPERS_H
#define SMATCHET_ISSUE_CREATE_PIPELINE_HELPERS_H

#include "IssueDraft.h"

#include <nlohmann/json.hpp>

#include <string>

/**
 * Pure-logic helpers extracted from IssueCreatePipeline.cpp so they can be unit-tested
 * without standing up an ITrackerBackend or LocalCacheManager. These are the deterministic
 * cache-merge decisions that run after a successful PUT — no I/O, no network, no DB.
 *
 * The matching `.cpp` definitions live in IssueCreatePipeline.cpp.
 */
namespace IssueCreatePipelineHelpers {

/**
 * Overlay `draft` onto a copy of `existing`, but only for field ids that appear as keys in
 * `putFieldsSucceeded` (the inner Jira `fields` object that the PUT call accepted). Also
 * stamps `issueKey` onto the resulting ticket id, and writes draft IssueType / Parent onto
 * the cache row when set. Used by RunUpdateExisting to keep the cache row in sync with what
 * the server actually accepted, without touching fields the user didn't change.
 */
CachedTicket MergeDraftIntoCachedTicketForUpdate(const CachedTicket& existing, const IssueDraft& draft,
                                                 const std::string& issueKey, const nlohmann::json& putFieldsSucceeded);

} // namespace IssueCreatePipelineHelpers

#endif
