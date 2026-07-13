// Gap-map Tier 5 `_detail` extraction for SmatchetFieldIconRender.cpp — the pure
// untrusted-input half of the priority-icon pipeline. The load-bearing cases are the
// CPP_CODE_AUDIT.md #14 same-origin confinement (a tracker-supplied `iconUrl` must never
// resolve to a foreign host) and ParsePriorityJson over hostile/degenerate tracker values.

#include "SmatchetFieldIconRender_detail.h"

#include <doctest/doctest.h>

#include <string>

using namespace SmatchetFieldIconRender::detail;

TEST_CASE("JoinDomainAndPath — trailing separators collapse to exactly one '/'") {
    CHECK(JoinDomainAndPath("https://j.example.com/", "/images/i.png") == "https://j.example.com/images/i.png");
    CHECK(JoinDomainAndPath("https://j.example.com\\", "images/i.png") == "https://j.example.com/images/i.png");
    CHECK(JoinDomainAndPath("  https://j.example.com  ", "") == "https://j.example.com");
}

TEST_CASE("slug normalisation — separators, case, unknown labels") {
    CHECK(NormalizeSlugFromLabel("Highest") == "highest");
    CHECK(NormalizeSlugFromLabel("  BLOCKER ") == "blocker");
    // Separator-carrying labels compact to a known slug ("Lo-West" style inputs).
    CHECK(NormalizeSlugFromLabel("hi-gh") == "high");
    CHECK(NormalizeSlugFromLabel("Won't Fix").empty());
    CHECK(NormalizeSlugFromLabel("").empty());
    CHECK(SlugIsKnown("critical"));
    CHECK_FALSE(SlugIsKnown("urgent"));
}

TEST_CASE("ExtractUrlHost — host isolation incl. ports, IPv6, and malformed inputs") {
    CHECK(ExtractUrlHost("https://J.Example.com/x/y?z") == "j.example.com");
    CHECK(ExtractUrlHost("http://host:8443/path") == "host");
    CHECK(ExtractUrlHost("https://[2001:db8::1]:8443/x") == "[2001:db8::1]");
    CHECK(ExtractUrlHost("https://host#frag") == "host");
    CHECK(ExtractUrlHost("no-scheme.example.com/x").empty());
    CHECK(ExtractUrlHost("").empty());
}

TEST_CASE("IconUrlHostAllowed — audit #14: only the tracker's own origin passes") {
    const std::string domain = "https://jira.example.com";
    CHECK(IconUrlHostAllowed("https://jira.example.com/images/p.png", domain));
    CHECK(IconUrlHostAllowed("https://JIRA.EXAMPLE.COM:443/images/p.png", domain));
    // The attack shape the finding documents: metadata-IP / foreign-host fetches.
    CHECK_FALSE(IconUrlHostAllowed("http://169.254.169.254/latest/meta-data/", domain));
    CHECK_FALSE(IconUrlHostAllowed("https://evil.example.net/p.png", domain));
    CHECK_FALSE(IconUrlHostAllowed("https://jira.example.com.evil.net/p.png", domain));
    // Schemeless config domain gets the https:// default before comparison.
    CHECK(IconUrlHostAllowed("https://jira.example.com/p.png", "jira.example.com"));
    CHECK_FALSE(IconUrlHostAllowed("not-a-url", domain));
}

TEST_CASE("ResolveJiraIconUrlReference — absolute, protocol-relative, site-relative, junk") {
    const std::string domain = "https://jira.example.com";
    CHECK(ResolveJiraIconUrlReference("https://jira.example.com/i.png", domain) == "https://jira.example.com/i.png");
    CHECK(ResolveJiraIconUrlReference("https://evil.example.net/i.png", domain).empty());
    CHECK(ResolveJiraIconUrlReference("//jira.example.com/i.png", domain) == "https://jira.example.com/i.png");
    CHECK(ResolveJiraIconUrlReference("//evil.example.net/i.png", domain).empty());
    CHECK(ResolveJiraIconUrlReference("/images/icons/p.png", domain) == "https://jira.example.com/images/icons/p.png");
    CHECK(ResolveJiraIconUrlReference("images/icons/p.png", domain).empty()); // bare relative — rejected
    CHECK(ResolveJiraIconUrlReference("   ", domain).empty());
}

TEST_CASE("ParsePriorityJson — object, double-encoded, bare label, hostile junk") {
    std::string iconUrl;
    std::string label;
    std::string slug;
    {
        const char* raw = R"({"iconUrl":"https://j/i.png","name":"Highest"})";
        REQUIRE(ParsePriorityJson(raw, iconUrl, label, slug));
        CHECK(iconUrl == "https://j/i.png");
        CHECK(label == "Highest");
        CHECK(slug == "highest");
    }
    {
        // Double-encoded (a JSON string whose payload is the object) still parses.
        const std::string raw = "\"{\\\"name\\\":\\\"Low\\\"}\"";
        REQUIRE(ParsePriorityJson(raw, iconUrl, label, slug));
        CHECK(label == "Low");
        CHECK(slug == "low");
    }
    {
        // `value` is the fallback label key.
        const char* raw = R"({"value":"Medium"})";
        REQUIRE(ParsePriorityJson(raw, iconUrl, label, slug));
        CHECK(label == "Medium");
        CHECK(slug == "medium");
    }
    {
        // Bare non-JSON label degrades to label(+slug), no iconUrl.
        REQUIRE(ParsePriorityJson("Critical", iconUrl, label, slug));
        CHECK(iconUrl.empty());
        CHECK(slug == "critical");
    }
    {
        // Unknown bare label still returns true with the label preserved for the tooltip.
        REQUIRE(ParsePriorityJson("Someday", iconUrl, label, slug));
        CHECK(label == "Someday");
        CHECK(slug.empty());
    }
    {
        // Wrong-typed members are ignored rather than thrown on.
        const char* raw = R"({"iconUrl":42,"name":["x"]})";
        CHECK_FALSE(ParsePriorityJson(raw, iconUrl, label, slug));
        CHECK(iconUrl.empty());
        CHECK(label.empty());
    }
    CHECK_FALSE(ParsePriorityJson("   ", iconUrl, label, slug));
}

TEST_CASE("UrlToCacheFileName — stable, distinct, filesystem-safe") {
    const std::string a = UrlToCacheFileName("https://j/i.png");
    CHECK(a == UrlToCacheFileName("https://j/i.png"));
    CHECK(a.rfind("icon_", 0) == 0);
    CHECK(a.substr(a.size() - 4) == ".bin");
    CHECK(a != UrlToCacheFileName("https://j/other.png"));
    CHECK(a.find('/') == std::string::npos);
}
