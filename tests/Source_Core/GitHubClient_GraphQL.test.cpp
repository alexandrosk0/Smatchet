// GitHubClient_GraphQL.test.cpp — phase-7 coverage for the GraphQL surface
// added in support of the merge-gates interaction (review-thread resolve
// after a CodeRabbit short-circuit reject).
//
// Same shape as GitHubClient_PrSurface.test.cpp — only the pure helpers in
// GitHubClientHelpers are exercised. The live HTTP calls
// (`ResolveReviewThread`, `LookupReviewThreadIdForComment`) are covered by
// the recorded-fixture pattern at the response-parser level so a wire-format
// drift fails the doctest before it ships.

#include "GitHubClientHelpers.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

TEST_SUITE("GitHubClient_GraphQL") {

TEST_CASE("BuildGraphqlUrl maps api.github.com to /graphql") {
    CHECK(GitHubClientHelpers::BuildGraphqlUrl("https://api.github.com") == "https://api.github.com/graphql");
}

TEST_CASE("BuildGraphqlUrl strips trailing slashes before appending") {
    CHECK(GitHubClientHelpers::BuildGraphqlUrl("https://api.github.com/") == "https://api.github.com/graphql");
    CHECK(GitHubClientHelpers::BuildGraphqlUrl("https://api.github.com///") == "https://api.github.com/graphql");
}

TEST_CASE("BuildGraphqlUrl maps GitHub Enterprise /api/v3 to /api/graphql") {
    CHECK(GitHubClientHelpers::BuildGraphqlUrl("https://github.acme.com/api/v3") ==
          "https://github.acme.com/api/graphql");
}

TEST_CASE("BuildGraphqlUrl handles bare-host enterprise URL") {
    // Non-enterprise + non-api.github.com just appends /graphql.
    CHECK(GitHubClientHelpers::BuildGraphqlUrl("https://github.acme.com") == "https://github.acme.com/graphql");
}

TEST_CASE("BuildPullRequestReviewCommentNodeId encodes per GitHub convention") {
    // Spot-check against a known shape: Base64 of "012:PullRequestReviewComment42".
    // Hand-computed: "MDEyOlB1bGxSZXF1ZXN0UmV2aWV3Q29tbWVudDQy" (no padding —
    // 30 chars, length=30 → 4*(30/3)=40 chars output, no '=').
    const std::string got = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId("42");
    CHECK(got == "MDEyOlB1bGxSZXF1ZXN0UmV2aWV3Q29tbWVudDQy");
}

TEST_CASE("BuildPullRequestReviewCommentNodeId is deterministic for same input") {
    const std::string a = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId("12345");
    const std::string b = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId("12345");
    CHECK(a == b);
}

TEST_CASE("BuildPullRequestReviewCommentNodeId differs for different ids") {
    const std::string a = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId("1");
    const std::string b = GitHubClientHelpers::BuildPullRequestReviewCommentNodeId("2");
    CHECK_FALSE(a == b);
}

TEST_CASE("BuildResolveReviewThreadBody wire format") {
    const auto j = GitHubClientHelpers::BuildResolveReviewThreadBody("PRRT_kwDOABC");
    CHECK(j.contains("query"));
    CHECK(j["query"].is_string());
    const std::string q = j["query"].get<std::string>();
    CHECK(q.find("mutation") != std::string::npos);
    CHECK(q.find("resolveReviewThread") != std::string::npos);
    CHECK(q.find("isResolved") != std::string::npos);
    CHECK(j.contains("variables"));
    CHECK(j["variables"]["threadId"] == "PRRT_kwDOABC");
}

TEST_CASE("BuildLookupReviewThreadBody wire format") {
    const auto j = GitHubClientHelpers::BuildLookupReviewThreadBody("MDEyOlB1bGxSZXF1ZXN0UmV2aWV3Q29tbWVudDQy");
    CHECK(j.contains("query"));
    CHECK(j["query"].is_string());
    const std::string q = j["query"].get<std::string>();
    CHECK(q.find("query") != std::string::npos);
    CHECK(q.find("node(id: $nodeId)") != std::string::npos);
    CHECK(q.find("PullRequestReviewComment") != std::string::npos);
    CHECK(q.find("pullRequestReview") != std::string::npos);
    CHECK(q.find("thread { id }") != std::string::npos);
    CHECK(j["variables"]["nodeId"] == "MDEyOlB1bGxSZXF1ZXN0UmV2aWV3Q29tbWVudDQy");
}

TEST_CASE("ParseResolveReviewThreadResponse: happy path returns isResolved=true") {
    const std::string body =
        R"({"data":{"resolveReviewThread":{"thread":{"isResolved":true}}}})";
    bool isResolved = false;
    std::string err;
    CHECK(GitHubClientHelpers::ParseResolveReviewThreadResponse(body, isResolved, err));
    CHECK(isResolved == true);
    CHECK(err.empty());
}

TEST_CASE("ParseResolveReviewThreadResponse: isResolved=false is parsed but flagged") {
    const std::string body =
        R"({"data":{"resolveReviewThread":{"thread":{"isResolved":false}}}})";
    bool isResolved = true;
    std::string err;
    CHECK(GitHubClientHelpers::ParseResolveReviewThreadResponse(body, isResolved, err));
    CHECK(isResolved == false);
    CHECK(err.empty());
}

TEST_CASE("ParseResolveReviewThreadResponse: errors array surfaces") {
    const std::string body =
        R"({"errors":[{"message":"Could not resolve to a node with the global id of 'PRRT_xxx'."}]})";
    bool isResolved = true;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseResolveReviewThreadResponse(body, isResolved, err));
    CHECK(err.find("GraphQL error") != std::string::npos);
    CHECK(err.find("Could not resolve") != std::string::npos);
}

TEST_CASE("ParseResolveReviewThreadResponse: malformed JSON rejected") {
    bool isResolved = true;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseResolveReviewThreadResponse("{not json", isResolved, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("ParseResolveReviewThreadResponse: missing data object rejected") {
    bool isResolved = true;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseResolveReviewThreadResponse(R"({"meta":{"foo":1}})", isResolved, err));
    CHECK(err.find("data object") != std::string::npos);
}

TEST_CASE("ParseResolveReviewThreadResponse: empty body rejected") {
    bool isResolved = true;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseResolveReviewThreadResponse("", isResolved, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("ParseResolveReviewThreadResponse: missing isResolved bool rejected") {
    const std::string body =
        R"({"data":{"resolveReviewThread":{"thread":{}}}})";
    bool isResolved = true;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseResolveReviewThreadResponse(body, isResolved, err));
    CHECK(err.find("isResolved") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: happy path returns thread id") {
    const std::string body =
        R"({"data":{"node":{"pullRequestReview":{"thread":{"id":"PRRT_kwDOABC123"}}}}})";
    std::string threadId;
    std::string err;
    CHECK(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(threadId == "PRRT_kwDOABC123");
    CHECK(err.empty());
}

TEST_CASE("ParseLookupReviewThreadResponse: null node surfaces deletion message") {
    const std::string body = R"({"data":{"node":null}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("deleted") != std::string::npos);
    CHECK(threadId.empty());
}

TEST_CASE("ParseLookupReviewThreadResponse: missing pullRequestReview surfaces shape error") {
    const std::string body = R"({"data":{"node":{"foo":"bar"}}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("pullRequestReview") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: errors array surfaces") {
    const std::string body = R"({"errors":[{"message":"Field 'foo' does not exist."}]})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("GraphQL error") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: empty body rejected") {
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse("", threadId, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("ParseLookupReviewThreadResponse: empty thread id rejected") {
    const std::string body =
        R"({"data":{"node":{"pullRequestReview":{"thread":{"id":""}}}}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("empty") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: missing thread object rejected") {
    const std::string body = R"({"data":{"node":{"pullRequestReview":{}}}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("thread") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: missing thread id field rejected") {
    const std::string body =
        R"({"data":{"node":{"pullRequestReview":{"thread":{"foo":"bar"}}}}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("id") != std::string::npos);
}

TEST_CASE("ParseLookupReviewThreadResponse: non-string thread id rejected") {
    const std::string body =
        R"({"data":{"node":{"pullRequestReview":{"thread":{"id":42}}}}})";
    std::string threadId;
    std::string err;
    CHECK_FALSE(GitHubClientHelpers::ParseLookupReviewThreadResponse(body, threadId, err));
    CHECK(err.find("id") != std::string::npos);
}

} // TEST_SUITE GitHubClient_GraphQL

#endif // SMATCHET_WITH_AGENTIC
