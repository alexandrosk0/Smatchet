#include "ProjectListLookup.h"

#include "ILookupCache.h"
#include "ITrackerConnectivity.h"
#include "Logger.h"
#include "LookupPayloadsPure.h"
#include "Sync/LookupFreshnessPure.h"

#include <chrono>
#include <exception>
#include <utility>

namespace smatchet {
namespace projects {

namespace {

bool LoadSavedProjects(const std::shared_ptr<ILookupCache>& store, const std::string& backendKey,
                       std::vector<RemoteProject>& out, std::int64_t* savedAtEpochSec = nullptr) {
    if (!store || backendKey.empty()) {
        return false;
    }
    try {
        LookupCacheRow row;
        const bool ok = store->TryGetLookup(backendKey, lookup::kProjectsKind, lookup::kProjectsKey, row) &&
                        lookup::ParseProjects(row.PayloadJson, out);
        if (ok && savedAtEpochSec != nullptr) {
            *savedAtEpochSec = row.UpdatedAtEpochSec;
        }
        return ok;
    } catch (const std::exception& ex) {
        LOG_WARN("ProjectListLookup: reading the saved project list failed: %s", ex.what());
    } catch (...) { // catch-all-ok: logged; the caller shows no saved list
        LOG_WARN("ProjectListLookup: reading the saved project list failed (unknown exception)");
    }
    out.clear();
    return false;
}

void SaveProjects(const std::shared_ptr<ILookupCache>& store, const std::string& backendKey,
                  const std::vector<RemoteProject>& projects) {
    if (!store || backendKey.empty()) {
        return;
    }
    const std::string payload = lookup::SerializeProjects(projects);
    if (payload.empty()) {
        return; // over the size cap: keep the older saved list rather than store an unreadable one
    }
    try {
        store->UpsertLookup(backendKey, lookup::kProjectsKind, lookup::kProjectsKey, payload);
    } catch (const std::exception& ex) {
        LOG_WARN("ProjectListLookup: saving the project list failed: %s", ex.what());
    } catch (...) { // catch-all-ok: logged; the live list is still shown
        LOG_WARN("ProjectListLookup: saving the project list failed (unknown exception)");
    }
}

} // namespace

ProjectListOutcome LoadProjectList(ITrackerConnectivity& connectivity, const std::shared_ptr<ILookupCache>& store,
                                   const std::string& backendKey, bool offline, std::int64_t ttlSec,
                                   std::int64_t nowEpochSec) {
    ProjectListOutcome outcome;
    outcome.Failed = true;
    if (!offline && ttlSec > 0) {
        // A saved list younger than its lifetime is current enough: no request.
        if (nowEpochSec == 0) {
            nowEpochSec = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }
        std::int64_t savedAt = 0;
        std::vector<RemoteProject> saved;
        // An empty saved list never stands in: "No projects found" must come from the tracker, not from a copy.
        if (LoadSavedProjects(store, backendKey, saved, &savedAt) && !saved.empty() &&
            smatchet::sync::IsLookupRowFresh(savedAt, nowEpochSec, ttlSec)) {
            outcome.Projects = std::move(saved);
            outcome.Failed = false;
            outcome.ServedFresh = true;
            return outcome;
        }
    }
    if (offline) {
        outcome.Error = TrackerErrorTransport("The tracker is offline; the project list was not refreshed.");
    } else {
        try {
            ProjectListResult listed = connectivity.ListProjectsTyped();
            if (listed.has_value()) {
                outcome.Projects = std::move(listed.value());
                outcome.Failed = false;
                SaveProjects(store, backendKey, outcome.Projects);
                return outcome;
            }
            outcome.Error = listed.error();
        } catch (const std::exception& ex) {
            outcome.Error = TrackerErrorUnknown(std::string("Listing projects failed: ") + ex.what());
        } catch (...) { // catch-all-ok: reported as the listing's failure
            outcome.Error = TrackerErrorUnknown("Listing projects failed (unknown exception).");
        }
        LOG_WARN("ProjectListLookup: listing projects failed kind=%s: %s", ToString(outcome.Error.Kind),
                 outcome.Error.Detail.c_str());
    }
    outcome.FromSaved = LoadSavedProjects(store, backendKey, outcome.Projects);
    return outcome;
}

} // namespace projects
} // namespace smatchet
