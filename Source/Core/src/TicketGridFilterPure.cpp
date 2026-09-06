// Pure grid text-filter predicate — see TicketGridFilterPure.h for the contract. Moved
// verbatim out of SmatchetActiveProjectGridTable.cpp's checkMatch lambda; behaviour is
// unchanged except that the match now also covers user-type fields beyond "summary".

#include "TicketGridFilterPure.h"

#include "StringUtil.h"
#include "Tracker/TrackerQuerySuggestCommon.h"

bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter,
                              const std::function<const TrackerField*(const std::string&)>& fieldMetaLookup) {
    if (filter.empty()) {
        return true;
    }
    if (ContainsCaseInsensitive(ticket.id, filter)) {
        return true;
    }
    if (ContainsCaseInsensitive(ticket.GetFieldValue("summary"), filter)) {
        return true;
    }
    for (const auto& fieldEntry : ticket.fieldValues) {
        const TrackerField* meta = fieldMetaLookup ? fieldMetaLookup(fieldEntry.first) : nullptr;
        if (meta && tracker_query_suggest::IsQueryUserField(*meta) && ContainsCaseInsensitive(fieldEntry.second, filter)) {
            return true;
        }
    }
    return false;
}
