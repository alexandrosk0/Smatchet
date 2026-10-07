// libFuzzer driver for the Jira field-catalog builders — TrackerFieldCatalogPure
// (TrackerFieldCatalogPure.cpp). These consume ALREADY-PARSED nlohmann::json from
// the Jira catalog endpoints and fold it into the field catalog: /field elements
// (ParseFieldDefinition — schema.type/items/custom), /issue/createmeta (the
// deepest nesting in the tracker layer: projects[].issuetypes[].fields{}.
// allowedValues[], via ApplyCreateMetaToCatalog), /issuetype and /project/{key}
// issue-type lists, flat /priority-style catalogs, component beans (flat or
// wrapped in componentBean{}), and the agile board / sprint pages.
//
// The raw parse is already depth/node-bounded by json_safe::ParseBounded, so this
// driver fuzzes the *consuming* layer: it feeds fuzz bytes through ParseBounded
// (what the production fetcher sees) and drives every pure entry point on the
// resulting DOM. The builders throw nlohmann::json::exception on a mistyped member
// by contract (TrackerFieldCatalog.cpp wraps each call), so each entry point is
// guarded on its own and the rest still run; the bug classes we hunt (OOB / UB /
// bad alloc / a stale catalog index) surface via ASan+UBSan regardless.
#include "TrackerFieldCatalogPure.h"

#include "Json/BoundedJsonParse.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace TrackerFieldCatalogPure;

// Runs one entry point; a std::exception is the builders' contractual reaction to
// a mistyped member (the production caller catches it), not a crash.
template <typename Fn> void Guarded(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return; // contractual throw — keep fuzzing the remaining entry points
    }
}

// A catalog with the fields createmeta merges allowedValues into, indexed by id
// exactly as TrackerFieldCatalog.cpp builds fieldIndexById.
void SeedCatalog(std::vector<TrackerField>& fields, std::unordered_map<std::string, std::size_t>& indexById) {
    const char* const kIds[] = {"summary", "priority", "components", "labels", "issuetype", "customfield_10020"};
    for (const char* id : kIds) {
        TrackerField f;
        f.Id = id;
        f.Name = id;
        indexById[f.Id] = fields.size();
        fields.push_back(f);
    }
}

void ParseFieldDefinitions(const nlohmann::json& j) {
    std::vector<std::string> sprintFieldIds;
    Guarded([&] {
        TrackerField one;
        (void)ParseFieldDefinition(j, one, sprintFieldIds);
    });
    if (j.is_array()) { // the /field response is an array of definitions
        for (const nlohmann::json& node : j) {
            Guarded([&] {
                TrackerField each;
                (void)ParseFieldDefinition(node, each, sprintFieldIds);
            });
        }
    }
}

void ApplyCreateMeta(const nlohmann::json& j) {
    std::vector<TrackerField> fields;
    std::unordered_map<std::string, std::size_t> indexById;
    SeedCatalog(fields, indexById);
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::set<std::string> uniqueIssueTypes;
    Guarded(
        [&] { ApplyCreateMetaToCatalog(j, "SMT", indexById, fields, components, issueTypeMeta, uniqueIssueTypes); });
    Guarded([&] { SortComponentCatalog(fields, components); });
}

void BuildOptionLists(const nlohmann::json& j) {
    Guarded([&] {
        std::vector<std::string> allowedValues;
        std::vector<TrackerFieldOption> options;
        BuildDedupedIssueTypeOptions(j, allowedValues, options);
    });
    Guarded([&] {
        TrackerField priority;
        priority.Id = "priority";
        BuildSimpleCatalogOptions(j, priority);
    });
    Guarded([&] {
        std::vector<TrackerFieldOption> options;
        (void)BuildIssueTypeOptionsFromProjectJson(j, options);
    });
}

void MergeComponents(const nlohmann::json& j) {
    std::vector<TrackerField> fields;
    std::unordered_map<std::string, std::size_t> indexById;
    SeedCatalog(fields, indexById);
    std::vector<TrackerComponent> components;
    Guarded([&] { (void)ResolveComponentJsonBean(j); });
    const auto mergeOne = [&](const nlohmann::json& node) {
        Guarded([&] {
            TrackerComponent component;
            TrackerFieldOption option;
            if (ExtractComponentOption(node, component, option)) {
                MergeComponentIntoCatalog(fields, components, component, option);
            }
        });
    };
    if (j.is_array()) { // the /project/{key}/components response is an array of beans
        for (const nlohmann::json& node : j) {
            mergeOne(node);
        }
    } else {
        mergeOne(j);
    }
    Guarded([&] { SortComponentCatalog(fields, components); });
}

void CollectAgilePages(const nlohmann::json& j) {
    Guarded([&] {
        std::set<int> seenBoardIds;
        std::vector<int> boardIds;
        (void)ExtractSprintBoardIdsFromPage(j, seenBoardIds, boardIds);
    });
    Guarded([&] {
        std::set<std::string> seenSprintIds;
        std::vector<TrackerFieldOption> options;
        CollectSprintOptionsFromPage(j, seenSprintIds, options);
    });
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    const nlohmann::json j = smatchet::json_safe::ParseBounded(bytes, err);
    if (!err.empty()) {
        return 0; // malformed / too-deep / oversized never reaches the builders in production
    }
    ParseFieldDefinitions(j);
    ApplyCreateMeta(j);
    BuildOptionLists(j);
    MergeComponents(j);
    CollectAgilePages(j);
    return 0;
}
