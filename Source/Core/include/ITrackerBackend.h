#pragma once

class ITrackerIssueReader;
class ITrackerConnectivity;
class ITrackerFieldCatalog;
class ITrackerIssueMutations;
class ITrackerCollaboration;
class ITrackerActivity;

class ITrackerBackend {
  public:
    virtual ~ITrackerBackend() = default;
    virtual ITrackerIssueReader& Reader() = 0;
    virtual ITrackerConnectivity& Connectivity() = 0;
    virtual ITrackerFieldCatalog* FieldCatalog() = 0;   // nullptr if unsupported
    virtual ITrackerIssueMutations* Mutations() = 0;    // nullptr if unsupported
    virtual ITrackerCollaboration* Collaboration() = 0; // nullptr if unsupported
    virtual ITrackerActivity* Activity() = 0;           // nullptr if unsupported

    // Const accessors for read-only call sites (delegate to the non-const virtuals above).
    const ITrackerIssueReader& Reader() const { return const_cast<ITrackerBackend*>(this)->Reader(); }
    const ITrackerConnectivity& Connectivity() const { return const_cast<ITrackerBackend*>(this)->Connectivity(); }
};
