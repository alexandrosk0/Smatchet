#pragma once

// TransitionsTypes — the query and result of an issue-transitions lookup (Quality Pillar 6). The
// status combo builds a TransitionsQuery; IssueTransitionsCacheService answers with the live
// transition set, else the workflow remembered from earlier online use, plus its freshness.

#include "OfflineFirstPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <vector>

struct TransitionsQuery {
    std::string IssueId;
    std::string ProjectKey;
    std::string IssueTypeKey;
    std::string FromStatusKey;
};

struct TransitionsLookup {
    bool applicable = false;                 ///< backend supports transitions (Jira)
    std::vector<TrackerFieldOption> options; ///< live if Fresh, else remembered, else empty
    smatchet::offline::DataFreshness freshness = smatchet::offline::DataFreshness::UnavailableNoCache;
    bool fromLearned = false; ///< options came from the remembered workflow
};
