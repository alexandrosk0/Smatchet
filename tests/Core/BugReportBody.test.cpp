// BugReportBody / TextRedaction / ScreenshotCensor doctests — the pure half of
// the "Log a Bug" feature. docs/plans/shipped/log-a-bug-github.md Slice 2.

#include "BugReportService.h"
#include "ScreenshotCensor.h"
#include "TextRedaction.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace smatchet::diagnostics;
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

TEST_CASE("RedactLogLine — redacts a 36-char UUID token (Plane), keeps benign 36-char text") {
    const std::string uuid = "550e8400-e29b-41d4-a716-446655440000"; // 36 chars, 8-4-4-4-12
    const std::string out = RedactLogLine("plane workspace " + uuid + " synced");
    CHECK(out.find(uuid) == std::string::npos);
    CHECK(out.find("<redacted>") != std::string::npos);

    // A benign 36-char run that is NOT UUID-shaped (no 8-4-4-4-12 dash structure)
    // must survive — the pattern is shape-specific, not any 36-char string.
    const std::string benign = "abcdefghijklmnopqrstuvwxyz0123456789"; // 36 chars, no dashes
    CHECK(static_cast<int>(benign.size()) == 36);
    CHECK(RedactLogLine(benign).find(benign) != std::string::npos);
}

TEST_CASE("RedactLogLine — strips CR/LF/ANSI so server data cannot forge a log line") {
    // CR/LF would forge a second physical log line; ANSI CSI drives the terminal.
    const std::string injected = "user=bob\r\n2099-01-01 FAKE level=error forged\x1b[31m DANGER\x1b[0m";
    const std::string out = RedactLogLine(injected);
    CHECK(out.find('\r') == std::string::npos);
    CHECK(out.find('\n') == std::string::npos);
    CHECK(out.find('\x1b') == std::string::npos);
    // The visible text survives (neutralized, not dropped) but as one inert line.
    CHECK(out.find("user=bob") != std::string::npos);
    CHECK(out.find("DANGER") != std::string::npos);

    // A bare ESC and other C0 controls are stripped too (no partial-escape bypass).
    const std::string ctl = std::string("a\x1b") + "b\x07" + "c\x08" + "d";
    const std::string ctlOut = RedactLogLine(ctl);
    CHECK(ctlOut.find('\x1b') == std::string::npos);
    CHECK(ctlOut.find('\x07') == std::string::npos);
    CHECK(ctlOut.find('\x08') == std::string::npos);
}

// --------------------------------------------------------------------------
// ScreenshotCensor
// --------------------------------------------------------------------------

TEST_CASE("DownscaleToMaxDimension — shrinks longest side, updates dims, no-op when small") {
    using smatchet::imaging::DownscaleToMaxDimension;

    // 4x2 uniform (value 100), comp=1, maxDim=2 -> 2x1, averages stay 100.
    std::vector<unsigned char> a(8, 100);
    int w = 4, h = 2;
    DownscaleToMaxDimension(a, w, h, 1, 2);
    CHECK(w == 2);
    CHECK(h == 1);
    CHECK(a.size() == 2u);
    CHECK(a[0] == 100);
    CHECK(a[1] == 100);

    // Already within maxDim -> no-op.
    std::vector<unsigned char> b(12, 7); // 2x2 RGB
    int bw = 2, bh = 2;
    DownscaleToMaxDimension(b, bw, bh, 3, 64);
    CHECK(bw == 2);
    CHECK(bh == 2);
    CHECK(b.size() == 12u);

    // 1920x1009-ish aspect clamps to 1280 longest side.
    std::vector<unsigned char> big(static_cast<std::size_t>(1920) * 1009 * 3, 50);
    int gw = 1920, gh = 1009;
    DownscaleToMaxDimension(big, gw, gh, 3, 1280);
    CHECK(gw == 1280);
    CHECK(gh < 1009);
    CHECK(big.size() == static_cast<std::size_t>(gw) * gh * 3);
}

// --------------------------------------------------------------------------
// ResolveBugReportTarget
// --------------------------------------------------------------------------

static TrackerConfig MakeCfg() {
    TrackerConfig cfg;
    // Direct-mode helper: clear the seeded default relay so these cases exercise
    // the owner/repo/PAT path (relay overrides direct when its URL is set).
    cfg.BugReportRelayUrl.clear();
    cfg.BugReportRelayKey.clear();
    cfg.BugReportGitHubOwner = "acme";
    cfg.BugReportGitHubRepo = "tracker";
    return cfg;
}

TEST_CASE("ResolveBugReportTarget — env token wins, defaults assets to issue repo") {
    TrackerConfig cfg = MakeCfg();
    cfg.BugReportGitHubPat = "dedicated-pat";
    const ResolvedBugTarget t = ResolveBugReportTarget(cfg, "env-token");
    REQUIRE(t.Ok);
    CHECK(t.Owner == "acme");
    CHECK(t.Repo == "tracker");
    CHECK(t.Pat == "env-token"); // env beats the dedicated PAT
    CHECK(t.BaseUrl == "https://api.github.com");
    CHECK(t.AssetsOwner == "acme"); // defaults to issue repo
    CHECK(t.AssetsRepo == "tracker");
}

TEST_CASE("ResolveBugReportTarget — env -> dedicated BugReportGitHubPat ONLY (never tracker GitHubPat)") {
    TrackerConfig cfg = MakeCfg();
    // The tracker PAT must NOT be borrowed by the bug reporter.
    cfg.GitHubPat = "tracker-pat";
    const ResolvedBugTarget noDedicated = ResolveBugReportTarget(cfg, "");
    CHECK_FALSE(noDedicated.Ok); // tracker PAT present but unusable -> no token
    CHECK(noDedicated.Pat.empty());

    cfg.BugReportGitHubPat = "dedicated-pat";
    CHECK(ResolveBugReportTarget(cfg, "").Pat == "dedicated-pat");
    CHECK(ResolveBugReportTarget(cfg, "env-token").Pat == "env-token"); // env still wins
}

TEST_CASE("ResolveBugReportTarget — explicit assets repo parsed") {
    TrackerConfig cfg = MakeCfg();
    cfg.BugReportGitHubPat = "p";
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
    cfg.BugReportGitHubPat = "p";
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
    CHECK_FALSE(b.contains("dumpBase64"));

    // Crash dump fields included only when present; default name applied.
    const nlohmann::json c = BuildRelayRequest("t", "b", "", false, "REVG", "");
    CHECK(c["dumpBase64"] == "REVG");
    CHECK(c["dumpName"] == "crash.dmp");
    const nlohmann::json d = BuildRelayRequest("t", "b", "", false, "REVG", "crash-123.dmp");
    CHECK(d["dumpName"] == "crash-123.dmp");
}

TEST_CASE("ResolveBugReportTarget — fresh-install default seeds the project relay (issue #989 follow-up)") {
    // A brand-new TrackerConfig (no user edits) must resolve via the bundled
    // relay so bug/crash reporting works with zero setup.
    TrackerConfig fresh;
    const ResolvedBugTarget t = ResolveBugReportTarget(fresh, "");
    REQUIRE(t.Ok);
    CHECK(t.UseRelay);
    CHECK(t.RelayUrl == SmatchetDefaults::kDefaultBugReportRelayUrl);
    CHECK(t.RelayKey == SmatchetDefaults::kDefaultBugReportRelayKey);
    CHECK(t.Pat.empty());
}

TEST_CASE("ResolveBugReportTarget — error paths: missing repo / no PAT / bad baseUrl") {
    TrackerConfig empty;
    empty.BugReportRelayUrl.clear(); // disable the seeded relay to reach the direct-path errors
    empty.BugReportRelayKey.clear();
    CHECK_FALSE(ResolveBugReportTarget(empty, "tok").Ok); // no owner/repo

    TrackerConfig noPat = MakeCfg();
    CHECK_FALSE(ResolveBugReportTarget(noPat, "").Ok); // no token anywhere

    TrackerConfig badUrl = MakeCfg();
    badUrl.BugReportGitHubPat = "p";
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

TEST_CASE("BuildMarkdownBody — arch cell annotates emulation on arm64 host") {
    ContextBundle bundle = MakeBundle();
    bundle.Env["emulated"] = true;
    bundle.Env["host_arch"] = "arm64";
    BugReportOptions opts;
    const std::string body = BuildMarkdownBody(opts, bundle, "");
    CHECK(body.find("| OS / Arch | Windows / x86_64 (emulated on arm64) |") != std::string::npos);
}

TEST_CASE("BuildMarkdownBody — arch cell unannotated when native (not emulated)") {
    ContextBundle bundle = MakeBundle();
    bundle.Env["emulated"] = false;
    bundle.Env["host_arch"] = "x86_64"; // host == build target
    BugReportOptions opts;
    const std::string body = BuildMarkdownBody(opts, bundle, "");
    CHECK(body.find("| OS / Arch | Windows / x86_64 |") != std::string::npos);
    CHECK(body.find("emulated") == std::string::npos);
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

// ---- #989 regression: uncapped blocks + UTF-8-unsafe backstop → GitHub 422 ----

TEST_CASE("TruncateUtf8 — never splits a multi-byte sequence") {
    // "é" = 0xC3 0xA9. Cap landing on the continuation byte must back off.
    std::string s = u8"abcé";
    smatchet::diagnostics::TruncateUtf8(s, 4); // byte 4 = 0xA9 (continuation)
    CHECK(s == "abc");

    std::string ascii = "abcdef";
    smatchet::diagnostics::TruncateUtf8(ascii, 4);
    CHECK(ascii == "abcd");

    std::string untouched = u8"aé";
    smatchet::diagnostics::TruncateUtf8(untouched, 16);
    CHECK(untouched == u8"aé");
}

TEST_CASE("BuildMarkdownBody — huge crash-mode description is capped with marker (issue #989)") {
    BugReportOptions opts;
    // Crash mode seeds the description with the crash context incl. a log tail —
    // simulate a 100KB description ending in multi-byte chars.
    opts.UserDescription = "Smatchet closed unexpectedly.\n";
    for (int i = 0; i < 4000; ++i) {
        opts.UserDescription += u8"log line with unicode — arrows → and dashes — number " + std::to_string(i) + "\n";
    }
    const std::string body = BuildMarkdownBody(opts, MakeBundle(), "");
    CHECK(body.size() <= 65536);
    CHECK(body.find(u8"… _(description truncated") != std::string::npos);
    // Env/audit tail blocks must survive (description can't starve them).
    CHECK(body.find("<details><summary>Recent log") != std::string::npos);
}

TEST_CASE("BuildMarkdownBody — huge audit-event dump is capped with marker (issue #989)") {
    ContextBundle bundle = MakeBundle();
    bundle.AuditEvents = nlohmann::json::array();
    for (int i = 0; i < 2000; ++i) {
        bundle.AuditEvents.push_back({{"event", "field-edit"},
                                      {"ticket", "SMA-" + std::to_string(i)},
                                      {"detail", u8"long value with unicode → → → padding padding padding"}});
    }
    BugReportOptions opts;
    opts.UserDescription = "audit flood";
    const std::string body = BuildMarkdownBody(opts, bundle, "");
    CHECK(body.size() <= 65536);
    CHECK(body.find("… (truncated)") != std::string::npos);
}

TEST_CASE("BuildIssueTitle — first line, 256-char GitHub cap, UTF-8-safe (issue #989)") {
    CHECK(smatchet::diagnostics::BuildIssueTitle("") == "[Bug] Report from Smatchet");
    CHECK(smatchet::diagnostics::BuildIssueTitle("short title\nrest") == "[Bug] short title");

    std::string longLine(300, 'x');
    const std::string capped = smatchet::diagnostics::BuildIssueTitle(longLine + "\nbody");
    CHECK(capped.size() < 256);
    CHECK(capped.find(u8"…") != std::string::npos);

    // Multi-byte char straddling the cap must not be split: the é starts at
    // byte 199, the 200-byte cap lands on its continuation byte, so the whole
    // é is dropped — no lone 0xC3 lead byte may survive.
    std::string tricky(199, 'y');
    tricky += u8"é"; // bytes 199-200 = the 2-byte é
    const std::string safe = smatchet::diagnostics::BuildIssueTitle(tricky);
    CHECK(safe.find('\xC3') == std::string::npos);
    CHECK(safe.find(u8"…") != std::string::npos);
}
