#include "IssueTableSerializer.h"

#include "Json/BoundedJsonParse.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace IssueTableSerializer {

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// Column header → field id resolver.
// Priority: literal field id (case-insensitive) > catalog display name
// (case-insensitive) > special keys > raw header.
std::string ResolveColumnKey(const std::string& header, const std::vector<TrackerField>& catalog) {
    const std::string trimmed = Trim(header);
    if (trimmed.empty())
        return {};
    const std::string low = ToLowerAsciiCopy(trimmed);

    // Before catalog: issue key column maps to draft.ExistingIssueKey (update path), not a Jira field id.
    if (low == "key" || low == "issuekey" || low == "issue key") {
        return "__existing_issue_key__";
    }

    auto idIt =
        std::find_if(catalog.begin(), catalog.end(), [&](const auto& f) { return ToLowerAsciiCopy(f.Id) == low; });
    if (idIt != catalog.end())
        return idIt->Id;

    auto nameIt =
        std::find_if(catalog.begin(), catalog.end(), [&](const auto& f) { return ToLowerAsciiCopy(f.Name) == low; });
    if (nameIt != catalog.end())
        return nameIt->Id;
    if (low == "project" || low == "projectkey" || low == "project key")
        return "project";
    if (low == "issuetype" || low == "issue type" || low == "type")
        return "issuetype";
    if (low == "parent" || low == "parentkey" || low == "parent key")
        return "parent";
    if (low == "attachments" || low == "attachment")
        return "attachments";
    if (low == "summary" || low == "title")
        return "summary";
    if (low == "description")
        return "description";
    return trimmed;
}

void ApplyKeyValueToDraft(IssueDraft& draft, const std::string& key, const std::string& rawValue) {
    if (key.empty())
        return;
    const std::string value = Trim(rawValue);
    if (key == "__existing_issue_key__") {
        // Skip empty cells so a duplicate "issuekey" column (often empty in exports)
        // does not overwrite a non-empty "key" column parsed earlier in the row.
        if (!value.empty()) {
            draft.ExistingIssueKey = value;
        }
        return;
    }
    if (key == "project") {
        draft.ProjectKey = value;
    } else if (key == "issuetype") {
        draft.IssueTypeName = value;
        // IssueTypeId is resolved later via catalog if possible; leave empty so
        // pipeline falls back to name.
        draft.IssueTypeId.clear();
    } else if (key == "parent") {
        draft.ParentKey = value;
    } else if (key == "attachments") {
        if (value.empty())
            return;
        // Exports serialize the attachments column as a JSON-ish array (e.g. "[]" when
        // empty, or "[{...}, ...]" for existing Jira attachments). Those are NOT local
        // file paths - don't stage them as upload sources. Only accept plain pipe-
        // separated absolute paths.
        if (value.front() == '[') {
            return;
        }
        std::stringstream ss(value);
        std::string path;
        while (std::getline(ss, path, '|')) {
            const std::string trimmedPath = Trim(path);
            if (trimmedPath.empty())
                continue;
            StagedAttachment att;
            att.AbsPath = trimmedPath;
            auto slash = trimmedPath.find_last_of("/\\");
            att.FileName = (slash == std::string::npos) ? trimmedPath : trimmedPath.substr(slash + 1);
            draft.StagedAttachments.push_back(std::move(att));
        }
    } else {
        draft.FieldValues[key] = value;
    }
}

namespace {

// RFC4180-ish CSV parse: respects quoted fields with "" escapes and embedded
// newlines. `delim` selects comma vs tab.
std::vector<std::vector<std::string>> ParseDelimited(const std::string& text, char delim) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool inQuotes = false;

    auto flushCell = [&]() {
        row.emplace_back(std::move(cell));
        cell.clear();
    };
    auto flushRow = [&]() {
        rows.emplace_back(std::move(row));
        row.clear();
    };

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell.push_back(c);
            }
        } else {
            if (c == '"') {
                inQuotes = true;
            } else if (c == delim) {
                flushCell();
            } else if (c == '\r') {
                // swallow; handled by \n or end
            } else if (c == '\n') {
                flushCell();
                flushRow();
            } else {
                cell.push_back(c);
            }
        }
    }
    if (!cell.empty() || !row.empty()) {
        flushCell();
        flushRow();
    }
    return rows;
}

std::string EscapeCsvCell(const std::string& s, char delim) {
    bool needsQuote =
        std::any_of(s.begin(), s.end(), [delim](char c) { return c == delim || c == '"' || c == '\n' || c == '\r'; });
    if (!needsQuote)
        return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"')
            out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

ImportResult ParseCsvOrTsv(const std::string& text, char delim, const std::vector<TrackerField>& catalog,
                           const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                           const std::string& fallbackIssueTypeName) {
    ImportResult result;
    result.DetectedFormat = (delim == '\t') ? Format::Tsv : Format::Csv;

    auto rows = ParseDelimited(text, delim);
    if (rows.empty()) {
        result.Error = "Empty table.";
        return result;
    }
    const auto& header = rows.front();
    std::vector<std::string> keys;
    keys.reserve(header.size());
    std::transform(header.begin(), header.end(), std::back_inserter(keys),
                   [&](const auto& h) { return ResolveColumnKey(h, catalog); });

    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        // Skip fully empty lines (e.g. trailing blank).
        bool anyNonEmpty = std::any_of(row.begin(), row.end(), [](const auto& c) { return !Trim(c).empty(); });
        if (!anyNonEmpty)
            continue;

        ImportRow ir;
        ir.SourceLine = static_cast<int>(r + 1);
        ir.Draft.ProjectKey = fallbackProjectKey;
        ir.Draft.IssueTypeId = fallbackIssueTypeId;
        ir.Draft.IssueTypeName = fallbackIssueTypeName;

        for (size_t i = 0; i < row.size() && i < keys.size(); ++i) {
            if (keys[i].empty())
                continue;
            ApplyKeyValueToDraft(ir.Draft, keys[i], row[i]);
        }
        result.Rows.push_back(std::move(ir));
    }
    return result;
}

// Coerce a single JSON value to the flat string the import draft stores.
// Simple arrays such as strings or labels flatten to a comma-separated list,
// recognising value-bearing and name-bearing objects; other objects dump raw.
std::string JsonValueToImportString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_null()) {
        return std::string();
    }
    if (value.is_array()) {
        std::string joined;
        for (const auto& el : value) {
            if (!joined.empty())
                joined.push_back(',');
            if (el.is_string())
                joined += el.get<std::string>();
            else if (el.is_object() && el.contains("value") && el["value"].is_string())
                joined += el["value"].get<std::string>();
            else if (el.is_object() && el.contains("name") && el["name"].is_string())
                joined += el["name"].get<std::string>();
            else
                joined += el.dump();
        }
        return joined;
    }
    return value.dump();
}

ImportResult ParseJson(const std::string& text, const std::vector<TrackerField>& catalog,
                       const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                       const std::string& fallbackIssueTypeName) {
    ImportResult result;
    result.DetectedFormat = Format::Json;

    // Import files are user-chosen but often externally-originated bytes (a shared issue-table
    // dump), so a depth bomb here is a real crash vector — bound the parse with import-sized
    // caps: 64 MiB (matches ConfigManager::LoadJsonFile's file cap), default depth 256, and a
    // 2M-node budget (~10x the default; a bulk import row is ~60 nodes, so this clears tens of
    // thousands of issues while still bounding heap growth).
    std::string parseErr;
    nlohmann::json root = smatchet::json_safe::ParseBounded(
        text, parseErr, /*maxBytes=*/64u * 1024u * 1024u, smatchet::json_safe::kDefaultMaxDepth,
        /*maxNodes=*/2000000u);
    if (!parseErr.empty()) {
        result.Error = std::string("JSON parse error: ") + parseErr;
        return result;
    }

    // Accept array | {"issues":[...]} | single object.
    const nlohmann::json* arr = nullptr;
    nlohmann::json singleWrap;
    if (root.is_array()) {
        arr = &root;
    } else if (root.is_object()) {
        if (root.contains("issues") && root["issues"].is_array()) {
            arr = &root["issues"];
        } else {
            singleWrap = nlohmann::json::array({root});
            arr = &singleWrap;
        }
    } else {
        result.Error = "JSON root must be array or object.";
        return result;
    }

    int idx = 0;
    for (const auto& item : *arr) {
        ++idx;
        ImportRow ir;
        ir.SourceLine = idx;
        ir.Draft.ProjectKey = fallbackProjectKey;
        ir.Draft.IssueTypeId = fallbackIssueTypeId;
        ir.Draft.IssueTypeName = fallbackIssueTypeName;

        if (!item.is_object()) {
            ir.Error = "Row is not a JSON object.";
            result.Rows.push_back(std::move(ir));
            continue;
        }

        // Some exports wrap values under "fields": { ... }; merge it in.
        auto applyObject = [&](const nlohmann::json& obj) {
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                const std::string key = ResolveColumnKey(it.key(), catalog);
                if (key.empty())
                    continue;
                const std::string value = JsonValueToImportString(*it);
                ApplyKeyValueToDraft(ir.Draft, key, value);
            }
        };

        if (item.contains("fields") && item["fields"].is_object()) {
            applyObject(item["fields"]);
            // top-level project/issuetype/parent still honored
            for (auto it = item.begin(); it != item.end(); ++it) {
                if (it.key() == "fields")
                    continue;
                nlohmann::json one = {{it.key(), *it}};
                applyObject(one);
            }
        } else {
            applyObject(item);
        }

        result.Rows.push_back(std::move(ir));
    }
    return result;
}

} // anonymous namespace

Format DetectFormat(const std::string& text) {
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)))
            continue;
        if (c == '[' || c == '{')
            return Format::Json;
        break;
    }
    // Inspect header line for tab vs comma.
    size_t nl = text.find('\n');
    const std::string first = text.substr(0, (nl == std::string::npos) ? text.size() : nl);
    const size_t tabs = std::count(first.begin(), first.end(), '\t');
    const size_t commas = std::count(first.begin(), first.end(), ',');
    if (tabs > commas)
        return Format::Tsv;
    return Format::Csv;
}

ImportResult ParseDrafts(const std::string& text, Format fmt, const std::vector<TrackerField>& catalog,
                         const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                         const std::string& fallbackIssueTypeName) {
    Format use = fmt;
    if (use == Format::Auto)
        use = DetectFormat(text);
    switch (use) {
    case Format::Json:
        return ParseJson(text, catalog, fallbackProjectKey, fallbackIssueTypeId, fallbackIssueTypeName);
    case Format::Tsv:
        return ParseCsvOrTsv(text, '\t', catalog, fallbackProjectKey, fallbackIssueTypeId, fallbackIssueTypeName);
    case Format::Csv:
    default:
        return ParseCsvOrTsv(text, ',', catalog, fallbackProjectKey, fallbackIssueTypeId, fallbackIssueTypeName);
    }
}

std::string SerializeTickets(const std::vector<CachedTicket>& tickets, const std::vector<std::string>& fieldIds,
                             Format fmt) {
    std::vector<std::string> cols = fieldIds;
    if (cols.empty()) {
        std::set<std::string> keys;
        for (const auto& t : tickets)
            for (const auto& kv : t.fieldValues)
                keys.insert(kv.first);
        cols.assign(keys.begin(), keys.end());
    }

    if (fmt == Format::Auto)
        fmt = Format::Csv;

    if (fmt == Format::Json) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& t : tickets) {
            nlohmann::json obj = nlohmann::json::object();
            if (!t.id.empty())
                obj["key"] = t.id;
            for (const auto& col : cols) {
                obj[col] = t.GetFieldValue(col);
            }
            arr.push_back(std::move(obj));
        }
        return arr.dump(2);
    }

    const char delim = (fmt == Format::Tsv) ? '\t' : ',';
    std::string out;
    // header: include implicit "key" first for round-tripping
    out += EscapeCsvCell("key", delim);
    for (const auto& c : cols) {
        out.push_back(delim);
        out += EscapeCsvCell(c, delim);
    }
    out.push_back('\n');

    for (const auto& t : tickets) {
        out += EscapeCsvCell(t.id, delim);
        for (const auto& c : cols) {
            out.push_back(delim);
            out += EscapeCsvCell(t.GetFieldValue(c), delim);
        }
        out.push_back('\n');
    }
    return out;
}

} // namespace IssueTableSerializer
