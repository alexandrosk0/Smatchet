#pragma once

// Pure (ImGui-free, AppController-free) grid text-filter predicate lifted out of
// SmatchetActiveProjectGridTable.cpp's RebuildGridSortAndFilterProjection. Decoupled
// from TrackerFieldCatalogIndex (which drags ConfigManager.h) via a plain field-id ->
// TrackerField* lookup callback, so this stays bucket-A unit-testable with only
// CachedTicketTypes.h + TrackerFieldSchema.h.

#include "CachedTicketTypes.h"
#include "TrackerFieldSchema.h"

#include <functional>
#include <string>

/// True when `filter` matches the ticket for the grid's `Filter...` text box: an empty
/// filter always matches; otherwise the ticket matches when `filter` is a case-insensitive
/// substring of the ticket id, the "summary" field, or the value of any user-type field
/// (assignee, reporter, watchers, or a backend-specific custom user field). `fieldMetaLookup`
/// resolves a field id to its catalog metadata (or nullptr when unknown) so a person's name
/// is found regardless of which user field it lives in.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter,
                             const std::function<const TrackerField*(const std::string&)>& fieldMetaLookup);
