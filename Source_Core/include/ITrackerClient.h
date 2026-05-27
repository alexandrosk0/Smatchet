#pragma once
#include "ITrackerCollaboration.h"
#include "ITrackerConnectivity.h"
#include "ITrackerFieldCatalog.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

class ITrackerClient : public ITrackerIssueReader,
                       public ITrackerConnectivity,
                       public ITrackerIssueMutations,
                       public ITrackerFieldCatalog,
                       public ITrackerCollaboration {
  public:
    virtual ~ITrackerClient() = default;
};
