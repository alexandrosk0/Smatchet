#include "AgentTriageScenarioFixtures.h"

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace smatchet {
namespace agentic {
namespace scenario_fixtures {

namespace {

// Returns the JSON string value for `key` if present and string-typed, else
// the supplied default. Centralises the "missing or wrong type" defaulting
// rule so callers never see a nullptr deref or a json::type_error.
std::string GetStr(const nlohmann::json& obj, const char* key, const std::string& fallback) {
    if (!obj.is_object()) return fallback;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

} // namespace

std::int64_t ParseIso8601Utc(const std::string& iso) {
    // Strict `YYYY-MM-DDTHH:MM:SSZ` — exactly 20 chars. Anything else returns 0.
    // We avoid std::get_time + std::mktime because Windows MSVC + UCRT do not
    // provide a portable UTC timegm() and we want zero TZ dependence. The
    // arithmetic here is the standard "days-since-epoch + seconds-of-day"
    // decomposition.
    //
    // Bundle C CR#233:41 — require exact length match. The previous `< 20`
    // check silently accepted any longer prefix (`2026-05-10T14:23:11Z+extra`)
    // by letting sscanf parse the first 20 chars and ignore the tail. Strict
    // exact-length matches the production parser in GitHubClientHelpers.
    if (iso.size() != 20) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char tsep = 0, zsep = 0;
    // %d picks up leading zeros via the standard format; the literal
    // `-` / `:` enforce shape.
    const int matched =
        std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &s, &zsep);
    if (matched != 7) return 0;
    (void)tsep;
    if (zsep != 'Z') return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return 0;

    // Howard Hinnant's days-from-civil algorithm — proleptic Gregorian.
    // Constant-time, no leap-year branches at runtime.
    const int yAdj = y - (mo <= 2 ? 1 : 0);
    const int era = (yAdj >= 0 ? yAdj : yAdj - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yAdj - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    return days * 86400 + static_cast<std::int64_t>(h) * 3600 + static_cast<std::int64_t>(mi) * 60 + s;
}

bool ParseGitHubIssueCommentsFixture(const std::string& jsonBody, std::vector<TrackerIssueComment>& outComments,
                                     std::string& outError) {
    outComments.clear();
    outError.clear();

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(jsonBody);
    } catch (const std::exception& ex) {
        outError = std::string("not JSON: ") + ex.what();
        return false;
    }
    if (!doc.is_array()) {
        outError = "expected top-level JSON array of comment objects.";
        return false;
    }

    outComments.reserve(doc.size());
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const nlohmann::json& el = doc[i];
        if (!el.is_object()) {
            outError = "element " + std::to_string(i) + " is not a JSON object.";
            outComments.clear();
            return false;
        }
        TrackerIssueComment c;

        // GitHub comment-id is a JSON integer; serialise as decimal string so
        // TrackerIssueComment::Id (string) holds the canonical handle.
        auto idIt = el.find("id");
        if (idIt != el.end() && idIt->is_number_integer()) {
            c.Id = std::to_string(idIt->get<std::int64_t>());
        }

        // user.login is the canonical author handle (GitHub maps the display
        // name to `user.login`; `user.name` is optional and often null).
        auto userIt = el.find("user");
        if (userIt != el.end() && userIt->is_object()) {
            c.Author = GetStr(*userIt, "login", std::string());
        }

        c.Body = GetStr(el, "body", std::string());

        const std::string created = GetStr(el, "created_at", std::string());
        const std::string updated = GetStr(el, "updated_at", std::string());
        c.CreatedAtSec = ParseIso8601Utc(created);
        c.UpdatedAtSec = ParseIso8601Utc(updated.empty() ? created : updated);

        outComments.push_back(std::move(c));
    }
    return true;
}

bool ParseOllamaProposalsFixture(const std::string& jsonBody,
                                 std::vector<AgenticInferenceClientPure::ProposalDraft>& outProposals,
                                 std::string& outError) {
    return AgenticInferenceClientPure::ParseInferenceResponse(jsonBody, outProposals, outError);
}

} // namespace scenario_fixtures
} // namespace agentic
} // namespace smatchet
