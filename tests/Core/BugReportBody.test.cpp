// BugReportBody / TextRedaction / ScreenshotCensor doctests — the pure half of
// the "Log a Bug" feature. docs/plans/active/log-a-bug-github.md Slice 2.

#include "BugReportService.h"
#include "ScreenshotCensor.h"
#include "TextRedaction.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace smatchet::diagnostics;
using smatchet::imaging::MosaicCensorInPlace;
using smatchet::imaging::RecommendedCensorBlock;
using smatchet::privacy::RedactLogLine;
using smatchet::privacy::RedactLogText;

// --------------------------------------------------------------------------
// TextRedaction
// --------------------------------------------------------------------------

TEST_CASE("RedactLogLine — scrubs auth header / secret params / userinfo / email") {
    CHECK(RedactLogLine("Authorization: Bearer ghp_AbCdEf1234567890abcdef").find("ghp_") == std::string::npos);
    CHECK(RedactLogLine("Authorization: Bearer ghp_AbCdEf1234567890abcdef").find("<redacted>") != std::string::npos);

    const std::string url = RedactLogLine("GET https://api.example.com/x?token=supersecretvalue123&page=2");
    CHECK(url.find("supersecretvalue123") == std::string::npos);
    CHECK(url.find("page=2") != std::string::npos); // non-secret param survives

    const std::string ui = RedactLogLine("connecting to https://alice:hunter2pw@db.internal/health");
    CHECK(ui.find("hunter2pw") == std::string::npos);
    CHECK(ui.find("alice") != std::string::npos);

    CHECK(RedactLogLine("mail to bob@example.com now").find("bob@example.com") == std::string::npos);
    CHECK(RedactLogLine("mail to bob@example.com now").find("<redacted-email>") != std::string::npos);
}

TEST_CASE("RedactLogLine — preserves 40-char git SHA, redacts longer opaque tokens") {
    const std::string sha = "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3"; // 40-char SHA-1
    CHECK(RedactLogLine("commit " + sha + " landed").find(sha) != std::string::npos);

    // 48-char base64-ish secret -> redacted.
    const std::string tok = "AbCdEf0123456789AbCdEf0123456789AbCdEf0123456789"; // 48 chars
    CHECK(RedactLogLine("key=" + tok).find(tok) == std::string::npos);
}

TEST_CASE("RedactLogText — applies per line, preserves newlines") {
    const std::string in = "line one bob@example.com\nline two clean\n";
    const std::string out = RedactLogText(in);
    CHECK(out.find("bob@example.com") == std::string::npos);
    CHECK(out.find("line two clean") != std::string::npos);
    // two newlines preserved
    CHECK(std::count(out.begin(), out.end(), '\n') == 2);
}

// --------------------------------------------------------------------------
// ScreenshotCensor
// --------------------------------------------------------------------------

TEST_CASE("MosaicCensorInPlace — replaces each block with its average (comp=1)") {
    // 2x2, one block of size 2 -> all pixels become the mean (0+10+20+30)/4 = 15.
    std::vector<unsigned char> px = {0, 10, 20, 30};
    MosaicCensorInPlace(px.data(), 2, 2, 1, 2);
    for (unsigned char v : px) {
        CHECK(v == 15);
    }
}

TEST_CASE("MosaicCensorInPlace — edge cells clamp in-bounds, no overrun") {
    // 3x1, block 2 -> cell0 = avg(0,10)=5, cell1(edge,1px)=avg(90)=90.
    std::vector<unsigned char> px = {0, 10, 90};
    MosaicCensorInPlace(px.data(), 3, 1, 1, 2);
    CHECK(px[0] == 5);
    CHECK(px[1] == 5);
    CHECK(px[2] == 90);
}

TEST_CASE("MosaicCensorInPlace — null / non-positive args are no-ops") {
    std::vector<unsigned char> px = {1, 2, 3, 4};
    MosaicCensorInPlace(nullptr, 2, 2, 1, 2);
    MosaicCensorInPlace(px.data(), 2, 2, 1, 0);
    CHECK(px[0] == 1); // unchanged
}

TEST_CASE("RecommendedCensorBlock — clamps to [12,48]") {
    CHECK(RecommendedCensorBlock(64, 64) == 12);     // 64/64=1 -> floor 12
    CHECK(RecommendedCensorBlock(800, 600) >= 12);   // min(800,600)/64≈9 -> floor 12
    CHECK(RecommendedCensorBlock(8000, 6000) == 48); // 6000/64≈94 -> ceil 48
    CHECK(RecommendedCensorBlock(0, 0) == 12);
}

// --------------------------------------------------------------------------
// ResolveBugReportTarget
// --------------------------------------------------------------------------

static TrackerConfig MakeCfg() {
    TrackerConfig cfg;
    cfg.BugReportGitHubOwner = "acme";
    cfg.BugReportGitHubRepo = "tracker";
    return cfg;
}

TEST_CASE("ResolveBugReportTarget — env token wins, defaults assets to issue repo") {
    TrackerConfig cfg = MakeCfg();
    cfg.GitHubPat = "user-pat";
    const ResolvedBugTarget t = ResolveBugReportTarget(cfg, "env-token");
    REQUIRE(t.Ok);
    CHECK(t.Owner == "acme");
    CHECK(t.Repo == "tracker");
    CHECK(t.Pat == "env-token"); // env beats cfg.GitHubPat
    CHECK(t.BaseUrl == "https://api.github.com");
    CHECK(t.AssetsOwner == "acme"); // defaults to issue repo
    CHECK(t.AssetsRepo == "tracker");
}

TEST_CASE("ResolveBugReportTarget — falls back env->GitHubPat->BugReportGitHubPat") {
    TrackerConfig cfg = MakeCfg();
    cfg.GitHubPat = "user-pat";
    CHECK(ResolveBugReportTarget(cfg, "").Pat == "user-pat");
    cfg.GitHubPat.clear();
    cfg.BugReportGitHubPat = "dedicated-pat";
    CHECK(ResolveBugReportTarget(cfg, "").Pat == "dedicated-pat");
}

TEST_CASE("ResolveBugReportTarget — explicit assets repo parsed") {
    TrackerConfig cfg = MakeCfg();
    cfg.GitHubPat = "p";
    cfg.BugReportAssetsRepo = "assets-org/blobs";
    const ResolvedBugTarget t = ResolveBugReportTarget(cfg, "");
    REQUIRE(t.Ok);
    CHECK(t.AssetsOwner == "assets-org");
    CHECK(t.AssetsRepo == "blobs");
}

TEST_CASE("ResolveBugReportTarget — relay mode overrides direct path, needs no PAT") {
    TrackerConfig cfg; // no owner/repo, no PAT
    cfg.BugReportRelayUrl = "https://relay.example.workers.dev/report";
    cfg.BugReportRelayKey = "shhh";
    const ResolvedBugTarget t = ResolveBugReportTarget(cfg, "");
    REQUIRE(t.Ok);
    CHECK(t.UseRelay);
    CHECK(t.RelayUrl == "https://relay.example.workers.dev/report");
    CHECK(t.RelayKey == "shhh");
    CHECK(t.Pat.empty()); // no client token needed in relay mode
}

TEST_CASE("ResolveBugReportTarget — relay wins even when direct owner/repo also set") {
    TrackerConfig cfg = MakeCfg();
    cfg.GitHubPat = "p";
    cfg.BugReportRelayUrl = "https://relay.example/report";
    const ResolvedBugTarget t = ResolveBugReportTarget(cfg, "env-token");
    REQUIRE(t.Ok);
    CHECK(t.UseRelay);
}

TEST_CASE("BuildRelayRequest — title/body/censored; screenshot only when present") {
    const nlohmann::json a = BuildRelayRequest("[Bug] x", "the body", "", false);
    CHECK(a["title"] == "[Bug] x");
    CHECK(a["body"] == "the body");
    CHECK(a["censored"] == false);
    CHECK_FALSE(a.contains("screenshotBase64"));

    const nlohmann::json b = BuildRelayRequest("t", "b", "QUJD", true);
    CHECK(b["censored"] == true);
    CHECK(b["screenshotBase64"] == "QUJD");
}

TEST_CASE("ResolveBugReportTarget — error paths: missing repo / no PAT / bad baseUrl") {
    TrackerConfig empty;
    CHECK_FALSE(ResolveBugReportTarget(empty, "tok").Ok); // no owner/repo

    TrackerConfig noPat = MakeCfg();
    CHECK_FALSE(ResolveBugReportTarget(noPat, "").Ok); // no token anywhere

    TrackerConfig badUrl = MakeCfg();
    badUrl.GitHubPat = "p";
    badUrl.BugReportGitHubBaseUrl = "http://not-https.example";
    const ResolvedBugTarget t = ResolveBugReportTarget(badUrl, "");
    CHECK_FALSE(t.Ok);
    CHECK(t.Error.find("base URL") != std::string::npos);
}

// --------------------------------------------------------------------------
// BuildMarkdownBody
// --------------------------------------------------------------------------

static ContextBundle MakeBundle() {
    ContextBundle b;
    b.Env["version"] = "1.2.3";
    b.Env["build_tag"] = "abc123";
    b.Env["os"] = "Windows";
    b.Env["arch"] = "x86_64";
    b.Env["tracker"] = "jira";
    b.Env["utc"] = "2026-05-30T12:00:00Z";
    b.Env["summary"] = nlohmann::json{{"read_only_mode", false}};
    b.LogLines.push_back("[INFO] started up");
    b.LogLines.push_back("[ERROR] token=supersecretvalue123 leaked");
    b.AuditEvents = nlohmann::json::array({nlohmann::json{{"action", "issue_create"}}});
    return b;
}

TEST_CASE("BuildMarkdownBody — header table + description + screenshot + redacted log") {
    BugReportOptions opts;
    opts.UserDescription = "Grid freezes when sorting by date";
    const std::string body = BuildMarkdownBody(opts, MakeBundle(), "![screenshot](https://x/y.png)");

    CHECK(body.find("| Version | 1.2.3 |") != std::string::npos);
    CHECK(body.find("| Active tracker | jira |") != std::string::npos);
    CHECK(body.find("Grid freezes when sorting by date") != std::string::npos);
    CHECK(body.find("![screenshot](https://x/y.png)") != std::string::npos);
    CHECK(body.find("<details><summary>Recent log") != std::string::npos);
    CHECK(body.find("<details><summary>Recent audit events") != std::string::npos);
    // log redaction applied inside the body
    CHECK(body.find("supersecretvalue123") == std::string::npos);
}

TEST_CASE("BuildMarkdownBody — empty description placeholder, no screenshot block") {
    BugReportOptions opts; // empty description
    const std::string body = BuildMarkdownBody(opts, MakeBundle(), "");
    CHECK(body.find("_(no description provided)_") != std::string::npos);
    CHECK(body.find("![screenshot") == std::string::npos);
}

TEST_CASE("BuildMarkdownBody — caps body and emits truncation marker on huge log") {
    ContextBundle big = MakeBundle();
    big.LogLines.clear();
    for (int i = 0; i < 5000; ++i) {
        big.LogLines.push_back("[INFO] this is a fairly long and repetitive log line number " + std::to_string(i));
    }
    BugReportOptions opts;
    opts.UserDescription = "huge";
    const std::string body = BuildMarkdownBody(opts, big, "");
    CHECK(body.size() <= 65536);
    CHECK(body.find("… (truncated)") != std::string::npos);
    // Newest line kept, oldest dropped.
    CHECK(body.find("line number 4999") != std::string::npos);
    CHECK(body.find("line number 0\n") == std::string::npos);
}
