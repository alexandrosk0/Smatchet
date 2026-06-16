#ifndef SMATCHET_TYPES_APP_UPDATE_TYPES_H
#define SMATCHET_TYPES_APP_UPDATE_TYPES_H

// Types/AppUpdateTypes.h — AppUpdate value types relocated out of AppController.h
// (fan-in Phase 3, docs/plans/appcontroller-fan-in.md). Leaf header under Types/
// (rank-0 vs the include-cycle gate — non-AppController basename, non-layer-prefix
// dir). These were already global-namespace structs in AppController.h; moving the
// DEFINITIONS here means editing them no longer recompiles every AppController.h
// includer. AppController.h includes this header, so its includers see them unchanged.
// Dependency-free beyond <string>.

#include <string>

struct AppUpdateAsset {
    std::string Name;
    std::string DownloadUrl;
};

struct AppUpdateInfo {
    bool Ok = false;
    bool UpdateAvailable = false;
    std::string CurrentVersion;
    std::string LatestVersion;
    std::string ReleaseTag;
    std::string ReleaseUrl;
    std::string ReleaseNotes;
    std::string Error;
    AppUpdateAsset InstallerAsset;
};

#endif // SMATCHET_TYPES_APP_UPDATE_TYPES_H
