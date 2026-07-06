#ifndef SMATCHET_INTERFACES_IAPP_TICKET_MUTATIONS_H
#define SMATCHET_INTERFACES_IAPP_TICKET_MUTATIONS_H

// Narrow facet for single-ticket mutations (set field(s) / comment / worklog / transition /
// create) — AppController fan-in Phase 5. AppController implements it; the ticket.* mutation
// command TU depends on this instead of the full AppController.h.
//
// Rank-0 leaf. The rank-3 Tracker payload types (TrackerField, IssueDraft, IssueCreateResult)
// are forward-declared — pointer/const-ref params and a by-value std::future<IssueCreateResult>
// return don't require completeness at the declaration point; the ticket.* TU includes the full
// types. smatchet::ui::CancelToken and CachedTicket are themselves rank-0 (CancelToken.h /
// CachedTicketTypes.h), so they are included directly.
//
// GetActiveTicketsSnapshot is also declared on IAppTicketData; ticket.set_field's dry-run diff
// reads it, so it is mirrored here to keep this TU on a single facet. A single AppController
// override is the final overrider for both interfaces — no diamond, the two facets are unrelated.

#include "CachedTicketTypes.h" // CachedTicket (rank-0) — snapshot element
#include "CancelToken.h"       // smatchet::ui::CancelToken (rank-0) — defaulted by-value on CreateIssueAsync

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

struct TrackerField;      // rank-3 Tracker/TrackerFieldSchema.h
struct IssueDraft;        // rank-3 Tracker/IssueDraft.h
struct IssueCreateResult; // rank-3 Tracker/IssueCreatePipeline.h

class IAppTicketMutations {
  public:
    virtual ~IAppTicketMutations() = default;

    virtual const TrackerField* FindFieldById(const std::string& fieldId) const = 0;
    virtual bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                 const std::vector<std::string>& rawValues, std::string& outError) = 0;
    virtual bool AddIssueCommentPlain(const std::string& issueKey, const std::string& plainText,
                                      std::string& outError) = 0;
    virtual bool SubmitWorklog(const std::string& issueId, const std::string& timeSpent,
                               const std::string& timeRemaining, const std::string& adjustEstimate,
                               const std::string& workDescription, const std::string& startedDate,
                               std::string& outError) = 0;
    virtual std::int64_t QueueCreateOffline(const IssueDraft& draft) = 0;
    // CancelToken default mirrored on the AppController override too: concrete-typed callers
    // (AppController_LuaBindings, SmatchetNewIssueDraftUi) use the 1-arg form via static binding.
    virtual std::future<IssueCreateResult>
    CreateIssueAsync(const IssueDraft& draft, smatchet::ui::CancelToken cancel = smatchet::ui::CancelToken()) = 0;

    virtual std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_TICKET_MUTATIONS_H
