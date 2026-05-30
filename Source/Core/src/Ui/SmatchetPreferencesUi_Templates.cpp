#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetUI.h"
#include "AppController.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "TrackerFieldValueUtils.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <cstring>
#include <string>
#include <vector>

void DrawTemplatePreferencesTabs(SmatchetUI& ui, AppController& app, UiDrawSession& d,
                                  SmatchetPreferencesUiTemplateFlags& flags) {
        if (ImGui::BeginTabItem("Grid")) {
            ImGui::TextUnformatted("Editing");
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Checkbox("Single-click to edit grid cells", &d.cfg.SingleClickToEditGridCells)) {
                MarkPrefsDirty(d);
            }
            ImGui::SetItemTooltip("When off, double-click is required to begin editing any cell. Default: on.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fields Inputs")) {
            if (ImGui::BeginTabBar("FieldsInputsSubTabBar")) {
                if (ImGui::BeginTabItem("Time Estimates")) {
                    ImGui::TextUnformatted("Duration Suggestions");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize the default options displayed in the dropdown menus for Original "
                                        "Estimate, Remaining Estimate, and Time Spent fields.");
                    ImGui::Spacing();

                    static std::vector<std::string> s_suggestionsList;
                    if (!flags.suggestionsLoaded) {
                        s_suggestionsList = TrackerFieldValueUtils::LoadDurationSuggestions();
                        flags.suggestionsLoaded = true;
                    }

                    // Render list of current suggestions in a premium boxed child frame
                    ImGui::Text("Current Suggestions:");
                    ImGui::BeginChild("SuggestionsListChild", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_suggestionsList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));

                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(s_suggestionsList[i].c_str());

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_suggestionsList[i], s_suggestionsList[i - 1]);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_suggestionsList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_suggestionsList[i], s_suggestionsList[i + 1]);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_suggestionsList.erase(s_suggestionsList.begin() + i);
                            TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            --i;
                        }
                        ImGui::PopStyleColor();

                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Inline add controls
                    static char s_prefNewSuggestionBuf[16] = "";
                    ImGui::Text("Add Custom Suggestion");
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::InputText("##PrefNewSuggestion", s_prefNewSuggestionBuf, sizeof(s_prefNewSuggestionBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Option", ImVec2(90.0f, 0.0f)) ||
                        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        std::string newVal = s_prefNewSuggestionBuf;
                        if (!newVal.empty()) {
                            if (std::find(s_suggestionsList.begin(), s_suggestionsList.end(), newVal) ==
                                s_suggestionsList.end()) {
                                s_suggestionsList.push_back(newVal);
                                TrackerFieldValueUtils::SaveDurationSuggestions(s_suggestionsList);
                            }
                            s_prefNewSuggestionBuf[0] = '\0';
                        }
                    }
                    ImGui::SetItemTooltip("Enter duration strings e.g. '15m', '2h', '3.5h', '1d', '2w'");

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Work Log Templates")) {
                    ImGui::TextUnformatted("Work Log Description Templates");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize the quick comment templates displayed in the 'Templates' dropdown "
                                        "next to the Log Work description field.");
                    ImGui::Spacing();

                    static std::vector<std::string> s_templatesList;
                    if (!flags.templatesLoaded) {
                        s_templatesList = TrackerFieldValueUtils::LoadCommentTemplates();
                        flags.templatesLoaded = true;
                    }

                    // Render list of current comment templates in a premium boxed child frame
                    ImGui::Text("Current Comment Templates:");
                    ImGui::BeginChild("TemplatesListChild", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_templatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));

                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(s_templatesList[i].c_str());

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_templatesList[i], s_templatesList[i - 1]);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_templatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_templatesList[i], s_templatesList[i + 1]);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_templatesList.erase(s_templatesList.begin() + i);
                            TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            --i;
                        }
                        ImGui::PopStyleColor();

                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Inline add controls for templates
                    static char s_prefNewTemplateBuf[128] = "";
                    ImGui::Text("Add Comment Template");
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110.0f);
                    ImGui::InputText("##PrefNewTemplate", s_prefNewTemplateBuf, sizeof(s_prefNewTemplateBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Add Template", ImVec2(100.0f, 0.0f)) ||
                        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        std::string newVal = s_prefNewTemplateBuf;
                        if (!newVal.empty()) {
                            if (std::find(s_templatesList.begin(), s_templatesList.end(), newVal) ==
                                s_templatesList.end()) {
                                s_templatesList.push_back(newVal);
                                TrackerFieldValueUtils::SaveCommentTemplates(s_templatesList);
                            }
                            s_prefNewTemplateBuf[0] = '\0';
                        }
                    }
                    ImGui::SetItemTooltip("Enter template text, e.g. 'Investigated and resolved issue #123.'");

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Quick Comments")) {
                    ImGui::TextUnformatted("Grid Right-Click Quick Comments");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize templates displayed when right-clicking issue cells in the grid. "
                                        "Placeholders: {key} (or {issueKey})");
                    ImGui::Spacing();

                    static std::vector<CommentTemplate> s_quickTemplatesList;
                    if (!flags.quickTemplatesLoaded) {
                        s_quickTemplatesList = d.cfg.QuickCommentTemplates;
                        flags.quickTemplatesLoaded = true;
                    }
                    static int s_selectedQuickIdx = -1;
                    if (s_selectedQuickIdx >= static_cast<int>(s_quickTemplatesList.size())) {
                        s_selectedQuickIdx = static_cast<int>(s_quickTemplatesList.size()) - 1;
                    }

                    // Render list of current comment templates on top
                    ImGui::BeginChild("QuickListChild", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_quickTemplatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::AlignTextToFramePadding();
                        std::string displayName =
                            s_quickTemplatesList[i].Title + " (" + s_quickTemplatesList[i].Id + ")";
                        if (ImGui::Selectable(displayName.c_str(), s_selectedQuickIdx == static_cast<int>(i))) {
                            s_selectedQuickIdx = static_cast<int>(i);
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_quickTemplatesList[i], s_quickTemplatesList[i - 1]);
                                if (s_selectedQuickIdx == static_cast<int>(i))
                                    s_selectedQuickIdx = static_cast<int>(i - 1);
                                else if (s_selectedQuickIdx == static_cast<int>(i - 1))
                                    s_selectedQuickIdx = static_cast<int>(i);
                                d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                                MarkPrefsDirty(d);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_quickTemplatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_quickTemplatesList[i], s_quickTemplatesList[i + 1]);
                                if (s_selectedQuickIdx == static_cast<int>(i))
                                    s_selectedQuickIdx = static_cast<int>(i + 1);
                                else if (s_selectedQuickIdx == static_cast<int>(i + 1))
                                    s_selectedQuickIdx = static_cast<int>(i);
                                d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                                MarkPrefsDirty(d);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_quickTemplatesList.erase(s_quickTemplatesList.begin() + i);
                            if (s_selectedQuickIdx == static_cast<int>(i)) {
                                s_selectedQuickIdx = -1;
                            } else if (s_selectedQuickIdx > static_cast<int>(i)) {
                                s_selectedQuickIdx--;
                            }
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
                            --i;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();

                    // Detail section / Add section
                    if (s_selectedQuickIdx >= 0 && s_selectedQuickIdx < static_cast<int>(s_quickTemplatesList.size())) {
                        auto& t = s_quickTemplatesList[s_selectedQuickIdx];
                        ImGui::TextDisabled("Edit Selected Template details:");

                        static char titleBuf[64] = "";
                        static char idBuf[64] = "";
                        static char textBuf[512] = "";

                        // Copy to buffer if different to avoid typing overwrites
                        static int lastSelectedIdx = -2;
                        if (lastSelectedIdx != s_selectedQuickIdx) {
                            std::strncpy(titleBuf, t.Title.c_str(), sizeof(titleBuf) - 1);
                            titleBuf[sizeof(titleBuf) - 1] = '\0';
                            std::strncpy(idBuf, t.Id.c_str(), sizeof(idBuf) - 1);
                            idBuf[sizeof(idBuf) - 1] = '\0';
                            std::strncpy(textBuf, t.Text.c_str(), sizeof(textBuf) - 1);
                            textBuf[sizeof(textBuf) - 1] = '\0';
                            lastSelectedIdx = s_selectedQuickIdx;
                        }

                        ImGui::TextUnformatted("Title:");
                        ImGui::SameLine(60.0f);
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::InputText("##EditQuickTitle", titleBuf, sizeof(titleBuf))) {
                            t.Title = titleBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditQuickId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditQuickText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                            MarkPrefsDirty(d);
                        }
                    } else {
                        ImGui::TextDisabled("Select a template above to view or edit its details.");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("+ Add New Template", ImVec2(160.0f, 0.0f))) {
                        CommentTemplate t;
                        t.Title = "New Template";
                        t.Id = "new_template";
                        t.Text = "Template text for {key}";
                        s_quickTemplatesList.push_back(t);
                        s_selectedQuickIdx = static_cast<int>(s_quickTemplatesList.size()) - 1;
                        d.cfg.QuickCommentTemplates = s_quickTemplatesList;
                        MarkPrefsDirty(d);
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Annotate Comments")) {
                    ImGui::TextUnformatted("Annotate Quick Comments");
                    ImGui::Separator();
                    ImGui::Spacing();
                    ImGui::TextDisabled("Customize templates displayed when clicking on the Annotate rows. "
                                        "Placeholders: {key}, {path}, {line}, {cl}, {user}, {function}");
                    ImGui::Spacing();

                    static std::vector<CommentTemplate> s_annotateTemplatesList;
                    if (!flags.annotateTemplatesLoaded) {
                        s_annotateTemplatesList = d.cfg.AnnotateCommentTemplates;
                        flags.annotateTemplatesLoaded = true;
                    }
                    static int s_selectedAnnotateIdx = -1;
                    if (s_selectedAnnotateIdx >= static_cast<int>(s_annotateTemplatesList.size())) {
                        s_selectedAnnotateIdx = static_cast<int>(s_annotateTemplatesList.size()) - 1;
                    }

                    // Render list of current comment templates on top
                    ImGui::BeginChild("AnnotateListChild", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders,
                                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    for (size_t i = 0; i < s_annotateTemplatesList.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::AlignTextToFramePadding();
                        std::string displayName =
                            s_annotateTemplatesList[i].Title + " (" + s_annotateTemplatesList[i].Id + ")";
                        if (ImGui::Selectable(displayName.c_str(), s_selectedAnnotateIdx == static_cast<int>(i))) {
                            s_selectedAnnotateIdx = static_cast<int>(i);
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
                        if (i > 0) {
                            if (ImGui::Button("▲")) {
                                std::swap(s_annotateTemplatesList[i], s_annotateTemplatesList[i - 1]);
                                if (s_selectedAnnotateIdx == static_cast<int>(i))
                                    s_selectedAnnotateIdx = static_cast<int>(i - 1);
                                else if (s_selectedAnnotateIdx == static_cast<int>(i - 1))
                                    s_selectedAnnotateIdx = static_cast<int>(i);
                                d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                                MarkPrefsDirty(d);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▲");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
                        if (i < s_annotateTemplatesList.size() - 1) {
                            if (ImGui::Button("▼")) {
                                std::swap(s_annotateTemplatesList[i], s_annotateTemplatesList[i + 1]);
                                if (s_selectedAnnotateIdx == static_cast<int>(i))
                                    s_selectedAnnotateIdx = static_cast<int>(i + 1);
                                else if (s_selectedAnnotateIdx == static_cast<int>(i + 1))
                                    s_selectedAnnotateIdx = static_cast<int>(i);
                                d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                                MarkPrefsDirty(d);
                            }
                        } else {
                            ImGui::BeginDisabled();
                            ImGui::Button("▼");
                            ImGui::EndDisabled();
                        }

                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        if (ImGui::Button("✖")) {
                            s_annotateTemplatesList.erase(s_annotateTemplatesList.begin() + i);
                            if (s_selectedAnnotateIdx == static_cast<int>(i)) {
                                s_selectedAnnotateIdx = -1;
                            } else if (s_selectedAnnotateIdx > static_cast<int>(i)) {
                                s_selectedAnnotateIdx--;
                            }
                            d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                            MarkPrefsDirty(d);
                            --i;
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();

                    ImGui::Spacing();

                    // Detail section / Add section
                    if (s_selectedAnnotateIdx >= 0 &&
                        s_selectedAnnotateIdx < static_cast<int>(s_annotateTemplatesList.size())) {
                        auto& t = s_annotateTemplatesList[s_selectedAnnotateIdx];
                        ImGui::TextDisabled("Edit Selected Template details:");

                        static char titleBuf[64] = "";
                        static char idBuf[64] = "";
                        static char textBuf[512] = "";

                        // Copy to buffer if different to avoid typing overwrites
                        static int lastSelectedIdx = -2;
                        if (lastSelectedIdx != s_selectedAnnotateIdx) {
                            std::strncpy(titleBuf, t.Title.c_str(), sizeof(titleBuf) - 1);
                            titleBuf[sizeof(titleBuf) - 1] = '\0';
                            std::strncpy(idBuf, t.Id.c_str(), sizeof(idBuf) - 1);
                            idBuf[sizeof(idBuf) - 1] = '\0';
                            std::strncpy(textBuf, t.Text.c_str(), sizeof(textBuf) - 1);
                            textBuf[sizeof(textBuf) - 1] = '\0';
                            lastSelectedIdx = s_selectedAnnotateIdx;
                        }

                        ImGui::TextUnformatted("Title:");
                        ImGui::SameLine(60.0f);
                        ImGui::SetNextItemWidth(200.0f);
                        if (ImGui::InputText("##EditAnnotateTitle", titleBuf, sizeof(titleBuf))) {
                            t.Title = titleBuf;
                            d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::SameLine(280.0f);
                        ImGui::TextUnformatted("ID:");
                        ImGui::SameLine(310.0f);
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::InputText("##EditAnnotateId", idBuf, sizeof(idBuf))) {
                            t.Id = idBuf;
                            d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                            MarkPrefsDirty(d);
                        }

                        ImGui::TextUnformatted("Body:");
                        if (ImGui::InputTextMultiline("##EditAnnotateText", textBuf, sizeof(textBuf),
                                                      ImVec2(-FLT_MIN, 60.0f))) {
                            t.Text = textBuf;
                            d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                            MarkPrefsDirty(d);
                        }
                    } else {
                        ImGui::TextDisabled("Select a template above to view or edit its details.");
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Button("+ Add New Template", ImVec2(160.0f, 0.0f))) {
                        CommentTemplate t;
                        t.Title = "New Template";
                        t.Id = "new_template";
                        t.Text = "Template text for {key}";
                        s_annotateTemplatesList.push_back(t);
                        s_selectedAnnotateIdx = static_cast<int>(s_annotateTemplatesList.size()) - 1;
                        d.cfg.AnnotateCommentTemplates = s_annotateTemplatesList;
                        MarkPrefsDirty(d);
                    }

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Annotate")) {
            ui.DrawAnnotatePreferencesTabForwarded(app);
            ImGui::EndTabItem();
        }
}