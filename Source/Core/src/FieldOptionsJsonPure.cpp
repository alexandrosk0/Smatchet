#include "FieldOptionsJsonPure.h"

#include "Json/BoundedJsonParse.h"
#include "Tracker/TrackerFieldValueParser.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace fieldoptions {

namespace {

nlohmann::json OptionToJsonAtDepth(const TrackerFieldOption& o, int depth) {
    nlohmann::json children = nlohmann::json::array();
    if (depth < kMaxOptionDepth) {
        for (const TrackerFieldOption& child : o.Children) {
            children.push_back(OptionToJsonAtDepth(child, depth + 1));
        }
    }
    return nlohmann::json::object({{"id", o.Id},
                                   {"value", o.Value},
                                   {"secondary", o.SecondaryValue},
                                   {"payload_json", o.PayloadJson},
                                   {"disabled", o.Disabled},
                                   {"children", std::move(children)}});
}

bool OptionFromJsonAtDepth(const nlohmann::json& j, TrackerFieldOption& out, int depth) {
    if (!j.is_object()) {
        return false;
    }
    // json::value throws type_error on a mistyped member; ParseFieldOptions turns that into "unreadable".
    const auto text = [&j](const char* key) { return j.value(key, std::string()); };
    out.Id = text("id");
    out.Value = text("value");
    out.SecondaryValue = text("secondary");
    out.PayloadJson = text("payload_json");
    out.Disabled = j.value("disabled", false);
    out.Children.clear();
    const auto it = j.find("children");
    if (depth < kMaxOptionDepth && it != j.end() && it->is_array()) {
        for (const auto& el : *it) {
            TrackerFieldOption child;
            if (OptionFromJsonAtDepth(el, child, depth + 1)) {
                out.Children.push_back(std::move(child));
            }
        }
    }
    return true;
}

// The compact {"id", "name"} form of the remembered workflow rows (LearnedWorkflowPure).
bool OptionFromIdNameJson(const nlohmann::json& entry, TrackerFieldOption& out) {
    if (!entry.is_object()) {
        return false;
    }
    const auto id = entry.find("id");
    if (id != entry.end()) {
        out.Id = JsonIdToString(*id); // a string or integer id; anything else reads as absent
    }
    const auto name = entry.find("name");
    if (name != entry.end() && name->is_string()) {
        out.Value = name->get<std::string>();
    }
    return !out.Id.empty() || !out.Value.empty();
}

template <typename ToJson> std::string SerializeEach(const std::vector<TrackerFieldOption>& options, ToJson toJson) {
    nlohmann::json arr = nlohmann::json::array();
    for (const TrackerFieldOption& option : options) {
        arr.push_back(toJson(option));
    }
    // Option text is tracker-supplied; replace invalid UTF-8 instead of throwing.
    return arr.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

template <typename FromJson>
bool ParseEach(const std::string& json, std::vector<TrackerFieldOption>& out, FromJson fromJson) {
    out.clear();
    const nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(json);
    if (!parsed.is_array()) {
        return false; // a parse error (discarded) or another shape
    }
    std::vector<TrackerFieldOption> options;
    options.reserve(parsed.size());
    try {
        for (const nlohmann::json& entry : parsed) {
            TrackerFieldOption option;
            if (fromJson(entry, option)) {
                options.push_back(std::move(option));
            }
        }
    } catch (const std::exception&) {
        return false; // a mistyped member: the row is unreadable, so treat it as absent
    }
    out = std::move(options);
    return true;
}

} // namespace

nlohmann::json FieldOptionToJson(const TrackerFieldOption& option) { return OptionToJsonAtDepth(option, 0); }

bool FieldOptionFromJson(const nlohmann::json& j, TrackerFieldOption& out) { return OptionFromJsonAtDepth(j, out, 0); }

std::string SerializeFieldOptions(const std::vector<TrackerFieldOption>& options) {
    return SerializeEach(options, FieldOptionToJson);
}

bool ParseFieldOptions(const std::string& json, std::vector<TrackerFieldOption>& out) {
    return ParseEach(json, out, FieldOptionFromJson);
}

std::string SerializeOptionIdNames(const std::vector<TrackerFieldOption>& options) {
    return SerializeEach(options, [](const TrackerFieldOption& option) {
        return nlohmann::json::object({{"id", option.Id}, {"name", option.Value}});
    });
}

bool ParseOptionIdNames(const std::string& json, std::vector<TrackerFieldOption>& out) {
    return ParseEach(json, out, OptionFromIdNameJson);
}

} // namespace fieldoptions
} // namespace smatchet
