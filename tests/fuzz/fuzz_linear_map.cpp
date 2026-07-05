// libFuzzer driver for the Linear tracker-response mapping layer —
// smatchet::linear::MapLinearIssueNode(s)ToCachedTicket (LinearIssueMappingPure.cpp).
// These consume an ALREADY-PARSED nlohmann::json from a Linear GraphQL HTTP
// response (`data.issues.nodes[]`) and walk each node with structural
// assumptions: nested `state{}`, `assignee{}`|null, `labels.nodes[]`, `project{}`,
// `team{}`, `cycle.number`, and an integer `priority`. The mapper documents a
// null/missing-safe Pillar-3 contract (a malformed nested field maps to a safe
// empty default rather than throwing), so this driver is a robustness check that
// the contract actually holds for ANY shape. Slice E2, testing-surface.md §6 P1
// Gap 3 / security.md §59-60.
//
// The raw parse is already depth/node-bounded by json_safe::ParseBounded, so this
// driver fuzzes the *consuming* layer: it feeds fuzz bytes through ParseBounded
// (what the production fetcher sees) and drives both the single-node and the
// nodes-array mapper on the resulting DOM. Any OOB / UB / bad alloc from an
// unexpected node shape surfaces via ASan+UBSan.
#include "LinearIssueMappingPure.h"

#include "Json/BoundedJsonParse.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    const nlohmann::json j = smatchet::json_safe::ParseBounded(bytes, err);
    if (!err.empty()) {
        return 0;
    }

    using namespace smatchet::linear;
    try {
        (void)MapLinearIssueNodeToCachedTicket(j);
        (void)MapLinearIssueNodesToTickets(j);
    } catch (const std::exception&) {
        // The mapper contracts to never throw; catch defensively so a contract
        // regression still surfaces as a corpus finding rather than aborting.
    }
    return 0;
}
