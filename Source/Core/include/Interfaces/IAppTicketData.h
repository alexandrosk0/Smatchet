#ifndef SMATCHET_INTERFACES_IAPP_TICKET_DATA_H
#define SMATCHET_INTERFACES_IAPP_TICKET_DATA_H

// Narrow facet for read-only active-ticket data (AppController fan-in Phase 5). AppController
// implements it; the tickets.* command TU depends on this, not the full AppController.h.
// GetActiveTicketsSnapshot is called once per frame from the grid, but through a concrete
// AppController& and it is an out-of-line accessor (lock + shared_ptr copy, never inlined),
// so virtualizing it costs at most one vtable lookup/frame — not the inlined-hot-getter case
// the Pillar-1 exclusion protects (the adjacent inline GetActiveTicketsRevision stays off any
// facet). Rank-0 leaf: CachedTicket comes from the rank-0 CachedTicketTypes.h (no back-edge).

#include "CachedTicketTypes.h" // CachedTicket (rank-0)

#include <memory>
#include <vector>

class IAppTicketData {
  public:
    virtual ~IAppTicketData() = default;

    virtual std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_TICKET_DATA_H
