#ifndef SMATCHET_INTERFACES_IAPP_META_H
#define SMATCHET_INTERFACES_IAPP_META_H

// Narrow facet for app-lifecycle / version / update queries — AppController fan-in Phase 5
// (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController implements it; the
// app.* command TU depends on this instead of the full AppController.h. No per-frame method.
// Rank-0 leaf (Interfaces/); AppUpdateInfo comes from the rank-0 Types/AppUpdateTypes.h.

#include "Types/AppUpdateTypes.h" // AppUpdateInfo (rank-0)

#include <string>

class IAppMeta {
  public:
    virtual ~IAppMeta() = default;

    virtual std::string GetAppVersion() const = 0;
    virtual std::string GetGitHubReleaseRepo() const = 0;
    virtual void RequestAppQuit() const = 0;
    virtual AppUpdateInfo CheckForAppUpdate(bool includePrerelease = false) const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_META_H
