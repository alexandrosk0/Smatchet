#pragma once

#include "FilterDropdown.h"
#include "Tracker/TrackerFieldSchema.h"

// How a tracker field option becomes a row of the shared dropdown. Every picker that lists
// TrackerFieldOption values goes through here, so they all pick, compare and remember an option by the
// same id.

namespace smatchet {
namespace dropdown {

/// The id an option is picked, compared and remembered by: its Id, or its Value when it has none. An
/// option without an Id must never be picked as "" (that would read as a field clear).
inline const char* OptionPickId(const TrackerFieldOption& option) {
    return option.Id.empty() ? option.Value.c_str() : option.Id.c_str();
}

/// A dropdown row for `option`: the id above, the Value as the label and the SecondaryValue (when it
/// has one) as the dimmed text. Checked, Disabled and Tooltip are the caller's. The row points into
/// `option`, which must outlive the DrawBody call.
inline Item ItemForOption(const TrackerFieldOption& option) {
    Item item;
    item.Id = OptionPickId(option);
    item.Label = option.Value.c_str();
    item.Secondary = option.SecondaryValue.empty() ? nullptr : option.SecondaryValue.c_str();
    return item;
}

} // namespace dropdown
} // namespace smatchet
