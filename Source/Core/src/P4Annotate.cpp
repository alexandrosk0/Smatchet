#include "P4Annotate.h"
#include "Logger.h"
#include "UiThreadAffinity.h"
#include "P4AnnotateParse.h"
#include "P4ErrorUtil.h"
#include "StringUtil.h"
#include "SubprocessCapture.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <regex>

namespace {

constexpr size_t kP4LogMaxStderr = 2048;
constexpr size_t kP4LogMaxStdoutTrace = 8192;
constexpr int kP4ProcessTimeoutMs = 120000;
constexpr size_t kP4CaptureBytesMax = 4u * 1024u * 1024u;

} // namespace

using P4AnnotateParse::ParseAnnotateTextLine;
using P4AnnotateParse::ParseLatestChangeFromChangesOutput;
using P4AnnotateParse::SplitLines;
using P4AnnotateParse::StripP4UserDomain;

bool P4RunCommand(const AnnotateAnalysisConfig& cfg, const std::vector<std::string>& args, int& outExitCode,
                  std::string& outStdout, std::string& outStderr) {
    outExitCode = -1;
    outStdout.clear();
    outStderr.clear();
    // Runner-seam override (slice 3 of autonomous-debugging-no-creds). When a test
    // installs cfg.P4RunOverride, it short-circuits the real spawn path — the lambda
    // returns canned exit code + stdout + stderr from a fixture. Production behaviour
    // preserved when the override is empty (fall through to SubprocessCapture::Run).
    if (cfg.P4RunOverride) {
        const bool ok = cfg.P4RunOverride(args, outExitCode, outStdout, outStderr);
        LOG_DEBUG("P4: ran via P4RunOverride args=%s exit=%d ok=%d", JoinStrings(args, " ").c_str(), outExitCode,
                  static_cast<int>(ok));
        return ok;
    }
    // Pillar-2 gate (close-gate-gaps Slice 1a): the real p4 subprocess spawn is a blocking
    // server round-trip — must not run on the UI render thread (#761). Placed after the
    // P4RunOverride short-circuit so the test seam (non-blocking) never trips it. Warn-only.
    UiThreadAffinity::WarnIfOnUiThread("P4RunCommand (p4 subprocess spawn)");
    const std::string exe = cfg.P4Executable.empty() ? "p4" : cfg.P4Executable;
    LOG_INFO("P4: spawn exe=\"%s\" args: %s", exe.c_str(), JoinStrings(args, " ").c_str());

    SubprocessCapture::CaptureOptions opts;
    opts.argv0 = exe;
    opts.args = args;
    opts.timeoutMs = kP4ProcessTimeoutMs;
    // Don't hand p4 the parent's secret-bearing env vars (GH_TOKEN, API keys,
    // tracker PATs, etc.) — p4 needs none of them (audit #15). P4PORT / P4USER
    // / P4CLIENT / P4CONFIG, PATH, locale and HOME all survive the scrub, so the
    // p4 client keeps resolving its connection and ticket exactly as before.
    opts.scrubSensitiveEnv = true;
    // Preserve P4Annotate's historical per-stream cap (4 MB) — matches
    // pre-lift kP4CaptureBytesMax behaviour for both stdout and stderr.
    opts.stdoutByteCap = kP4CaptureBytesMax;
    opts.stderrByteCap = kP4CaptureBytesMax;
    SubprocessCapture::CaptureResult cap;
    std::string spawnError;
    const bool ran = SubprocessCapture::Run(opts, cap, spawnError);
    if (!ran) {
        LOG_ERROR("P4: spawn failed after %lld ms: %s", static_cast<long long>(cap.durationMs), spawnError.c_str());
        return false;
    }
    outExitCode = cap.exitCode;
    outStdout = std::move(cap.stdoutText);
    outStderr = std::move(cap.stderrText);
    if (cap.timedOut && outStderr.empty()) {
        outStderr = "p4 process timed out";
    }
    if (cap.stdoutCapped || cap.stderrCapped) {
        LOG_WARN("P4: output capture capped at %zu bytes per stream", kP4CaptureBytesMax);
    }
    LOG_INFO("P4: completed exit=%d duration=%lld ms", outExitCode, static_cast<long long>(cap.durationMs));
    if (outExitCode != 0 && !outStderr.empty()) {
        LOG_WARN("P4: stderr: %s", TruncateForLog(outStderr, kP4LogMaxStderr).c_str());
    } else if (outExitCode != 0) {
        LOG_WARN("P4: non-zero exit=%d with empty stderr", outExitCode);
    }
    if (Logger::Instance().GetLogP4Io() && Logger::Instance().ShouldLog(LogLevel::Trace) && !outStdout.empty()) {
        LOG_TRACE("P4: stdout: %s", TruncateForLog(outStdout, kP4LogMaxStdoutTrace).c_str());
    }
    return true;
}

P4LineAnnotate P4AnnotateLine(const AnnotateAnalysisConfig& cfg, const std::string& depotOrPath, int oneBasedLine,
                              const std::string& atChangelist) {
    P4LineAnnotate result;
    if (depotOrPath.empty() || oneBasedLine <= 0) {
        result.Error = "invalid path or line";
        LOG_WARN("P4AnnotateLine: invalid input (empty path or line<=0) line=%d", oneBasedLine);
        return result;
    }

    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos && pathArg.find('#') == std::string::npos) {
        pathArg += "@" + atChangelist;
    }
    LOG_DEBUG("P4AnnotateLine: pathArg=%s line=%d atCl=%s", pathArg.c_str(), oneBasedLine, atChangelist.c_str());
    const std::vector<std::string> args = {"annotate", "-u", "-c", "-q", pathArg};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        result.Error = "failed to run p4";
        LOG_WARN("P4AnnotateLine: failed to spawn p4 pathArg=%s line=%d", pathArg.c_str(), oneBasedLine);
        return result;
    }
    if (code != 0) {
        result.Error = FormatP4CommandError("p4 annotate failed", code, err);
        LOG_DEBUG("P4AnnotateLine: annotate non-zero exit=%d, trying changes fallback pathArg=%s err=%s", code,
                  pathArg.c_str(), TruncateForLog(result.Error, 512).c_str());
        // Fallback: latest change on file
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            LOG_DEBUG("P4AnnotateLine: changes fallback success pathArg=%s", pathArg.c_str());
            return ParseLatestChangeFromChangesOutput(o2, e2);
        }
        LOG_WARN("P4AnnotateLine: annotate failed and changes fallback failed pathArg=%s line=%d", pathArg.c_str(),
                 oneBasedLine);
        return result;
    }

    const std::vector<std::string> lines = SplitLines(out);
    if (oneBasedLine <= 0 || static_cast<size_t>(oneBasedLine) > lines.size()) {
        result.Approximate = true;
        LOG_DEBUG("P4AnnotateLine: line out of annotate range (line=%zu annotateLines=%zu) pathArg=%s",
                  static_cast<size_t>(oneBasedLine), lines.size(), pathArg.c_str());
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            return ParseLatestChangeFromChangesOutput(o2, e2);
        }
        result.Error = "line out of range";
        LOG_WARN("P4AnnotateLine: %s pathArg=%s", result.Error.c_str(), pathArg.c_str());
        return result;
    }

    const std::string& L = lines[static_cast<size_t>(oneBasedLine - 1)];
    std::string cl;
    std::string user;
    std::string annDate;
    std::string codeLine;
    if (!ParseAnnotateTextLine(L, cl, user, annDate, codeLine)) {
        result.Approximate = true;
        LOG_DEBUG("P4AnnotateLine: unrecognized annotate line, trying changes pathArg=%s raw=%s", pathArg.c_str(),
                  TruncateForLog(L, 200).c_str());
        std::vector<std::string> chArgs = {"changes", "-m", "1", pathArg};
        int c2 = 0;
        std::string o2;
        std::string e2;
        if (P4RunCommand(cfg, chArgs, c2, o2, e2) && c2 == 0) {
            P4LineAnnotate fb = ParseLatestChangeFromChangesOutput(o2, e2);
            return fb;
        }
        result.Error = "unrecognized annotate line";
        LOG_WARN("P4AnnotateLine: %s pathArg=%s", result.Error.c_str(), pathArg.c_str());
        return result;
    }

    result.Changelist = cl;
    result.User = user;
    if (!annDate.empty()) {
        result.Date = annDate;
    }
    constexpr size_t kMaxSnippet = 400;
    if (codeLine.size() > kMaxSnippet) {
        result.LineSnippet = codeLine.substr(0, kMaxSnippet);
    } else {
        result.LineSnippet = codeLine;
    }
    LOG_DEBUG("P4AnnotateLine: success cl=%s user=%s pathArg=%s", cl.c_str(), user.c_str(), pathArg.c_str());
    return result;
}

std::vector<P4AnnotatedLine> P4AnnotateFile(const AnnotateAnalysisConfig& cfg, const std::string& depotOrPath,
                                            const std::string& atChangelist, std::string& outError) {
    std::vector<P4AnnotatedLine> rows;
    outError.clear();
    if (depotOrPath.empty()) {
        outError = "empty path";
        LOG_WARN("P4AnnotateFile: empty depot path");
        return rows;
    }
    std::string pathArg = depotOrPath;
    if (!atChangelist.empty() && pathArg.find('@') == std::string::npos && pathArg.find('#') == std::string::npos) {
        pathArg += "@" + atChangelist;
    }
    LOG_DEBUG("P4AnnotateFile: pathArg=%s atCl=%s", pathArg.c_str(), atChangelist.c_str());
    const std::vector<std::string> args = {"annotate", "-u", "-c", "-q", pathArg};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        outError = "failed to run p4";
        LOG_WARN("P4AnnotateFile: failed to spawn p4 pathArg=%s", pathArg.c_str());
        return rows;
    }
    if (code != 0) {
        outError = FormatP4CommandError("p4 annotate failed", code, err);
        LOG_WARN("P4AnnotateFile: annotate failed exit=%d pathArg=%s err=%s", code, pathArg.c_str(),
                 TruncateForLog(outError, kP4LogMaxStderr).c_str());
        return rows;
    }

    const std::vector<std::string> lines = SplitLines(out);
    LOG_DEBUG("P4AnnotateFile: parsed %zu lines pathArg=%s", lines.size(), pathArg.c_str());
    for (size_t i = 0; i < lines.size(); ++i) {
        P4AnnotatedLine row;
        row.SourceLine = static_cast<int>(i + 1);
        std::string cl;
        std::string user;
        std::string annDate;
        std::string codeLine;
        if (ParseAnnotateTextLine(lines[i], cl, user, annDate, codeLine)) {
            row.Changelist = cl;
            row.User = user;
            row.Date = annDate;
            row.Code = codeLine;
        } else {
            row.Code = lines[i];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/** Advance (y,m,d) by one day in UTC (exclusive end date for Perforce `//...@start,end` ranges). */
static bool SmatchetAddOneUtcCalendarDay(int y, int m, int d, int& oy, int& om, int& od) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = 0;
#if defined(_WIN32)
    const time_t sec0 = _mkgmtime(&t);
#else
    const time_t sec0 = timegm(&t);
#endif
    if (sec0 == static_cast<time_t>(-1)) {
        return false;
    }
    const time_t sec1 = sec0 + 86400;
    std::tm out{};
#if defined(_WIN32)
    if (gmtime_s(&out, &sec1) != 0) {
        return false;
    }
#else
    if (gmtime_r(&sec1, &out) == nullptr) {
        return false;
    }
#endif
    oy = out.tm_year + 1900;
    om = out.tm_mon + 1;
    od = out.tm_mday;
    return true;
}

Result<std::string> P4FirstSubmittedChangelistOnCalendarDay(const AnnotateAnalysisConfig& cfg, int year, int month,
                                                            int day) {
    using R = Result<std::string>;
    if (year < 1970 || year > 3000 || month < 1 || month > 12 || day < 1 || day > 31) {
        return R::Err("invalid calendar date");
    }
    int ey = 0;
    int em = 0;
    int ed = 0;
    if (!SmatchetAddOneUtcCalendarDay(year, month, day, ey, em, ed)) {
        return R::Err("date range arithmetic failed");
    }
    char start[32];
    char endEx[32];
    std::snprintf(start, sizeof(start), "%04d/%02d/%02d", year, month, day);
    std::snprintf(endEx, sizeof(endEx), "%04d/%02d/%02d", ey, em, ed);
    const std::string filespec = std::string("//...@") + start + "," + endEx;
    const std::vector<std::string> args = {"changes", "-r", "-m", "1", "-s", "submitted", filespec};

    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        return R::Err("failed to run p4");
    }
    if (code != 0) {
        return R::Err(FormatP4CommandError("p4 changes failed", code, err));
    }
    if (out.find_first_not_of(" \t\r\n") == std::string::npos) {
        return R::Err("no submitted changelists on that calendar day (or no visibility to //...@)");
    }
    const P4LineAnnotate parsed = ParseLatestChangeFromChangesOutput(out, err);
    if (parsed.Changelist.empty()) {
        return R::Err(parsed.Error.empty() ? "could not parse p4 changes output" : parsed.Error);
    }
    return R::Ok(parsed.Changelist);
}

Result<std::vector<P4ChangeSummary>> P4ChangesForUser(const AnnotateAnalysisConfig& cfg, const std::string& p4User,
                                                      int maxN) {
    using R = Result<std::vector<P4ChangeSummary>>;
    if (p4User.empty()) {
        return R::Err("empty p4 user");
    }
    const int capped = maxN > 0 ? maxN : 1;
    const std::vector<std::string> args = {"changes", "-u", p4User, "-m", std::to_string(capped), "-s", "submitted"};
    int code = 0;
    std::string out;
    std::string err;
    if (!P4RunCommand(cfg, args, code, out, err)) {
        return R::Err("failed to run p4");
    }
    if (code != 0) {
        return R::Err(FormatP4CommandError("p4 changes failed", code, err));
    }
    std::vector<P4ChangeSummary> changes = P4AnnotateParse::ParseChangesForUserOutput(out);
    LOG_DEBUG("P4ChangesForUser: user=%s max=%d parsed=%zu", p4User.c_str(), capped, changes.size());
    return R::Ok(std::move(changes));
}

std::string P4UserForEmail(const AnnotateAnalysisConfig& cfg, const std::string& email) {
    const std::string wanted = TrimCopy(email);
    if (wanted.empty()) {
        return std::string();
    }
    int code = 0;
    std::string out;
    std::string err;
    // `p4 users` prints "login <email> (FullName) accessed YYYY/MM/DD" per line.
    if (!P4RunCommand(cfg, {"users"}, code, out, err) || code != 0) {
        LOG_DEBUG("P4UserForEmail: p4 users failed code=%d (%s)", code, err.c_str());
        return std::string();
    }
    const std::string login = P4AnnotateParse::ParseP4UserLoginForEmail(out, wanted);
    if (login.empty()) {
        LOG_DEBUG("P4UserForEmail: no p4 user matched email '%s'", wanted.c_str());
    } else {
        LOG_DEBUG("P4UserForEmail: matched email '%s' -> login '%s'", wanted.c_str(), login.c_str());
    }
    return login;
}

P4ChangelistDescribeCache::P4ChangelistDescribeCache(int maxEntries) : maxEntries_(maxEntries > 0 ? maxEntries : 16) {}

P4ChangelistDetails P4ChangelistDescribeCache::Get(const std::string& changelist) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(changelist);
    if (it == map_.end()) {
        return P4ChangelistDetails();
    }
    return it->second;
}

void P4ChangelistDescribeCache::Touch(const std::string& cl) {
    auto it = std::find(lru_.begin(), lru_.end(), cl);
    if (it != lru_.end()) {
        lru_.erase(it);
    }
    lru_.push_back(cl);
}

void P4ChangelistDescribeCache::EvictIfNeeded() {
    while (static_cast<int>(map_.size()) > maxEntries_ && !lru_.empty()) {
        const std::string victim = lru_.front();
        lru_.erase(lru_.begin());
        map_.erase(victim);
    }
}

void P4ChangelistDescribeCache::Store(const std::string& changelist, P4ChangelistDetails d) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[changelist] = std::move(d);
    Touch(changelist);
    EvictIfNeeded();
}

P4ChangelistDetails P4ChangelistDescribeCache::GetOrFetch(const AnnotateAnalysisConfig& cfg,
                                                          const std::string& changelist) {
    if (changelist.empty()) {
        return P4ChangelistDetails();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = map_.find(changelist);
        if (it != map_.end() && it->second.Loaded) {
            Touch(changelist);
            LOG_DEBUG("P4ChangelistDescribeCache: hit cl=%s", changelist.c_str());
            return it->second;
        }
    }

    LOG_DEBUG("P4ChangelistDescribeCache: miss cl=%s fetching", changelist.c_str());
    std::vector<std::string> args = {"describe", "-s", changelist};
    int code = 0;
    std::string out;
    std::string err;
    P4ChangelistDetails d;
    d.Loaded = true;
    if (!P4RunCommand(cfg, args, code, out, err) || code != 0) {
        d.Error = FormatP4CommandError("p4 describe failed", code, err);
        LOG_WARN("P4ChangelistDescribeCache: describe failed cl=%s err=%s", changelist.c_str(),
                 TruncateForLog(d.Error, kP4LogMaxStderr).c_str());
        Store(changelist, d);
        return d;
    }

    // Common: "Change N by user@client on 2026/04/09 ..."  OR  "Change N on date by user ..."
    std::smatch m;
    static const std::regex headByOn(R"(Change\s+\d+\s+by\s+(\S+)\s+on\s+([^\r\n]+))");
    static const std::regex headOnBy(R"(Change\s+\d+\s+on\s+(\S+)\s+by\s+(\S+))");
    if (std::regex_search(out, m, headByOn)) {
        std::string who = m[1].str();
        StripP4UserDomain(who);
        d.Author = who;
        d.Date = TrimCopy(m[2].str());
    } else if (std::regex_search(out, m, headOnBy)) {
        d.Date = m[1].str();
        std::string who = m[2].str();
        StripP4UserDomain(who);
        d.Author = who;
    }
    size_t descStart = out.find('\t');
    if (descStart == std::string::npos) {
        descStart = out.find('\n');
    }
    if (descStart != std::string::npos) {
        d.Description = TrimCopy(out.substr(descStart));
        if (d.Description.size() > 2000) {
            d.Description.resize(2000);
            d.Description += "...";
        }
    }
    if (d.Author.empty() && d.Date.empty()) {
        LOG_DEBUG("P4ChangelistDescribeCache: header parse miss cl=%s stdout=%s", changelist.c_str(),
                  TruncateForLog(out, 300).c_str());
    }
    Store(changelist, d);
    return d;
}
