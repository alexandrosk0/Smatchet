// libFuzzer driver for the GitHub tracker-response mapping layer —
// smatchet::github::Map*JsonToCachedTicket and the GraphQL→REST shape adapters
// (GitHubIssueSearchMapping.cpp). These consume an ALREADY-PARSED nlohmann::json
// from a GitHub REST / GraphQL HTTP response (issues, pull requests, commits) and
// walk it with structural assumptions — string-slicing `repository_url`, treating
// `labels[]` elements as objects OR bare strings, branching on the `pull_request`
// sub-object, reading nested `commit.author`/`committer`. Slice E2, testing-
// surface.md §6 P1 Gap 3 / security.md §59-60.
//
// The raw parse is already depth/node-bounded by json_safe::ParseBounded (the
// bare-json-parse-untrusted lint), so this driver fuzzes the *consuming* layer,
// not the parser: it feeds fuzz bytes through ParseBounded (exactly what the
// production caller sees) and, on a valid DOM, drives every pure mapper. The
// mappers throw nlohmann::json::exception on malformed structural shapes by
// contract, so those are caught; the crash classes we hunt (OOB / UB / bad
// alloc from an unexpected shape) surface via ASan+UBSan regardless of the catch.
#include "GitHubIssueSearchMapping.h"

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
        return 0; // malformed / too-deep / oversized never reaches the mappers in production
    }

    using namespace smatchet::github;
    try {
        CachedTicket ticket = MapIssueOrPullRequestJsonToCachedTicket(j, "owner", "repo");
        EnrichPullRequestFieldsFromJson(ticket, j);
        (void)MapCommitJsonToCachedTicket(j, "owner", "repo");
        (void)MapGraphQlNodeToRestShape(j);
        (void)MapGraphQlPullRequestNodeToRestPrShape(j);
        (void)MapGraphQlNodesToTickets(j, "owner", "repo", /*includePullRequests=*/true);
    } catch (const std::exception&) {
        // Contractual throw on an unexpected shape — not a crash.
    }
    return 0;
}
