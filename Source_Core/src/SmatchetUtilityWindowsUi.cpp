#include "SmatchetUI.h"

#if defined(SMATCHET_WITH_AI)
#include "AiController.h"
#endif
#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "SmatchetUiSession.h"
#include "TicketGridModel.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <limits>
#include <string>

#if defined(SMATCHET_WITH_AI)
void SmatchetUI::drawAIAssistantWindow(AppController& app, UiDrawSession& d) {
    if (d.aiPromptPending && !d.aiIsThinking) {
        d.aiIsThinking = true;
        d.aiPromptPending = false;
        // In a real app, this should be async to avoid UI freeze.
        // For this implementation, we follow the pattern in AnalyzeTicket button.
        auto result = AiController::ChatCompletion(d.aiPromptMessage, app.GetAiContext(), d.cfg.AiApiKey, d.cfg.AiModel, d.cfg.AiBaseUrl);
        d.aiResponse = result.Response;
        d.aiIsThinking = false;
        d.showAiAssistantWindow = true;
        d.requestAiAssistantFocus = true;
    }

    if (!d.showAiAssistantWindow) {
        return;
    }

    ImGui::Begin("AI Assistant", &d.showAiAssistantWindow);
    if (d.requestAiAssistantFocus) {
        ImGui::SetWindowFocus();
        d.requestAiAssistantFocus = false;
    }
    if (d.gridState.ActiveIssueId.empty()) {
        ImGui::TextDisabled("Select a ticket to see AI insights.");
    } else {
        const auto ticketsSnapAi = app.GetActiveTicketsSnapshot();
        const auto& tickets = *ticketsSnapAi;
        auto it = std::find_if(tickets.begin(), tickets.end(),
                               [&](const CachedTicket& t) { return t.id == d.gridState.ActiveIssueId; });

        if (it != tickets.end()) {
            const std::string summary = it->GetFieldValue("summary");
            ImGui::Text("Analyzing: %s", it->id.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", summary.empty() ? "<no summary field selected>" : summary.c_str());
            if (!it->fieldValues.empty()) {
                ImGui::Spacing();
                ImGui::Text("Selected Fields");
                ImGui::Separator();
                TrackerFieldCatalogIndex aiCatalog(app.GetAvailableFields());
                for (const auto& kv : it->fieldValues) {
                    const TrackerField* f = aiCatalog.Find(kv.first);
                    const std::string display = DisplayValueForTrackerDateField(kv.first, f, kv.second);
                    const std::string* tip = IsTrackerDateOrDateTimeField(kv.first, f) ? &kv.second : nullptr;
                    const std::string label = kv.first + ": ";
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::SameLine();
                    const float valueAvail = ImGui::GetContentRegionAvail().x;
                    RenderClippedFieldText(display, valueAvail, d.cfg.EnableFieldOverflowTooltips, false, tip);
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Model: %s", d.cfg.AiModel.empty() ? "(unset)" : d.cfg.AiModel.c_str());
            ImGui::TextDisabled("Endpoint: %s", d.cfg.AiBaseUrl.empty() ? "(unset)" : d.cfg.AiBaseUrl.c_str());
            if (d.cfg.AiApiKey.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                                   "Set AI API Key under Settings -> Preferences -> Assistant to enable generation.");
            }

            if (ImGui::Button("Generate Action Plan") && !d.aiIsThinking) {
                d.aiIsThinking = true;
                auto result =
                    AiController::AnalyzeTicket(it->id, summary, d.cfg.AiApiKey, d.cfg.AiModel, d.cfg.AiBaseUrl);
                d.aiResponse = result.Response;
                d.aiIsThinking = false;
            }
            
            const auto& context = app.GetAiContext();
            if (!context.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Session Context (%zu items)", context.size());
                if (ImGui::Button("Clear Context")) {
                    app.ClearAiContext();
                }
                ImGui::BeginChild("aiContextScroll", ImVec2(0, 100), true);
                for (const auto& ctx : context) {
                    ImGui::BulletText("%s", ctx.c_str());
                }
                ImGui::EndChild();
            }

            if (d.aiIsThinking)
                ImGui::Text("AI is thinking...");

            if (!d.aiResponse.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
                ImGui::TextWrapped("%s", d.aiResponse.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
}
#else
void SmatchetUI::drawAIAssistantWindow(AppController&, UiDrawSession&) {}
#endif

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
    if (ImGui::Checkbox("Auto-scroll", &d.logAutoScroll) && d.logAutoScroll) {
        d.logTailReleasedByUser = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(application log)");

    ImGui::Separator();

    const std::uint64_t revision = logger.GetRevision();
    const bool rebuildLog = d.logViewLines.empty() || revision != d.lastSeenLogRevision;
    if (rebuildLog) {
        const auto entries = logger.GetEntriesSnapshot();
        d.logViewLines.clear();
        d.logViewLines.reserve(entries.size() * 2);

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
            AppendLogEntryAsLines(d.logViewLines, levelLabel, e.message);
        }

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



