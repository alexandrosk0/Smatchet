#include "SmatchetUI.h"

#if defined(SMATCHET_WITH_AI)
#include "AiController.h"
#endif
#include "AppController.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "SmatchetUiSession.h"
#include "TicketGridModel.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
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
        ImGui::SetWindowFocus("AI Assistant");
    }

    ImGui::Begin("AI Assistant");
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
                ImGui::Text("Selected Jira Fields");
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

void SmatchetUI::drawLogWindow(UiDrawSession& d) {
    Logger& logger = Logger::Instance();
    if (!ImGui::Begin("Log", &d.showLogWindow)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear Log")) {
        logger.Clear();
        d.lastSeenLogRevision = logger.GetRevision();
        d.logBuffer.assign(1, '\0');
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(application log)");
    ImGui::SameLine();
    ImGui::TextDisabled("Log level and verbose options: Settings -> Preferences -> Diagnostics.");

    ImGui::Separator();

    const std::uint64_t revision = logger.GetRevision();
    const bool rebuildLogBuffer = d.logBuffer.empty() || revision != d.lastSeenLogRevision;
    if (rebuildLogBuffer) {
        const auto entries = logger.GetEntriesSnapshot();
        std::string aggregated;
        aggregated.reserve(entries.size() * 64);

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
            aggregated += levelLabel;
            aggregated += e.message;
            aggregated += '\n';
        }

        d.logBuffer.assign(aggregated.begin(), aggregated.end());
        d.logBuffer.push_back('\0');
        d.lastSeenLogRevision = revision;
    }

    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline("##LogText", d.logBuffer.data(), d.logBuffer.size(), ImVec2(-FLT_MIN, -FLT_MIN),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::EndChild();
    ImGui::End();
}
