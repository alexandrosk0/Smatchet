#include <doctest/doctest.h>

#include "Tracker/TrackerInputNormalisePure.h"

#include <string>

using namespace TrackerInputNormalise;
using TrackerParseExplain::ParseMessageId;

namespace {

SiteUrlParse Jira(const char* text) { return NormaliseSiteField(text, SiteField::JiraDomain); }
SiteUrlParse Plane(const char* text) { return NormaliseSiteField(text, SiteField::PlaneUrl); }
SiteUrlParse GitHubBase(const char* text) { return NormaliseSiteField(text, SiteField::GitHubBaseUrl); }
SiteUrlParse Owner(const char* text) { return NormaliseSiteField(text, SiteField::GitHubOwner); }

struct SiteCase {
    const char* Input;
    const char* Value;
    bool Rewritten;
};

} // namespace

TEST_CASE("Jira domain: a pasted browser URL is cut back to the host (#80)") {
    const SiteCase kCases[] = {
        {"acme.atlassian.net", "acme.atlassian.net", false},
        {"  acme.atlassian.net  ", "acme.atlassian.net", false}, // trimming alone is not a rewrite worth a note
        {"https://acme.atlassian.net", "acme.atlassian.net", true},
        {"https://acme.atlassian.net/", "acme.atlassian.net", true},
        {"https://acme.atlassian.net/jira/software/projects/PROJ/boards/1", "acme.atlassian.net", true},
        {"https://acme.atlassian.net/browse/PROJ-12", "acme.atlassian.net", true},
        {"https://acme.atlassian.net/jira/your-work", "acme.atlassian.net", true},
        {"ACME.Atlassian.NET", "acme.atlassian.net", true},
        {"acme.atlassian.net/browse/PROJ-1?x=1#y", "acme.atlassian.net", true},
        {"\"acme.atlassian.net\"", "acme.atlassian.net", true},
        {"<https://acme.atlassian.net/>", "acme.atlassian.net", true},
        // A server may live under a context path; that stays, the Jira routes after it go.
        {"https://jira.corp.example/jira", "jira.corp.example/jira", true},
        {"https://jira.corp.example/jira/browse/OPS-7", "jira.corp.example/jira", true},
        {"https://jira.corp.example/secure/Dashboard.jspa", "jira.corp.example", true},
        {"jira.corp.example:8443", "jira.corp.example:8443", false},
        // Atlassian Cloud has no context path, so a page that is not a Jira route is cut too; a server keeps it.
        {"https://acme.atlassian.net/wiki/spaces/ENG", "acme.atlassian.net", true},
        {"https://jira.corp.example/wiki/spaces/ENG", "jira.corp.example/wiki/spaces/ENG", true},
        // An explicit http:// is a choice the user made on purpose; it is kept.
        {"http://jira.corp.example", "http://jira.corp.example", false},
    };
    for (const SiteCase& c : kCases) {
        INFO("input: " << std::string(c.Input));
        const SiteUrlParse p = Jira(c.Input);
        CHECK(p.Ok);
        CHECK(p.Value == c.Value);
        CHECK(p.Rewritten == c.Rewritten);
        CHECK_FALSE(p.Message.IsError());
    }
    // Nothing typed is fine, and not a rewrite.
    CHECK(Jira("").Ok);
    CHECK(Jira("   ").Value.empty());
    CHECK_FALSE(Jira("   ").Rewritten);
}

TEST_CASE("Jira domain: text that is not a site is refused with the reason") {
    CHECK(Jira("acme atlassian net").Message.Id == ParseMessageId::SiteUrlHasSpaces);
    CHECK(Jira("ftp://acme.atlassian.net").Message.Id == ParseMessageId::SiteUrlNotHttp);
    CHECK(Jira("ftp://acme.atlassian.net").Message.Arg == "ftp");
    CHECK(Jira("https://").Message.Id == ParseMessageId::SiteUrlNoHost);
    CHECK(Jira("/browse/PROJ-1").Message.Id == ParseMessageId::SiteUrlNoHost);
    CHECK(Jira("acme$atlassian.net").Message.Id == ParseMessageId::SiteUrlNoHost);
    CHECK(Jira("-acme.net").Message.Id == ParseMessageId::SiteUrlNoHost);
    CHECK_FALSE(Jira("acme atlassian net").Ok);
    CHECK(Jira("acme atlassian net").Value.empty());
}

TEST_CASE("Plane URL: the web app address becomes the API base plus a workspace") {
    SiteUrlParse api = Plane("https://api.plane.so");
    CHECK(api.Ok);
    CHECK(api.Value == "https://api.plane.so");
    CHECK_FALSE(api.Rewritten);
    CHECK(api.WorkspaceSlug.empty());

    const SiteUrlParse app = Plane("https://app.plane.so/acme/projects/1234/issues/");
    CHECK(app.Ok);
    CHECK(app.Value == "https://api.plane.so");
    CHECK(app.Rewritten);
    CHECK(app.WorkspaceSlug == "acme");

    const SiteUrlParse bare = Plane("api.plane.so");
    CHECK(bare.Value == "https://api.plane.so"); // a missing scheme gets https
    CHECK(bare.Rewritten);

    const SiteUrlParse selfHosted = Plane("https://plane.corp.example/acme/projects");
    CHECK(selfHosted.Value == "https://plane.corp.example");
    CHECK(selfHosted.WorkspaceSlug == "acme");

    // A self-hosted install may sit behind a path prefix; saving must not cut it off.
    const SiteUrlParse prefixed = Plane("https://corp.example/plane");
    CHECK(prefixed.Value == "https://corp.example/plane");
    CHECK_FALSE(prefixed.Rewritten);
    CHECK(prefixed.WorkspaceSlug.empty());
    const SiteUrlParse prefixedApp = Plane("https://corp.example/plane/acme/projects/1");
    CHECK(prefixedApp.Value == "https://corp.example/plane");
    CHECK(prefixedApp.WorkspaceSlug == "acme");
    // Plane cloud: the API host, and the workspace only from the web app address.
    CHECK(Plane("https://api.plane.so/api/v1").Value == "https://api.plane.so");
    CHECK(Plane("https://api.plane.so/api/v1").WorkspaceSlug.empty());
    CHECK(Plane("https://app.plane.so/acme").WorkspaceSlug == "acme");

    CHECK(Plane("plane example").Message.Id == ParseMessageId::SiteUrlHasSpaces);
    CHECK(Plane("").Ok);
}

TEST_CASE("GitHub base URL: a repository URL becomes the API base and fills owner and repo") {
    const SiteUrlParse web = GitHubBase("https://github.com/octo/repo");
    CHECK(web.Ok);
    CHECK(web.Value == "https://api.github.com");
    CHECK(web.Owner == "octo");
    CHECK(web.Repo == "repo");
    CHECK(web.Rewritten);

    const SiteUrlParse git = GitHubBase("https://github.com/octo/repo.git");
    CHECK(git.Repo == "repo");

    const SiteUrlParse issues = GitHubBase("github.com/octo/repo/issues/12");
    CHECK(issues.Value == "https://api.github.com");
    CHECK(issues.Owner == "octo");
    CHECK(issues.Repo == "repo");

    const SiteUrlParse api = GitHubBase("https://api.github.com");
    CHECK(api.Value == "https://api.github.com");
    CHECK_FALSE(api.Rewritten);
    CHECK(api.Owner.empty());

    const SiteUrlParse ghe = GitHubBase("https://ghe.corp.example/octo/repo");
    CHECK(ghe.Value == "https://ghe.corp.example/api/v3");
    CHECK(ghe.Owner == "octo");
    CHECK(ghe.Repo == "repo");

    const SiteUrlParse gheApi = GitHubBase("https://ghe.corp.example/api/v3");
    CHECK(gheApi.Value == "https://ghe.corp.example/api/v3");
    CHECK_FALSE(gheApi.Rewritten);
    CHECK(gheApi.Owner.empty());

    CHECK(GitHubBase("git hub").Message.Id == ParseMessageId::SiteUrlHasSpaces);
    CHECK(GitHubBase("").Ok);
}

TEST_CASE("GitHub owner: owner, owner/repo and a repository URL all work") {
    const SiteUrlParse plain = Owner("octo");
    CHECK(plain.Ok);
    CHECK(plain.Value == "octo");
    CHECK(plain.Owner == "octo");
    CHECK(plain.Repo.empty());
    CHECK_FALSE(plain.Rewritten);

    const SiteUrlParse pair = Owner("octo/repo");
    CHECK(pair.Ok);
    CHECK(pair.Value == "octo");
    CHECK(pair.Repo == "repo");
    CHECK(pair.Rewritten);

    const SiteUrlParse url = Owner("https://github.com/octo/repo.git");
    CHECK(url.Ok);
    CHECK(url.Value == "octo");
    CHECK(url.Repo == "repo");

    const SiteUrlParse fromIssue = Owner("https://github.com/octo/repo/issues/12");
    CHECK(fromIssue.Owner == "octo");
    CHECK(fromIssue.Repo == "repo");

    for (const char* bad : {"octo repo", "octo/repo/extra", "oc$to", "octo/re po", "@octo", "octo//repo/x"}) {
        INFO("input: " << std::string(bad));
        const SiteUrlParse p = Owner(bad);
        CHECK_FALSE(p.Ok);
        CHECK(p.Message.Id == ParseMessageId::OwnerRepoShape);
        CHECK(p.Value.empty());
    }
    // A trailing slash is only a slip of the finger.
    CHECK(Owner("octo/").Ok);
    CHECK(Owner("octo/").Value == "octo");
    CHECK(Owner("octo/").Repo.empty());
    CHECK(Owner("https://github.com/octo").Value == "octo");
    CHECK(Owner("").Ok);
}

TEST_CASE("Executable path: Copy-as-path quotes and stray spaces are removed") {
    struct Case {
        const char* Input;
        const char* Value;
        bool Rewritten;
    };
    const Case kCases[] = {
        {"p4", "p4", false},
        {"C:\\Program Files\\Perforce\\p4.exe", "C:\\Program Files\\Perforce\\p4.exe", false},
        {"\"C:\\Program Files\\Perforce\\p4.exe\"", "C:\\Program Files\\Perforce\\p4.exe", true},
        {"  \"C:\\Perforce\\p4.exe\"  ", "C:\\Perforce\\p4.exe", true},
        {"'/usr/local/bin/p4'", "/usr/local/bin/p4", true},
        {"/usr/local/bin/p4  ", "/usr/local/bin/p4", false},
        {"\"  /usr/local/bin/p4  \"", "/usr/local/bin/p4", true},
        {"", "", false},
        {"   ", "", false},
    };
    for (const Case& c : kCases) {
        INFO("input: [" << std::string(c.Input) << "]");
        const PathParse p = NormaliseExecutablePath(c.Input);
        CHECK(p.Ok);
        CHECK(p.Value == c.Value);
        CHECK(p.Rewritten == c.Rewritten);
    }
}

TEST_CASE("Executable path: a folder or a line break is refused") {
    for (const char* folder : {"C:\\Perforce\\", "/usr/local/bin/", "\"C:\\Program Files\\Perforce\\\""}) {
        INFO("input: " << std::string(folder));
        const PathParse p = NormaliseExecutablePath(folder);
        CHECK_FALSE(p.Ok);
        CHECK(p.Message.Id == ParseMessageId::PathIsFolder);
    }
    const PathParse lines = NormaliseExecutablePath("p4\nrm -rf x");
    CHECK_FALSE(lines.Ok);
    CHECK(lines.Message.Id == ParseMessageId::PathHasLineBreak);
    CHECK(NormaliseExecutablePath("p4\r").Ok); // a trailing CR is trimmed, not a line break inside the path
}

TEST_CASE("Chat URL: only a link the app will open is accepted") {
    CHECK(ValidateHttpUrl("").Ok);
    CHECK(ValidateHttpUrl("   ").Ok);
    CHECK(ValidateHttpUrl("   ").Value.empty());

    const PathParse https = ValidateHttpUrl("https://chat.example.com/?q=x");
    CHECK(https.Ok);
    CHECK(https.Value == "https://chat.example.com/?q=x");
    CHECK_FALSE(https.Rewritten);
    CHECK(ValidateHttpUrl("http://localhost:3000").Ok);
    CHECK(ValidateHttpUrl("HTTPS://chat.example.com").Ok);
    const PathParse trimmed = ValidateHttpUrl("  https://chat.example.com  ");
    CHECK(trimmed.Ok);
    CHECK_FALSE(trimmed.Rewritten);
    CHECK(ValidateHttpUrl("\"https://chat.example.com\"").Rewritten);

    const PathParse noScheme = ValidateHttpUrl("chat.example.com");
    CHECK_FALSE(noScheme.Ok);
    CHECK(noScheme.Message.Id == ParseMessageId::HttpUrlMissingScheme);
    CHECK(noScheme.Message.Arg == "chat.example.com");

    const PathParse ftp = ValidateHttpUrl("ftp://chat.example.com");
    CHECK_FALSE(ftp.Ok);
    CHECK(ftp.Message.Id == ParseMessageId::HttpUrlUnsupportedScheme);
    CHECK(ftp.Message.Arg == "ftp");
    CHECK(ValidateHttpUrl("javascript://x").Message.Id == ParseMessageId::HttpUrlUnsupportedScheme);
    CHECK(ValidateHttpUrl("file:///etc/passwd").Message.Id == ParseMessageId::HttpUrlUnsupportedScheme);

    CHECK(ValidateHttpUrl("https://chat example.com").Message.Id == ParseMessageId::SiteUrlHasSpaces);
    CHECK(ValidateHttpUrl("https://").Message.Id == ParseMessageId::SiteUrlNoHost);
}

TEST_CASE("AcceptedOr: refused text never replaces the last value that worked") {
    const std::string kept = "https://chat.example.com";
    CHECK(AcceptedOr(ValidateHttpUrl("https://other.example.com"), kept) == "https://other.example.com");
    CHECK(AcceptedOr(ValidateHttpUrl("  \"https://other.example.com\"  "), kept) == "https://other.example.com");
    CHECK(AcceptedOr(ValidateHttpUrl("javascript://x"), kept) == kept);
    CHECK(AcceptedOr(ValidateHttpUrl("chat.example.com"), kept) == kept);
    // Empty is a valid choice (the optional link was cleared), not a refusal.
    CHECK(AcceptedOr(ValidateHttpUrl(""), kept).empty());

    const std::string p4 = "C:\\Perforce\\p4.exe";
    CHECK(AcceptedOr(NormaliseExecutablePath("\"D:\\Tools\\p4.exe\""), p4) == "D:\\Tools\\p4.exe");
    CHECK(AcceptedOr(NormaliseExecutablePath("C:\\Perforce\\"), p4) == p4);
}

TEST_CASE("Normalisers survive hostile input without throwing") {
    const std::string wall(20000, 'a');
    CHECK_NOTHROW(Jira(wall.c_str()));
    CHECK_NOTHROW(Owner((wall + "/" + wall).c_str()));
    CHECK_NOTHROW(NormaliseExecutablePath(wall));
    CHECK_NOTHROW(ValidateHttpUrl("https://" + wall));
    CHECK_NOTHROW(Jira(std::string("\xff\xfe://\x80/\x00"
                                   "x",
                                   9)
                           .c_str()));
    CHECK_NOTHROW(GitHubBase("://///"));
    CHECK_NOTHROW(Plane("////?#"));
    CHECK_FALSE(Jira("://").Ok);
}
