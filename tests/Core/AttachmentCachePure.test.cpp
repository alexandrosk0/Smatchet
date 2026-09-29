// AttachmentCachePure — the rules behind the attachment disk cache (offline-first S13): the entry key, the
// file-name sanitizer (a hostile attachment name can never escape its entry directory or collide with a
// Windows device), the LRU eviction choice with its grace window, and the entry.json codec.

#include "AttachmentCachePure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::attachments::CacheEntryInfo;
using smatchet::attachments::EntryKeyForUrl;
using smatchet::attachments::EntryMeta;
using smatchet::attachments::IsEntryKey;
using smatchet::attachments::kMaxFileNameBytes;
using smatchet::attachments::ParseEntryMeta;
using smatchet::attachments::SanitizeFileName;
using smatchet::attachments::SelectEvictions;
using smatchet::attachments::SerializeEntryMeta;
using smatchet::attachments::StoredFileName;

namespace {

CacheEntryInfo Entry(const std::string& key, std::uint64_t size, std::int64_t lastUsed, bool complete = true) {
    CacheEntryInfo e;
    e.Key = key;
    e.SizeBytes = size;
    e.LastUsedEpochSec = lastUsed;
    e.Complete = complete;
    return e;
}

} // namespace

TEST_SUITE("AttachmentCachePure") {

    TEST_CASE("entry keys are one 64-hex directory per URL") {
        const std::string a = EntryKeyForUrl("https://site.atlassian.net/rest/api/3/attachment/content/1");
        const std::string b = EntryKeyForUrl("https://site.atlassian.net/rest/api/3/attachment/content/2");
        CHECK(IsEntryKey(a));
        CHECK(IsEntryKey(b));
        CHECK(a != b);
        CHECK(a == EntryKeyForUrl("https://site.atlassian.net/rest/api/3/attachment/content/1"));
        CHECK_FALSE(IsEntryKey("notes"));
        CHECK_FALSE(IsEntryKey(a.substr(1)));
        std::string upper = a;
        upper[0] = 'A';
        CHECK_FALSE(IsEntryKey(upper));
    }

    TEST_CASE("the sanitizer keeps a hostile name inside one path component") {
        CHECK(SanitizeFileName("report.pdf") == "report.pdf");
        CHECK(SanitizeFileName("../../etc/passwd") == "_.._etc_passwd");
        CHECK(SanitizeFileName("..\\..\\boot.ini") == "_.._boot.ini");
        CHECK(SanitizeFileName("a:b*c?d\"e<f>g|h") == "a_b_c_d_e_f_g_h");
        CHECK(SanitizeFileName("tab\there\nnew\x7F") == "tabherenew");
        CHECK(SanitizeFileName("  .hidden.txt. ") == "hidden.txt");
        CHECK(SanitizeFileName("..") == "attachment");
        CHECK(SanitizeFileName("") == "attachment");
        CHECK(SanitizeFileName(" . ") == "attachment");
    }

    TEST_CASE("Windows device names get a prefix, lookalikes do not") {
        CHECK(SanitizeFileName("CON") == "_CON");
        CHECK(SanitizeFileName("con.txt") == "_con.txt");
        CHECK(SanitizeFileName("nul .log") == "_nul .log");
        CHECK(SanitizeFileName("COM1.log") == "_COM1.log");
        CHECK(SanitizeFileName("lpt9") == "_lpt9");
        CHECK(SanitizeFileName("conout$.txt") == "_conout$.txt");
        CHECK(SanitizeFileName("console.txt") == "console.txt");
        CHECK(SanitizeFileName("com10.txt") == "com10.txt");
        CHECK(SanitizeFileName("COM0") == "COM0");
    }

    TEST_CASE("a long name is shortened on a UTF-8 boundary and keeps its extension") {
        const std::string longName = std::string(300, 'a') + ".pdf";
        const std::string shortened = SanitizeFileName(longName);
        CHECK(shortened.size() == kMaxFileNameBytes);
        CHECK(shortened.substr(shortened.size() - 4) == ".pdf");

        std::string accents;
        for (int i = 0; i < 100; ++i) {
            accents += "\xC3\xA9"; // é: two bytes each
        }
        const std::string cut = SanitizeFileName(accents);
        CHECK(cut.size() <= kMaxFileNameBytes);
        CHECK(cut.size() % 2 == 0); // never half a character
        CHECK(static_cast<unsigned char>(cut.back()) == 0xA9u);
    }

    TEST_CASE("sanitizing is idempotent, so entry.json's check accepts every stored name") {
        const std::vector<std::string> inputs = {
            "report.pdf", "../x", "CON", std::string(300, 'b'), " .a. ", "nul.txt", std::string(250, 'c') + ".jpeg",
            "tab\tname"};
        for (const std::string& in : inputs) {
            const std::string once = SanitizeFileName(in);
            CHECK(SanitizeFileName(once) == once);
            const std::string stored = StoredFileName(in, ".png");
            CHECK(SanitizeFileName(stored) == stored);
            CHECK(stored.size() <= kMaxFileNameBytes);
        }
    }

    TEST_CASE("the stored name gains the MIME extension only when it has none") {
        CHECK(StoredFileName("screenshot", ".png") == "screenshot.png");
        CHECK(StoredFileName("screenshot.jpg", ".png") == "screenshot.jpg");
        CHECK(StoredFileName("", ".pdf") == "attachment.pdf");
        CHECK(StoredFileName("blob", "/evil") == "blob");  // not an extension: not appended
        CHECK(StoredFileName("blob", ".exe..") == "blob"); // not a plain extension
        const std::string stored = StoredFileName(std::string(200, 'z'), ".png");
        CHECK(stored.size() == kMaxFileNameBytes);
        CHECK(stored.substr(stored.size() - 4) == ".png");
    }

    TEST_CASE("under the cap nothing is evicted") {
        const std::vector<std::string> evict =
            SelectEvictions({Entry("a", 10, 100), Entry("b", 10, 200)}, 20, 10000, 600);
        CHECK(evict.empty());
    }

    TEST_CASE("over the cap the least recently used entries go first, until it fits") {
        const std::vector<std::string> evict =
            SelectEvictions({Entry("new", 10, 9000), Entry("old", 10, 1000), Entry("mid", 10, 5000)}, 15, 100000, 600);
        REQUIRE(evict.size() == 2u);
        CHECK(evict[0] == "old");
        CHECK(evict[1] == "mid");
    }

    TEST_CASE("an entry used within the grace window is never evicted, even over the cap") {
        const std::int64_t now = 100000;
        const std::vector<std::string> evict =
            SelectEvictions({Entry("old", 10, 1000), Entry("shown", 10, now - 60)}, 5, now, 600);
        REQUIRE(evict.size() == 1u);
        CHECK(evict[0] == "old");
    }

    TEST_CASE("an interrupted download is removed once its grace window passes, cap or not") {
        const std::int64_t now = 100000;
        const std::vector<std::string> evict = SelectEvictions(
            {Entry("stale-part", 3, 1000, false), Entry("fresh-part", 3, now - 10, false), Entry("ok", 3, 2000)}, 1000,
            now, 600);
        REQUIRE(evict.size() == 1u);
        CHECK(evict[0] == "stale-part");
    }

    TEST_CASE("ties in last use evict in key order, so the choice is deterministic") {
        const std::vector<std::string> evict =
            SelectEvictions({Entry("b", 10, 100), Entry("a", 10, 100), Entry("c", 10, 100)}, 10, 100000, 0);
        REQUIRE(evict.size() == 2u);
        CHECK(evict[0] == "a");
        CHECK(evict[1] == "b");
    }

    TEST_CASE("entry metadata round-trips") {
        EntryMeta meta;
        meta.Url = "https://site.atlassian.net/rest/api/3/attachment/content/10001";
        meta.Mime = "image/png";
        meta.FileName = "diagram.png";
        EntryMeta back;
        REQUIRE(ParseEntryMeta(SerializeEntryMeta(meta), back));
        CHECK(back.Url == meta.Url);
        CHECK(back.Mime == meta.Mime);
        CHECK(back.FileName == meta.FileName);
    }

    TEST_CASE("tampered or malformed metadata is rejected") {
        EntryMeta out;
        CHECK_FALSE(ParseEntryMeta("not json", out));
        CHECK_FALSE(ParseEntryMeta("[1,2]", out));
        CHECK_FALSE(ParseEntryMeta(R"({"file":"a.png"})", out));
        CHECK_FALSE(ParseEntryMeta(R"({"url":"","file":"a.png"})", out));
        CHECK_FALSE(ParseEntryMeta(R"({"url":"https://x","file":"../../outside.png"})", out));
        CHECK_FALSE(ParseEntryMeta(R"({"url":"https://x","file":"a/b.png"})", out));
        CHECK_FALSE(ParseEntryMeta(R"({"url":"https://x","file":"CON"})", out));
        CHECK_FALSE(ParseEntryMeta(R"({"url":7,"file":"a.png"})", out));
        REQUIRE(ParseEntryMeta(R"({"url":"https://x","file":"a.png"})", out));
        CHECK(out.Mime.empty()); // optional: the caller falls back to application/octet-stream
    }

    TEST_CASE("metadata that is not valid UTF-8 is never stored altered") {
        EntryMeta meta;
        meta.Url = std::string("https://x/\xFF\xFE", 12);
        meta.FileName = "a.png";
        CHECK(SerializeEntryMeta(meta).empty());
    }
}
