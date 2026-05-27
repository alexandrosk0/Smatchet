#pragma once

class ITrackerIssueReader;
class ITrackerConnectivity;
class ITrackerFieldCatalog;
class ITrackerIssueMutations;
class ITrackerCollaboration;

class ITrackerBackend {
  public:
    virtual ~ITrackerBackend() = default;
    virtual ITrackerIssueReader& Reader() = 0;
    virtual ITrackerConnectivity& Connectivity() = 0;
    virtual ITrackerFieldCatalog* FieldCatalog() = 0;   // nullptr if unsupported
    virtual ITrackerIssueMutations* Mutations() = 0;    // nullptr if unsupported
    virtual ITrackerCollaboration* Collaboration() = 0; // nullptr if unsupported
};
