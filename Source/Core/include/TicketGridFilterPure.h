#pragma once

// Pure (ImGui-free, AppController-free) grid text-filter predicate lifted out of
// SmatchetActiveProjectGridTable.cpp's RebuildGridSortAndFilterProjection. Takes a plain
// field-id lookup callback instead of TrackerFieldCatalogIndex (which drags ConfigManager.h),
// so this stays bucket-A unit-testable with only CachedTicketTypes.h + TrackerFieldSchema.h.

#include "CachedTicketTypes.h"
#include "TrackerFieldSchema.h"

#include <functional>
#include <string>

/// True when `filter` (case-insensitive substring, empty always matches) is found in the
/// ticket id, the "summary" field, or the value of any user-type field (assignee, reporter,
/// watchers, or a custom user field) resolved via `fieldMetaLookup`.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter,
                             const std::function<const TrackerField*(const std::string&)>& fieldMetaLookup);
