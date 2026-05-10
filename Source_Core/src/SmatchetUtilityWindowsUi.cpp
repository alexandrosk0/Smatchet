#include "SmatchetUI.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <limits>
#include <string>

namespace {

void DrawLogWindowPreferences(UiDrawSession& d) {
    static const LogLevel kLogLevels[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                                          LogLevel::Error};
    LogLevel parsedLevel = Logger::ParseLogLevelString(d.cfg.LogMinLevel, LogLevel::Info);
    int levelComboIndex = 2;
    for (int i = 0; i < 5; ++i) {
        if (kLogLevels[i] == parsedLevel) {
            levelComboIndex = i;
            break;
        }
    }
    ImGui::TextUnformatted("Min log level");
    ImGui::SameLine();
    if (ImGui::Combo("##LogWinMinLevel", &levelComboIndex,
                     "Trace\0"
                     "Debug\0"
                     "Info\0"
                     "Warn\0"
                     "Error\0"
                     "\0")) {
        d.cfg.LogMinLevel = Logger::LogLevelToString(kLogLevels[levelComboIndex]);
        Logger::Instance().SetMinLevel(kLogLevels[levelComboIndex]);
        ConfigManager::Save(d.cfg);
    }
    bool trackerBodies = d.cfg.LogTrackerHttpBodies;
    if (ImGui::Checkbox("Log Tracker HTTP bodies (truncated)", &trackerBodies)) {
        d.cfg.LogTrackerHttpBodies = trackerBodies;
        Logger::Instance().SetLogTrackerHttpBodies(trackerBodies);
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Verbose: logs response text (capped per request). May include issue summaries and user-visible data.");
    }
    bool logP4Io = d.cfg.LogP4Io;
    if (ImGui::Checkbox("Log Perforce p4 stdout (truncated, Trace level)", &logP4Io)) {
        d.cfg.LogP4Io = logP4Io;
        Logger::Instance().SetLogP4Io(logP4Io);
        ConfigManager::Save(d.cfg);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Requires min level Trace. Logs capped p4 stdout per command; stderr is logged on non-zero exit.");
    }
}

void AppendLogEntryAsLines(std::vector<std::string>& out, const char* levelTag8, const std::string& message) {
    constexpr const char* kPad = "        ";
    size_t start = 0;
    bool first = true;
    for (;;) {
        const size_t nl = message.find('\n', start);
        const std::string part =
            (nl == std::string::npos) ? message.substr(start) : message.substr(start, nl - start);
        out.push_back(std::string(first ? levelTag8 : kPad) + part);
        first = false;
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
}

void FillLogViewLinesFromEntries(const std::vector<LogEntry>& entries, std::vector<std::string>& out) {
    out.clear();
    out.reserve(entries.size() * 2);
    for (const auto& e : entries) {
        const char* levelLabel;
        switch (e.level) {
        case LogLevel::Trace:
            levelLabel = "[TRACE] ";
            break;
        case LogLevel::Debug:
            levelLabel = "[DEBUG] ";
            break;
        case LogLevel::Info:
            levelLabel = "[INFO ] ";
            break;
        case LogLevel::Warn:
            levelLabel = "[WARN ] ";
            break;
        case LogLevel::Error:
            levelLabel = "[ERROR] ";
            break;
        default:
            levelLabel = "";
            break;
        }
        AppendLogEntryAsLines(out, levelLabel, e.message);
    }
}

} // namespace

void SmatchetUI::drawLogWindow(UiDrawSession& d) {
    Logger& logger = Logger::Instance();
    prepareTopLevelWindow(d, "log", 900.0f, 320.0f);
    if (!ImGui::Begin("Log", &d.showLogWindow)) {
        ImGui::End();
        return;
    }
    repairTopLevelWindow(d, "log", 360.0f, 220.0f);

    DrawLogWindowPreferences(d);
    ImGui::Separator();

    if (ImGui::Button("Clear Log")) {
        logger.Clear();
        d.lastSeenLogRevision = (std::numeric_limits<std::uint64_t>::max)();
        d.logViewLines.clear();
        d.logScrollToTailPending = false;
        d.logTailReleasedByUser = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::Label("log.copy_log", "Copy log", "LogWinCopy"))) {
        std::vector<std::string> clipLines;
        FillLogViewLinesFromEntries(logger.GetEntriesSnapshot(), clipLines);
        std::string clip;
        size_t n = 0;
        for (const auto& s : clipLines) {
            n += s.size() + 1;
        }
        if (!clipLines.empty() && n > 0) {
            --n;
        }
        clip.reserve(n);
        for (size_t i = 0; i < clipLines.size(); ++i) {
            if (i > 0) {
                clip.push_back('\n');
            }
            clip.append(clipLines[i]);
        }
        ImGui::SetClipboardText(clip.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          SmatchetLocalization::T("log.copy_log_tip",
                                                  "Copy the full application log to the clipboard."));
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-scroll", &d.logAutoScroll) && d.logAutoScroll) {
        d.logTailReleasedByUser = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(application log)");

    ImGui::Separator();

    const std::uint64_t revision = logger.GetRevision();
    const bool rebuildLog = d.logViewLines.empty() || revision != d.lastSeenLogRevision;
    if (rebuildLog) {
        FillLogViewLinesFromEntries(logger.GetEntriesSnapshot(), d.logViewLines);

        d.lastSeenLogRevision = revision;

        if (d.logAutoScroll && !d.logTailReleasedByUser) {
            d.logScrollToTailPending = true;
            d.logScrollTailGiveUpFrame = static_cast<std::uint64_t>(ImGui::GetFrameCount()) + 96;
        }
    }

    if (!d.logAutoScroll) {
        d.logScrollToTailPending = false;
    }

    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    // Default ImGui wrapping makes row height != clipper item height → broken clipper and “empty” bands.
    ImGui::PushTextWrapPos(FLT_MAX);

    const int lineCount = static_cast<int>(d.logViewLines.size());
    constexpr int kMaxLogLinesDrawn = 8000;
    const int skipLines = (lineCount > kMaxLogLinesDrawn) ? (lineCount - kMaxLogLinesDrawn) : 0;
    if (skipLines > 0) {
        ImGui::TextDisabled("(… %d older lines omitted …)", skipLines);
    }
    if (lineCount <= 0) {
        ImGui::TextDisabled("(empty)");
    } else {
        for (int row = skipLines; row < lineCount; ++row) {
            const std::string& line = d.logViewLines[static_cast<size_t>(row)];
            ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
        }
    }

    ImGui::PopTextWrapPos();

    if (d.logScrollToTailPending && d.logAutoScroll) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY());
        const float smax = ImGui::GetScrollMaxY();
        const float sy = ImGui::GetScrollY();
        if ((smax > 2.0f && sy >= smax - 6.0f) ||
            static_cast<std::uint64_t>(ImGui::GetFrameCount()) >= d.logScrollTailGiveUpFrame) {
            d.logScrollToTailPending = false;
        }
    }

    const float smaxAfter = ImGui::GetScrollMaxY();
    const float syAfter = ImGui::GetScrollY();
    const bool nearBottom = smaxAfter <= 1.5f || syAfter >= smaxAfter - 36.0f;
    if (d.logAutoScroll && nearBottom) {
        d.logTailReleasedByUser = false;
    } else if (d.logAutoScroll && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
               ImGui::GetIO().MouseWheel > 0.0f && syAfter < smaxAfter - 48.0f) {
        d.logTailReleasedByUser = true;
    }

    ImGui::EndChild();
    ImGui::End();
}



