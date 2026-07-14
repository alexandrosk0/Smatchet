// BackendAuditTrail tests — exercise the pure-logic surface (Redact*, MakeFieldDiffUnknownBefore,
// MakeOperationId) and the file-IO append + read path against a per-test temp directory.
//
// We do NOT exercise the async writer's full lifecycle (the static AuditWriter::thread persists
// for the entire process); we rely on a short sleep + retry probe of the audit file to verify
// that AppendEvent actually flushes. Because the writer is a process-wide singleton, tests must
// not run in parallel (doctest is single-threaded by default — fine).

#include "BackendAuditTrail.h"
#include "ConfigManager.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace BackendAuditTrail;

namespace {

bool MakeDir(const std::string& path) {
#if defined(_WIN32)
    return ::_mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

void RemoveFileIfExists(const std::string& path) { std::remove(path.c_str()); }

/// Per-test scoped temp directory. Sets ConfigManager's user-data dir to this path so the
/// audit writer routes there. Removes the audit files on destruction. **Process-wide** — only
/// one fixture should be live at a time (doctest's single-threaded runner enforces this).
class AuditDirGuard {
  public:
    AuditDirGuard() {
        // Build a unique directory under the platform temp area.
        const char* envTmp = nullptr;
#if defined(_WIN32)
        envTmp = std::getenv("TEMP");
        if (!envTmp)
            envTmp = std::getenv("TMP");
        if (!envTmp)
            envTmp = "C:\\Windows\\Temp";
        const char sep = '\\';
#else
        envTmp = std::getenv("TMPDIR");
        if (!envTmp)
            envTmp = "/tmp";
        const char sep = '/';
#endif
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        dir_ = std::string(envTmp) + sep + "smatchet_audit_test_" + std::to_string(unique);
        MakeDir(dir_);

        // ConfigManager normalises trailing separator — pass with trailing slash to match.
        std::string dirWithSep = dir_;
        if (dirWithSep.back() != sep)
            dirWithSep.push_back(sep);
        ConfigManager::SetUserDataDirectory(dirWithSep);
    }

    ~AuditDirGuard() {
        // Best-effort cleanup. The async writer may still be flushing; remove what we can.
        RemoveFileIfExists(GetAuditFilePath());
        // Reset user-data dir to empty so the next fixture starts clean.
        ConfigManager::SetUserDataDirectory("");
        std::remove(dir_.c_str()); // rmdir for empty dir; non-fatal if non-empty
    }

    AuditDirGuard(const AuditDirGuard&) = delete;
    AuditDirGuard& operator=(const AuditDirGuard&) = delete;
    const std::string& Dir() const { return dir_; }

  private:
    std::string dir_;
};

/// Wait for the async audit writer to flush at least `expected_min` lines that contain
/// `operationId` to the audit file at `path`. Operation-id filtering scopes the wait to events
/// the current test produced — useful because the AuditWriter is a process-wide singleton and
/// the audit file may accumulate lines from earlier tests in the same run.
/// Returns the number of matching lines observed (after the count is met, or timeout).
std::size_t WaitForOperationLinesAt(const std::string& path, const std::string& operationId, std::size_t expected_min,
                                    int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t matches = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            matches = 0;
            std::string l;
            while (std::getline(f, l)) {
                if (l.find(operationId) != std::string::npos)
                    ++matches;
            }
            if (matches >= expected_min)
                return matches;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return matches;
}

std::size_t WaitForOperationLines(const std::string& operationId, std::size_t expected_min, int timeout_ms = 2000) {
    return WaitForOperationLinesAt(GetAuditFilePath(), operationId, expected_min, timeout_ms);
}

} // namespace

TEST_CASE("BackendAuditTrail: MakeOperationId generates unique monotonic ids") {
    const std::string a = MakeOperationId("create");
    const std::string b = MakeOperationId("create");
    CHECK_FALSE(a.empty());
    CHECK_FALSE(b.empty());
    CHECK(a != b);
    CHECK(a.find("create-") == 0);
    CHECK(b.find("create-") == 0);
}

TEST_CASE("BackendAuditTrail: MakeOperationId falls back to 'audit' for empty prefix") {
    const std::string id = MakeOperationId("");
    CHECK(id.find("audit-") == 0);
}

TEST_CASE("BackendAuditTrail: RedactText redacts sensitive keys") {
    CHECK(RedactText("password", "hunter2") == "[redacted]");
    CHECK(RedactText("api_token", "abc123") == "[redacted]");
    CHECK(RedactText("apikey", "abc") == "[redacted]");
    CHECK(RedactText("Authorization", "Bearer xyz") == "[redacted]");
    CHECK(RedactText("summary", "Public summary") == "[redacted]"); // 'summary' is on the sensitive list
    CHECK(RedactText("description", "long text") == "[redacted]");
}

TEST_CASE("BackendAuditTrail: RedactText passes non-sensitive keys untouched (truncated if very long)") {
    CHECK(RedactText("project", "PROJ") == "PROJ");
    CHECK(RedactText("issuetype", "Story") == "Story");
    CHECK(RedactText("status", "Open") == "Open");

    // Truncation kicks in at >1000 chars
    std::string longVal(1500, 'a');
    const std::string out = RedactText("project", longVal);
    CHECK(out.size() < longVal.size());
    CHECK(out.find("[truncated]") != std::string::npos);
}

TEST_CASE("BackendAuditTrail: RedactJson redacts sensitive object keys recursively") {
    nlohmann::json j = {
        {"project", "PROJ"},
        {"summary", "secret"},
        {"meta", {{"token", "abc"}, {"safe", "yes"}}},
    };
    const auto r = RedactJson(j);
    CHECK(r["project"].get<std::string>() == "PROJ");
    CHECK(r["summary"].get<std::string>() == "[redacted]");
    CHECK(r["meta"]["token"].get<std::string>() == "[redacted]");
    CHECK(r["meta"]["safe"].get<std::string>() == "yes");
}

TEST_CASE("BackendAuditTrail: RedactJson recognises field-diff objects and redacts before/after by field id") {
    nlohmann::json diff = nlohmann::json::array();
    diff.push_back({{"field_id", "summary"}, {"before", "old"}, {"after", "new"}});
    diff.push_back({{"field_id", "priority"}, {"before", "Low"}, {"after", "High"}});

    const auto r = RedactJson(diff);
    REQUIRE(r.is_array());
    REQUIRE(r.size() == 2);
    // 'summary' is on the sensitive key list → both before+after redacted.
    CHECK(r[0]["before"].get<std::string>() == "[redacted]");
    CHECK(r[0]["after"].get<std::string>() == "[redacted]");
    CHECK(r[0]["field_id"].get<std::string>() == "summary");

    // 'priority' is not sensitive → before+after pass through.
    CHECK(r[1]["before"].get<std::string>() == "Low");
    CHECK(r[1]["after"].get<std::string>() == "High");
}

TEST_CASE("BackendAuditTrail: MakeFieldDiffUnknownBefore builds an array of {field_id, before='unknown', after}") {
    nlohmann::json fields = {{"summary", "S"}, {"priority", "High"}};
    const auto diff = MakeFieldDiffUnknownBefore(fields);
    REQUIRE(diff.is_array());
    REQUIRE(diff.size() == 2);
    for (const auto& e : diff) {
        CHECK(e.contains("field_id"));
        CHECK(e["before"].get<std::string>() == "unknown");
        CHECK(e.contains("after"));
        CHECK(e["before_reason"].get<std::string>() == "previous value not available at mutation boundary");
    }
}

TEST_CASE("BackendAuditTrail: MakeFieldDiffUnknownBefore returns empty array for non-object input") {
    CHECK(MakeFieldDiffUnknownBefore(nlohmann::json("string")).empty());
    CHECK(MakeFieldDiffUnknownBefore(nlohmann::json::array()).empty());
    CHECK(MakeFieldDiffUnknownBefore(nlohmann::json::object()).empty());
}

// The async writer thread re-resolves GetAuditFilePath() per event, so tests that swap the
// user-data dir between AppendEvent calls (or between TEST_CASEs) route subsequent lines to
// the new path. The cross-TEST_CASE behaviour is exercised by the runtime-dir-change case
// below; this case keeps a single AuditDirGuard so the two in-fixture scenarios share one
// directory (cheaper than two guards per case).
TEST_CASE("BackendAuditTrail: file IO surface (begin/result + failure error string)") {
    AuditDirGuard guard;

    // Scenario 1: AppendBegin + AppendResult round-trip via ReadRecentEvents.
    {
        const std::string opId = MakeOperationId("phase3-begin-result");
        AppendBegin("create_issue", "test", "ABC-1", opId,
                    nlohmann::json{{"draft_summary", "[redacted-in-real-life]"}});
        AppendResult("create_issue", "test", "ABC-1", opId, true, "", nlohmann::json::object());

        REQUIRE(WaitForOperationLines(opId, 2) >= 2);

        const auto readResult = ReadRecentEvents(500);
        CHECK(readResult.has_value());
        const std::vector<nlohmann::json> events = readResult.value_or({});

        bool sawBegin = false;
        bool sawResult = false;
        for (const auto& e : events) {
            if (e.value("operation_id", std::string()) != opId)
                continue;
            CHECK(e.contains("timestamp_ms"));
            CHECK(e["action"].get<std::string>() == "create_issue");
            if (e["phase"].get<std::string>() == "begin")
                sawBegin = true;
            if (e["phase"].get<std::string>() == "result")
                sawResult = true;
        }
        CHECK(sawBegin);
        CHECK(sawResult);
    }

    // Scenario 2: AppendResult on failure carries the (un-redacted) error string.
    {
        const std::string opId = MakeOperationId("phase3-failure");
        AppendResult("update_issue", "test", "ABC-1", opId, false, "Backend error: 500", nlohmann::json::object());

        REQUIRE(WaitForOperationLines(opId, 1) >= 1);

        const std::vector<nlohmann::json> events = ReadRecentEvents(500).value_or({});
        const auto it = std::find_if(events.begin(), events.end(), [&](const nlohmann::json& e) {
            return e.value("operation_id", std::string()) == opId;
        });
        REQUIRE(it != events.end());
        CHECK((*it)["success"].get<bool>() == false);
        CHECK((*it)["error"].get<std::string>() == "Backend error: 500");
    }
}

// Runtime user-data-dir change: writer re-resolves GetAuditFilePath() per event, so events
// emitted after a SetUserDataDirectory swap land in the new directory's audit file, not the
// previous one's. Prior to the per-event re-resolve fix, the writer cached the path on first
// event and silently kept appending to the original location.
TEST_CASE("BackendAuditTrail: writer follows ConfigManager user-data dir change at runtime") {
    AuditDirGuard guardA;
    const std::string pathA = GetAuditFilePath();

    const std::string opIdA = MakeOperationId("rt-dir-A");
    AppendBegin("create_issue", "test", "ABC-1", opIdA, nlohmann::json::object());
    REQUIRE(WaitForOperationLinesAt(pathA, opIdA, 1) >= 1);

    AuditDirGuard guardB;
    const std::string pathB = GetAuditFilePath();
    REQUIRE(pathA != pathB);

    const std::string opIdB = MakeOperationId("rt-dir-B");
    AppendBegin("create_issue", "test", "DEF-2", opIdB, nlohmann::json::object());
    REQUIRE(WaitForOperationLinesAt(pathB, opIdB, 1) >= 1);

    CHECK(WaitForOperationLinesAt(pathA, opIdB, 1, 200) == 0);
    CHECK(WaitForOperationLinesAt(pathB, opIdA, 1, 200) == 0);
}
