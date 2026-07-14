#include "GitHubProjectsV2Pure.h"

#include "StringUtil.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace smatchet {
namespace github {

namespace {

// Grid-facing formatting for a GraphQL number scalar: %.10g keeps integers
// integral ("3", not "3.000000") and trims float noise.
std::string FormatNumberValue(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", value);
    return std::string(buf);
}

std::string JsonStringOr(const nlohmann::json& obj, const char* key, const std::string& fallback = std::string()) {
    if (!obj.is_object()) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

const nlohmann::json* JsonObjectAt(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) {
        return nullptr;
    }
    return &(*it);
}

const nlohmann::json* JsonArrayAt(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) {
        return nullptr;
    }
    return &(*it);
}

// The `data.organization.projectV2` / `data.user.projectV2` object — whichever
// half of the dual query resolved for this login. Null when neither did.
const nlohmann::json* FindProjectNode(const nlohmann::json& body) {
    const nlohmann::json* data = JsonObjectAt(body, "data");
    if (data == nullptr) {
        return nullptr;
    }
    const char* const kOwnerKinds[] = {"organization", "user"};
    for (const char* kind : kOwnerKinds) {
        const nlohmann::json* ownerNode = JsonObjectAt(*data, kind);
        if (ownerNode == nullptr) {
            continue;
        }
        const nlohmann::json* project = JsonObjectAt(*ownerNode, "projectV2");
        if (project != nullptr) {
            return project;
        }
    }
    return nullptr;
}

// Map a ProjectV2 field dataType to the grid's family + type strings. Returns
// false for the built-in projections this slice does not surface as columns.
bool MapDataTypeToFamily(const std::string& dataType, TrackerFieldFamily& outFamily, std::string& outType,
                         bool& outReadOnly) {
    if (dataType == "TEXT") {
        outFamily = TrackerFieldFamily::Text;
        outType = "string";
        return true;
    }
    if (dataType == "NUMBER") {
        outFamily = TrackerFieldFamily::Number;
        outType = "number";
        return true;
    }
    if (dataType == "DATE") {
        outFamily = TrackerFieldFamily::Date;
        outType = "date";
        return true;
    }
    if (dataType == "SINGLE_SELECT") {
        outFamily = TrackerFieldFamily::SelectSingle;
        outType = "string";
        return true;
    }
    if (dataType == "ITERATION") {
        // Iteration assignment needs the iteration-id picker this slice doesn't
        // build — surfaced as a read-only display column.
        outFamily = TrackerFieldFamily::Text;
        outType = "string";
        outReadOnly = true;
        return true;
    }
    return false;
}

} // namespace

const char* ProjectV2FieldsQueryDocument() {
    // Both owner kinds queried at once — exactly one resolves for a login; the
    // other half yields an errors[] entry the parser tolerates.
    return R"(query($owner: String!, $number: Int!) {
  organization(login: $owner) { projectV2(number: $number) { id title fields(first: 50) { nodes {
    ... on ProjectV2FieldCommon { id name dataType }
    ... on ProjectV2SingleSelectField { options { id name } } } } } }
  user(login: $owner) { projectV2(number: $number) { id title fields(first: 50) { nodes {
    ... on ProjectV2FieldCommon { id name dataType }
    ... on ProjectV2SingleSelectField { options { id name } } } } } }
})";
}

const char* ProjectV2ItemsQueryDocument() {
    return R"(query($project: ID!, $first: Int!, $after: String) {
  node(id: $project) { ... on ProjectV2 { items(first: $first, after: $after) {
    pageInfo { hasNextPage endCursor }
    nodes {
      content {
        ... on Issue { number repository { name owner { login } } }
        ... on PullRequest { number repository { name owner { login } } } }
      fieldValues(first: 50) { nodes {
        ... on ProjectV2ItemFieldTextValue { text field { ... on ProjectV2FieldCommon { id } } }
        ... on ProjectV2ItemFieldNumberValue { number field { ... on ProjectV2FieldCommon { id } } }
        ... on ProjectV2ItemFieldDateValue { date field { ... on ProjectV2FieldCommon { id } } }
        ... on ProjectV2ItemFieldSingleSelectValue { name field { ... on ProjectV2FieldCommon { id } } }
        ... on ProjectV2ItemFieldIterationValue { title field { ... on ProjectV2FieldCommon { id } } } } } } } } }
})";
}

const char* ProjectV2ItemForIssueQueryDocument() {
    return R"(query($owner: String!, $repo: String!, $number: Int!) {
  repository(owner: $owner, name: $repo) { issueOrPullRequest(number: $number) {
    ... on Issue { projectItems(first: 20) { nodes { id project { id } } } }
    ... on PullRequest { projectItems(first: 20) { nodes { id project { id } } } } } }
})";
}

const char* ProjectV2UpdateFieldValueMutationDocument() {
    return R"(mutation($project: ID!, $item: ID!, $field: ID!, $value: ProjectV2FieldValue!) {
  updateProjectV2ItemFieldValue(input: {projectId: $project, itemId: $item, fieldId: $field, value: $value}) {
    projectV2Item { id } }
})";
}

const char* ProjectV2ClearFieldValueMutationDocument() {
    return R"(mutation($project: ID!, $item: ID!, $field: ID!) {
  clearProjectV2ItemFieldValue(input: {projectId: $project, itemId: $item, fieldId: $field}) {
    projectV2Item { id } }
})";
}

std::string SlugifyProjectV2FieldName(const std::string& name) {
    std::string out;
    bool pendingDash = false;
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0) {
            if (pendingDash && !out.empty()) {
                out.push_back('-');
            }
            pendingDash = false;
            out.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            pendingDash = true;
        }
    }
    return out.empty() ? std::string("field") : out;
}

Result<ProjectV2Catalog, std::string> ParseProjectV2Catalog(const nlohmann::json& body) {
    using CatalogResult = Result<ProjectV2Catalog, std::string>;
    const nlohmann::json* project = FindProjectNode(body);
    if (project == nullptr) {
        return CatalogResult::Err("GitHub project not found for the configured owner + project number "
                                  "(check Preferences > Tracker > GitHub Project number)");
    }
    ProjectV2Catalog out;
    out.ProjectNodeId = JsonStringOr(*project, "id");
    out.ProjectTitle = JsonStringOr(*project, "title");
    if (out.ProjectNodeId.empty()) {
        return CatalogResult::Err("GitHub projectV2 response missing the project node id");
    }

    const nlohmann::json* fieldsObj = JsonObjectAt(*project, "fields");
    const nlohmann::json* nodes = fieldsObj ? JsonArrayAt(*fieldsObj, "nodes") : nullptr;
    if (nodes == nullptr) {
        return CatalogResult::Ok(std::move(out)); // a project with no representable fields is valid
    }

    std::set<std::string> takenSlugs;
    for (const auto& node : *nodes) {
        if (!node.is_object()) {
            continue;
        }
        const std::string nodeId = JsonStringOr(node, "id");
        const std::string name = JsonStringOr(node, "name");
        const std::string dataType = JsonStringOr(node, "dataType");
        if (nodeId.empty() || name.empty()) {
            continue;
        }
        TrackerField field;
        bool readOnly = false;
        if (!MapDataTypeToFamily(dataType, field.Family, field.Type, readOnly)) {
            continue; // built-in projection (TITLE / ASSIGNEES / …) — native columns cover it
        }
        // Unique lowercase slug id — see the header's field-id-scheme note.
        std::string slug = SlugifyProjectV2FieldName(name);
        int suffix = 2;
        while (takenSlugs.count(slug) != 0) {
            slug = SlugifyProjectV2FieldName(name) + "-" + std::to_string(suffix++);
        }
        takenSlugs.insert(slug);
        field.Id = "pv2." + slug;
        field.Name = name + " (project)"; // disambiguates vs same-named native columns in pickers
        field.SchemaSystem = "projects_v2";
        field.SchemaCustom = nodeId;
        field.ReadOnly = readOnly;
        field.IsCustom = true;
        const nlohmann::json* options = JsonArrayAt(node, "options");
        if (options != nullptr) {
            for (const auto& option : *options) {
                if (!option.is_object()) {
                    continue;
                }
                TrackerFieldOption out_option;
                out_option.Id = JsonStringOr(option, "id");
                out_option.Value = JsonStringOr(option, "name");
                if (!out_option.Id.empty() && !out_option.Value.empty()) {
                    field.AllowedValues.push_back(out_option.Value);
                    field.AllowedValueOptions.push_back(std::move(out_option));
                }
            }
        }
        out.Fields.push_back(std::move(field));
    }
    return CatalogResult::Ok(std::move(out));
}

Result<ProjectV2ItemsPage, std::string> ParseProjectV2ItemsPage(const nlohmann::json& body,
                                                                const std::vector<TrackerField>& catalogFields) {
    using PageResult = Result<ProjectV2ItemsPage, std::string>;
    const nlohmann::json* data = JsonObjectAt(body, "data");
    const nlohmann::json* node = data ? JsonObjectAt(*data, "node") : nullptr;
    const nlohmann::json* items = node ? JsonObjectAt(*node, "items") : nullptr;
    if (items == nullptr) {
        return PageResult::Err("GitHub projectV2 items response missing data.node.items");
    }

    ProjectV2ItemsPage out;
    const nlohmann::json* pageInfo = JsonObjectAt(*items, "pageInfo");
    if (pageInfo != nullptr) {
        const auto hasNextIt = pageInfo->find("hasNextPage");
        out.HasNext = hasNextIt != pageInfo->end() && hasNextIt->is_boolean() && hasNextIt->get<bool>();
        out.EndCursor = JsonStringOr(*pageInfo, "endCursor");
    }

    const nlohmann::json* nodes = JsonArrayAt(*items, "nodes");
    if (nodes == nullptr) {
        return PageResult::Ok(std::move(out));
    }

    for (const auto& item : *nodes) {
        if (!item.is_object()) {
            continue;
        }
        const nlohmann::json* content = JsonObjectAt(item, "content");
        if (content == nullptr) {
            continue; // draft items carry no issue/PR content — nothing to join onto
        }
        const nlohmann::json* repo = JsonObjectAt(*content, "repository");
        const nlohmann::json* owner = repo ? JsonObjectAt(*repo, "owner") : nullptr;
        const auto numberIt = content->find("number");
        if (repo == nullptr || owner == nullptr || numberIt == content->end() || !numberIt->is_number_integer()) {
            continue;
        }
        const std::string ownerLogin = JsonStringOr(*owner, "login");
        const std::string repoName = JsonStringOr(*repo, "name");
        if (ownerLogin.empty() || repoName.empty()) {
            continue;
        }
        ProjectV2ItemValues parsed;
        parsed.TicketKey = ownerLogin + "/" + repoName + "#" + std::to_string(numberIt->get<long long>());

        const nlohmann::json* fieldValues = JsonObjectAt(item, "fieldValues");
        const nlohmann::json* valueNodes = fieldValues ? JsonArrayAt(*fieldValues, "nodes") : nullptr;
        if (valueNodes != nullptr) {
            for (const auto& value : *valueNodes) {
                if (!value.is_object()) {
                    continue;
                }
                const nlohmann::json* fieldRef = JsonObjectAt(value, "field");
                const std::string fieldNodeId = fieldRef ? JsonStringOr(*fieldRef, "id") : std::string();
                if (fieldNodeId.empty()) {
                    continue;
                }
                std::string slugId;
                for (const auto& catalogField : catalogFields) {
                    if (catalogField.SchemaCustom == fieldNodeId) {
                        slugId = catalogField.Id;
                        break;
                    }
                }
                if (slugId.empty()) {
                    continue; // a built-in projection value — not one of our columns
                }
                std::string display;
                const auto numIt = value.find("number");
                if (value.contains("text") && value["text"].is_string()) {
                    display = value["text"].get<std::string>();
                } else if (numIt != value.end() && numIt->is_number()) {
                    display = FormatNumberValue(numIt->get<double>());
                } else if (value.contains("date") && value["date"].is_string()) {
                    display = value["date"].get<std::string>();
                } else if (value.contains("name") && value["name"].is_string()) {
                    display = value["name"].get<std::string>(); // single-select option name
                } else if (value.contains("title") && value["title"].is_string()) {
                    display = value["title"].get<std::string>(); // iteration title
                }
                if (!display.empty()) {
                    parsed.Values.emplace_back(slugId, std::move(display));
                }
            }
        }
        out.Items.push_back(std::move(parsed));
    }
    return PageResult::Ok(std::move(out));
}

Result<std::string, std::string> ParseProjectV2ItemIdForProject(const nlohmann::json& body,
                                                                const std::string& projectNodeId) {
    using ItemResult = Result<std::string, std::string>;
    const nlohmann::json* data = JsonObjectAt(body, "data");
    const nlohmann::json* repository = data ? JsonObjectAt(*data, "repository") : nullptr;
    const nlohmann::json* issueNode = repository ? JsonObjectAt(*repository, "issueOrPullRequest") : nullptr;
    if (issueNode == nullptr) {
        return ItemResult::Err("GitHub projectV2 item lookup: issue/PR not found");
    }
    const nlohmann::json* projectItems = JsonObjectAt(*issueNode, "projectItems");
    const nlohmann::json* nodes = projectItems ? JsonArrayAt(*projectItems, "nodes") : nullptr;
    if (nodes == nullptr) {
        return ItemResult::Ok(std::string());
    }
    for (const auto& node : *nodes) {
        if (!node.is_object()) {
            continue;
        }
        const nlohmann::json* project = JsonObjectAt(node, "project");
        if (project != nullptr && JsonStringOr(*project, "id") == projectNodeId) {
            return ItemResult::Ok(JsonStringOr(node, "id"));
        }
    }
    return ItemResult::Ok(std::string());
}

Result<nlohmann::json, std::string> BuildProjectV2FieldValue(const TrackerField& field,
                                                             const std::vector<std::string>& values) {
    using ValueResult = Result<nlohmann::json, std::string>;
    std::string raw;
    for (const auto& value : values) {
        raw = TrimCopy(value);
        if (!raw.empty()) {
            break;
        }
    }
    if (raw.empty()) {
        return ValueResult::Ok(nlohmann::json(nullptr)); // caller routes null to the clear mutation
    }
    if (field.ReadOnly) {
        return ValueResult::Err("GitHub project field '" + field.Name + "' is read-only in Smatchet");
    }

    nlohmann::json value = nlohmann::json::object();
    switch (field.Family) {
    case TrackerFieldFamily::Text:
        value["text"] = raw;
        return ValueResult::Ok(std::move(value));
    case TrackerFieldFamily::Number: {
        char* end = nullptr;
        const double parsed = std::strtod(raw.c_str(), &end);
        if (end == raw.c_str() || (end != nullptr && *end != '\0')) {
            return ValueResult::Err("GitHub project field '" + field.Name + "' expects a number (got '" + raw + "')");
        }
        value["number"] = parsed;
        return ValueResult::Ok(std::move(value));
    }
    case TrackerFieldFamily::Date: {
        // Strict YYYY-MM-DD — the shape the fetch path displays, so edits round-trip.
        const bool shapeOk = raw.size() == 10 && raw[4] == '-' && raw[7] == '-';
        bool digitsOk = shapeOk;
        if (shapeOk) {
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (i == 4 || i == 7) {
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(raw[i])) == 0) {
                    digitsOk = false;
                    break;
                }
            }
        }
        if (!digitsOk) {
            return ValueResult::Err("GitHub project field '" + field.Name + "' expects a YYYY-MM-DD date (got '" + raw +
                                    "')");
        }
        value["date"] = raw;
        return ValueResult::Ok(std::move(value));
    }
    case TrackerFieldFamily::SelectSingle: {
        for (const auto& option : field.AllowedValueOptions) {
            if (option.Id == raw || option.Value == raw) {
                value["singleSelectOptionId"] = option.Id;
                return ValueResult::Ok(std::move(value));
            }
        }
        return ValueResult::Err("GitHub project field '" + field.Name + "' has no option named '" + raw + "'");
    }
    default:
        return ValueResult::Err("GitHub project field '" + field.Name + "' is not editable in this slice");
    }
}

} // namespace github
} // namespace smatchet
