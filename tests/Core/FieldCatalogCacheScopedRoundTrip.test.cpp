#include <doctest/doctest.h>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"

#include "../support/TestEnvGuard.h"

#include <string>
#include <vector>

// Regression coverage for the Fix C plumbing:
//   the Jira `components` field renders as plain text in the ticket grid because the
//   project-scoped catalog (which carries Phase-3 component options) was persisted under
//   the UNSCOPED ("") cache key and lost. The fix saves + reloads the catalog under the
//   project-scoped key. These tests pin that the scoped key is the carrier of the options
//   and that the unscoped key does NOT see them — i.e. scope actually matters.

namespace {

TrackerField MakeComponentsField(bool withOptions) {
    TrackerField f;
    f.Id = "components";
    f.Name = "Components";
    f.IsArray = true; // Jira components schema is type=array, items=component
    f.ItemsType = "component";
    f.Family = TrackerFieldFamily::SelectMulti; // classified opts-independently
    if (withOptions) {
        TrackerFieldOption opt;
        opt.Id = "10001";
        opt.Value = "Backend";
        f.AllowedValueOptions.push_back(opt);
        TrackerFieldOption opt2;
        opt2.Id = "10002";
        opt2.Value = "Frontend";
        f.AllowedValueOptions.push_back(opt2);
    }
    return f;
}

const TrackerField* FindField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const auto& f : fields) {
        if (f.Id == id) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("FieldCatalogCache: scoped components catalog round-trips under the project key") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://example.atlassian.net";

    const std::string projectKey = "BLOOP";
    const std::string scopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKey);
    const std::string unscopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, std::string());
    REQUIRE(scopedKey != unscopedKey);

    std::vector<TrackerField> fields;
    fields.push_back(MakeComponentsField(/*withOptions=*/true));

    std::string saveErr;
    REQUIRE(FieldCatalogCache::SaveFieldCatalogSnapshot(scopedKey, "Jira", cfg.Domain, projectKey,
                                                        /*maxProjects=*/16, fields, {}, {}, saveErr));
    CHECK(saveErr.empty());

    SUBCASE("loading under the scoped key preserves component options") {
        std::vector<TrackerField> outFields;
        std::vector<TrackerComponent> outComponents;
        std::vector<TrackerIssueTypeCreateMeta> outMeta;
        std::string loadErr;
        REQUIRE(FieldCatalogCache::TryLoadFieldCatalogSnapshot(scopedKey, outFields, outComponents, outMeta, loadErr));
        const TrackerField* comp = FindField(outFields, "components");
        REQUIRE(comp != nullptr);
        CHECK(comp->IsArray);
        CHECK(comp->ItemsType == "component");
        CHECK(comp->Family == TrackerFieldFamily::SelectMulti);
        REQUIRE(comp->AllowedValueOptions.size() == 2);
        CHECK(comp->AllowedValueOptions[0].Value == "Backend");
        CHECK(comp->AllowedValueOptions[1].Value == "Frontend");
    }

    SUBCASE("loading under the unscoped key does NOT see the scoped options (this was the bug)") {
        std::vector<TrackerField> outFields;
        std::vector<TrackerComponent> outComponents;
        std::vector<TrackerIssueTypeCreateMeta> outMeta;
        std::string loadErr;
        // No snapshot was ever saved under the unscoped key, so the load must miss — proving the
        // startup unscoped load (pre-fix) could never surface the component options.
        CHECK_FALSE(
            FieldCatalogCache::TryLoadFieldCatalogSnapshot(unscopedKey, outFields, outComponents, outMeta, loadErr));
    }
}
