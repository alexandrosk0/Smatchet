// libFuzzer driver for the Plane tracker-response mapping layer —
// smatchet::plane::MapPlaneWorkItem*ToCachedTicket + the pagination / query
// helpers (PlaneIssueMappingPure.cpp). These consume an ALREADY-PARSED
// nlohmann::json from a Plane REST work-item HTTP response and walk it through
// the most type-confused shape surface in the tracker layer: each of
// state_detail{} vs state(string), assignees[] as objects OR bare id strings,
// label_details[] vs labels[] with object-or-string elements, cycle_details vs
// cycle, type_detail vs type — every field has a "detailed object" and a "flat
// scalar" branch. Slice E2, testing-surface.md §6 P1 Gap 3 / security.md §59-60.
//
// The raw parse is already depth/node-bounded by json_safe::ParseBounded, so
// this driver fuzzes the *consuming* layer: it feeds fuzz bytes through
// ParseBounded (what the production fetcher sees) and drives the pure mappers on
// the resulting DOM. The per-issue mapper throws nlohmann::json::exception on
// malformed input by contract (the array variant catches internally); the bug
// classes we hunt (OOB / UB / bad alloc) surface via ASan+UBSan regardless.
// ExtractKeyFromPlaneQuery takes a raw string and parses internally, so it is
// exercised on every input independent of top-level JSON validity.
#include "PlaneIssueMappingPure.h"

#include "Json/BoundedJsonParse.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string bytes(reinterpret_cast<const char*>(data), size);

    // Raw-string entry point: parses internally, tolerant of malformed input.
    (void)smatchet::plane::ExtractKeyFromPlaneQuery(bytes);

    std::string err;
    const nlohmann::json j = smatchet::json_safe::ParseBounded(bytes, err);
    if (!err.empty()) {
        return 0;
    }

    using namespace smatchet::plane;
    const std::vector<UserDisplayLookup> users = {{"user-alice", "Alice Tester"}, {"user-bob", "Bob Reviewer"}};
    try {
        (void)MapPlaneWorkItemJsonToCachedTicket(j, "SMT", users);
        std::unordered_map<std::string, std::string> keyToId;
        (void)MapPlaneWorkItemsArrayToCachedTickets(j, "SMT", users, &keyToId);
        (void)NextPaginationCursor(j);
    } catch (const std::exception&) {
        // Contractual throw on an unexpected shape — not a crash.
    }
    return 0;
}
