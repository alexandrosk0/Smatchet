#pragma once

// TrackerFieldOptionLookup — find an option in a field's option tree, children included. Shared by the
// payload path and the display path so both resolve a value to the same option. Pure, header-only.

#include "Tracker/TrackerFieldSchema.h"

#include <string>
#include <vector>

namespace tracker_field_options {

inline const TrackerFieldOption* FindById(const std::vector<TrackerFieldOption>& options, const std::string& id) {
    for (const TrackerFieldOption& option : options) {
        if (option.HasId(id)) {
            return &option;
        }
        if (const TrackerFieldOption* nested = FindById(option.Children, id)) {
            return nested;
        }
    }
    return nullptr;
}

inline const TrackerFieldOption* FindByName(const std::vector<TrackerFieldOption>& options, const std::string& name) {
    for (const TrackerFieldOption& option : options) {
        if (option.HasName(name)) {
            return &option;
        }
        if (const TrackerFieldOption* nested = FindByName(option.Children, name)) {
            return nested;
        }
    }
    return nullptr;
}

/// Id-preferred: a full id pass before any name match, so a component literally named "10033" cannot
/// shadow the option whose id is 10033.
inline const TrackerFieldOption* FindByIdOrName(const std::vector<TrackerFieldOption>& options,
                                                const std::string& idOrName) {
    if (const TrackerFieldOption* byId = FindById(options, idOrName)) {
        return byId;
    }
    return FindByName(options, idOrName);
}

} // namespace tracker_field_options
