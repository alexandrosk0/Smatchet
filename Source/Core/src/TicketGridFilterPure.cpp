// Pure grid text-filter predicate — see TicketGridFilterPure.h for the contract.

#include "TicketGridFilterPure.h"

#include "StringUtil.h"

bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    if (ContainsCaseInsensitive(ticket.id, filter)) {
        return true;
    }
    for (const auto& fieldEntry : ticket.fieldValues) {
        if (ContainsCaseInsensitive(fieldEntry.second, filter)) {
            return true;
        }
    }
    return false;
}
