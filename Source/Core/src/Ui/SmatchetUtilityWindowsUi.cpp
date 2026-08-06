#include "SmatchetUI.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "Ui/SmatchetLog_detail.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
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

using SmatchetLog::detail::FillLogViewLinesFromEntries;
using SmatchetLog::detail::JoinLogLines;

// Scrollable log body: the cached display lines, tail-following auto-scroll, and user
// scroll-up release detection. Keeps the BeginChild/EndChild and PushTextWrapPos/PopTextWrapPos
// pairs together inside this helper. Behaviour-identical to the original inline block.
void DrawLogScrollRegion(UiDrawSession& d) {
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    // Default ImGui wrapping makes row height != clipper item height → broken clipper and “empty” bands.
    ImGui::PushTextWrapPos(FLT_MAX);

    const int lineCount = static_cast<int>(d.logViewLines.size());
    const int kMaxLogLinesDrawn = 8000;
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
}

} // namespace

void SmatchetUI::drawLogWindow(UiDrawSession& d, bool embedded) {
    Logger& logger = Logger::Instance();
    const bool wantFocus = d.requestLogFocus;
    // In embedded mode (dual-ui slice 4) the mobile Log page draws this body straight into the
    // mobile page child, so the surrounding dock-window chrome is bypassed. The desktop path
    // below stays unchanged from the pre-slice-4 flow.
    if (!embedded) {
        // Pass wantFocus as 4th arg so prepareTopLevelWindow calls SetNextWindowFocus before Begin —
        // this is what activates a docked tab. The post-Begin SetWindowFocus below is belt-and-braces
        // for floating-window state (mirrors SmatchetViewsDashboardUi.cpp pattern).
        // SMATCHET_DEVIATION(rule=duplication; reason=window-open prologue idiom; owner=ui; revisit=2026-12-01)
        prepareTopLevelWindow(d, "log", 900.0f, 320.0f, wantFocus);
        SmatchetWindowExpand::BeginWindow(d, "Log");
        if (!ImGui::Begin("Log", &d.showLogWindow)) {
            if (wantFocus) {
                d.requestLogFocus = false;
            }
            ImGui::End();
            return;
        }
        SmatchetWindowExpand::DrawToggle(d);
        repairTopLevelWindow(d, "log", 360.0f, 220.0f);
        if (wantFocus) {
            ImGui::SetWindowFocus();
            d.requestLogFocus = false;
            LOG_DEBUG("Log window: focused via menu request");
        }
    }

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
        const std::string clip = JoinLogLines(clipLines);
        ImGui::SetClipboardText(clip.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s", SmatchetLocalization::T("log.copy_log_tip", "Copy the full application log to the clipboard."));
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

    DrawLogScrollRegion(d);
    if (!embedded) {
        ImGui::End();
    }
}
