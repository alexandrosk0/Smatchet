#include "Sync/ViewListingDiffPure.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace smatchet {

namespace {

std::string Trimmed(const std::string& text) {
    static const char* const kSpace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string::npos) {
        return std::string();
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

} // namespace

bool SameViewQuery(const std::string& a, const std::string& b) { return Trimmed(a) == Trimmed(b); }

std::vector<std::string> DepartedFromListing(const SavedViewListing& previous, const std::vector<std::string>& listed,
                                             const std::string& query,
                                             const std::unordered_set<std::string>& stillHeld) {
    std::vector<std::string> departed;
    if (!previous.known || listed.empty() || !SameViewQuery(previous.query, query)) {
        return departed;
    }
    const std::unordered_set<std::string> current(listed.begin(), listed.end());
    std::unordered_set<std::string> seen; // overlapping result pages can list a key twice
    for (const std::string& key : previous.ids) {
        if (current.find(key) == current.end() && stillHeld.find(key) == stillHeld.end() && seen.insert(key).second) {
            departed.push_back(key);
        }
    }
    return departed;
}

std::vector<std::string> KeysStillListed(const SavedViewListing& listing, const std::vector<std::string>& keys) {
    const std::unordered_set<std::string> listed(listing.ids.begin(), listing.ids.end());
    std::vector<std::string> out;
    for (const std::string& key : keys) {
        if (listed.find(key) != listed.end()) {
            out.push_back(key);
        }
    }
    return out;
}

std::vector<std::string> ListingWithout(const SavedViewListing& listing, const std::vector<std::string>& keys) {
    const std::unordered_set<std::string> removed(keys.begin(), keys.end());
    std::vector<std::string> out;
    out.reserve(listing.ids.size());
    for (const std::string& key : listing.ids) {
        if (removed.find(key) == removed.end()) {
            out.push_back(key);
        }
    }
    return out;
}

std::string ViewListingToJson(const std::string& query, const std::vector<std::string>& ids) {
    nlohmann::json out = nlohmann::json::object();
    out["jql"] = query;
    out["ids"] = ids;
    return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

SavedViewListing ViewListingFromJson(const std::string& json, std::int64_t savedAtUnixSec) {
    SavedViewListing listing;
    const nlohmann::json parsed = json_safe::ParseBoundedOrDiscarded(json);
    if (!parsed.is_object() || !parsed.contains("jql") || !parsed.contains("ids") || !parsed["jql"].is_string() ||
        !parsed["ids"].is_array()) {
        return listing;
    }
    listing.query = parsed["jql"].get<std::string>();
    for (const nlohmann::json& id : parsed["ids"]) {
        if (id.is_string()) {
            listing.ids.push_back(id.get<std::string>());
        }
    }
    listing.savedAtUnixSec = savedAtUnixSec;
    listing.known = true;
    return listing;
}

} // namespace smatchet
