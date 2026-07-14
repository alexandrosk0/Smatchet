#ifndef SMATCHET_INTERFACES_IAPP_FIELDS_H
#define SMATCHET_INTERFACES_IAPP_FIELDS_H

// Narrow facet for the tracker field-catalog surface (list / get / refresh / icon-for) —
// AppController fan-in Phase 5. AppController implements it; the fields.* command TU
// (Commands/Builtin/BuiltinCommands_Fields.cpp) depends on this instead of the full
// AppController.h.
//
// Rank-0 leaf. The rank-3 TrackerField and TrackerConfig are forward-declared — the by-reference
// return / const-ref + pointer params don't require completeness at the declaration point; the
// fields.* TU includes the full types.
//
// FindFieldById is also declared on IAppTicketMutations and GetFieldCatalogError on IAppSync;
// both are mirrored here so the fields.* TU rides a single facet. A single AppController override
// is the final overrider for every base carrying the same signature — no diamond, the facets are
// unrelated bases.
//
// Pillar-1 note: GetAvailableFields is header-inlined and has per-frame callers (SmatchetUI's
// grid-frame catalog index, SmatchetGridHeaderUi's draft build), but every one of them binds a
// concrete AppController& / AppController*, so those sites keep static binding; only calls made
// through IAppFields& take the vtable slot, and there are none in a draw loop. This matches the
// IAppTicketData::GetActiveTicketsSnapshot / IAppSync::GetFieldCatalogError trade-off, not the
// inlined-hot-getter case the Pillar-1 exclusion protects.

#include <string>
#include <vector>

struct TrackerField;  // rank-3 Tracker/TrackerFieldSchema.h — reference return / pointer param
struct TrackerConfig; // rank-3 — const-ref param

class IAppFields {
  public:
    virtual ~IAppFields() = default;

    virtual const std::vector<TrackerField>& GetAvailableFields() const = 0;
    virtual const TrackerField* FindFieldById(const std::string& fieldId) const = 0;
    virtual bool RefreshFieldCatalog(const TrackerConfig& cfg) = 0;
    virtual const std::string& GetFieldCatalogError() const = 0;
    virtual bool TryGetFieldIconMapTarget(const std::string& fieldId, const TrackerField* field,
                                          const std::string& rawValue, std::string& outPathOrUrl) const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_FIELDS_H
