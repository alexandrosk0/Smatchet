// Logger redaction (SECURITY_AUDIT_2026-06-13 LOW + pre-release hardening). The
// async file sink previously wrote LogEntry::message verbatim; a LOG_* line
// carrying a secret/PII (e.g. Trace HTTP-body logging) landed unredacted in the
// on-disk log. Redaction now happens once at ingestion (Logger::Log routes every
// message through privacy::RedactLogLine), so BOTH sinks — the persisted file
// and the in-memory ring buffer behind the in-app log viewer — are scrubbed.
//
// Pillar 3 (never crash): drives the real Logger singleton + worker thread; the
// flush is synchronous (FlushFileSink) so the read-back is deterministic.

#include "Logger.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = ghc::filesystem;

namespace {

std::string ReadWhole(const fs::path& p) {
    std::ifstream in(p.string().c_str(), std::ios::in | std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path UniqueSinkPath() {
    const auto unique =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return fs::temp_directory_path() / ("smatchet_logsink_" + std::to_string(unique) + ".log");
}

} // namespace

TEST_CASE("Logger file sink — redacts secret/long-token + strips CR/LF/ANSI on the persisted line") {
    Logger& log = Logger::Instance();
    const LogLevel savedLevel = log.GetMinLevel();
    log.SetMinLevel(LogLevel::Trace);

    const fs::path sink = UniqueSinkPath();
    log.SetFileSinkPath(sink.string());

    const std::string secret = "AbCdEf0123456789AbCdEf0123456789AbCdEf0123456789"; // 48-char opaque token
    log.Log(LogLevel::Info, "outbound token=" + secret);
    log.Log(LogLevel::Info, std::string("server said hi\r\n2099 FAKE forged\x1b[31m line"));
    log.FlushFileSink();

    const std::string contents = ReadWhole(sink);

    // The raw secret never reaches the file; the redaction marker does.
    CHECK(contents.find(secret) == std::string::npos);
    CHECK(contents.find("<redacted>") != std::string::npos);

    // No CR / bare ESC survives — a forged second log line cannot be injected.
    CHECK(contents.find('\r') == std::string::npos);
    CHECK(contents.find('\x1b') == std::string::npos);
    // The benign prefix is still present (neutralized, not wholesale-dropped).
    CHECK(contents.find("server said hi") != std::string::npos);

    // The in-memory ring buffer (in-app log viewer, copied into bug reports and
    // screenshots) is scrubbed too — redaction happens at ingestion, not per sink.
    bool ringHoldsRawSecret = false;
    bool ringHoldsRedactionMarker = false;
    for (const LogEntry& e : log.GetEntriesSnapshot()) {
        if (e.message.find(secret) != std::string::npos) {
            ringHoldsRawSecret = true;
        }
        if (e.message.find("<redacted>") != std::string::npos) {
            ringHoldsRedactionMarker = true;
        }
    }
    CHECK_FALSE(ringHoldsRawSecret);
    CHECK(ringHoldsRedactionMarker);

    // Stop the sink + restore shared singleton state for other test cases.
    log.SetFileSinkPath("");
    log.SetMinLevel(savedLevel);

    std::error_code ec;
    fs::remove(sink, ec);
}
