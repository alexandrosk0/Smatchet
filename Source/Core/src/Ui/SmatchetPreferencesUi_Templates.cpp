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
#include "Logger.h"
#include "SmatchetHelpMarker.h"
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

namespace {

// Grid behaviour. The overflow-tooltip and wheel-swallow rows came from the old
// Appearance tab's "Grid and field text" block — both are grid behaviour, not
// typography, and the descriptor table homes them here.
void DrawGridSectionBody(UiDrawSession& d) {
    // No title/Separator here — PrefsSection already drew the section header.
    if (d.prefsFilter.ShowSetting("editing.grid.single_click")) {
        if (ImGui::Checkbox("Single-click to edit grid cells", &d.cfg.SingleClickToEditGridCells)) {
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("When off, double-click is required to begin editing any cell. Default: on.");
    }
    if (d.prefsFilter.ShowSetting("editing.grid.long_text_preview")) {
        if (ImGui::Checkbox("Open long-text editor in preview mode", &d.cfg.DefaultLongTextEditorPreview)) {
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("Long-text editor opens in preview mode.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.templates.longtext_preview.help",
                                   "When on, the long-text edit modal (description, callstack, custom "
                                   "textarea fields) opens showing the rendered preview. When off (default) "
                                   "it opens in edit mode. Ctrl+P cycles Edit/Split/Preview either way.");
    }
    if (d.prefsFilter.ShowSetting("editing.grid.overflow_tooltips")) {
        if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("Hover truncated cells to read the full text.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.appearance.overflow_tooltips.help",
                                   "When a value is truncated to fit the cell, or spans multiple lines, hover "
                                   "to read the full text in a tooltip.");
    }
    if (d.prefsFilter.ShowSetting("editing.grid.wheel_ticks")) {
        int gridWheelSwallowTicks = d.cfg.GridEndWheelSwallowsBeforeHorizontal;
        if (ImGui::InputInt("Wheel ticks before horizontal scroll", &gridWheelSwallowTicks)) {
            if (gridWheelSwallowTicks < 0) {
                gridWheelSwallowTicks = 0;
            }
            if (gridWheelSwallowTicks > 32) {
                gridWheelSwallowTicks = 32;
            }
            d.cfg.GridEndWheelSwallowsBeforeHorizontal = gridWheelSwallowTicks;
            MarkPrefsDirty(d);
        }
        ImGui::SetItemTooltip("Wheel ticks swallowed at the grid edge.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.appearance.wheel_swallow.help",
                                   "At top/bottom of the ticket grid, vertical wheel starts horizontal "
                                   "scrolling after this many wheel ticks. 0 routes immediately.");
    }
}

void DrawTimeEstimatesSectionBody(UiDrawSession& d, SmatchetPreferencesUiTemplateFlags& flags) {
    // One descriptor covers the whole list editor — its per-row inputs are not
    // individually filterable.
    if (!d.prefsFilter.ShowSetting("editing.time_estimates.duration_suggestions")) {
        return;
    }
    ImGui::TextDisabled("Default options for time-estimate dropdowns.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.templates.duration_suggestions.help",
                               "Customize the default options displayed in the dropdown menus for "
                               "Original Estimate, Remaining Estimate, and Time Spent fields.");
    ImGui::Spacing();

    // Pillar 2 (#732): seed the working list from live cfg (no disk read) and persist each edit
    // via MarkPrefsDirty's debounced save — never a synchronous per-click RMW+Save on the render
    // thread. Matches the sibling comment-template sub-tabs, which write the cfg field then mark
    // dirty. The runtime consumer (TicketFieldEditor) re-reads via ConfigManager once the
    // debounced save lands.
    static std::vector<std::string> s_suggestionsList;
    if (!flags.suggestionsLoaded) {
        s_suggestionsList = d.cfg.DurationSuggestions;
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
                d.cfg.DurationSuggestions = s_suggestionsList;
                MarkPrefsDirty(d);
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
                d.cfg.DurationSuggestions = s_suggestionsList;
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
            s_suggestionsList.erase(s_suggestionsList.begin() + i);
            d.cfg.DurationSuggestions = s_suggestionsList;
            MarkPrefsDirty(d);
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
    // EnterReturnsTrue commits from the input itself — the old IsItemFocused() check
    // ran on the BUTTON, so Enter in the field silently did nothing (P2-L10).
    const bool suggestionEntered =
        ImGui::InputText("##PrefNewSuggestion", s_prefNewSuggestionBuf, sizeof(s_prefNewSuggestionBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Add Option", ImVec2(90.0f, 0.0f)) || suggestionEntered) {
        std::string newVal = s_prefNewSuggestionBuf;
        if (!newVal.empty()) {
            if (std::find(s_suggestionsList.begin(), s_suggestionsList.end(), newVal) == s_suggestionsList.end()) {
                s_suggestionsList.push_back(newVal);
                d.cfg.DurationSuggestions = s_suggestionsList;
                MarkPrefsDirty(d);
            }
            s_prefNewSuggestionBuf[0] = '\0';
        }
    }
    ImGui::SetItemTooltip("Enter duration strings e.g. '15m', '2h', '3.5h', '1d', '2w'");
}

void DrawWorkLogTemplatesSectionBody(UiDrawSession& d, SmatchetPreferencesUiTemplateFlags& flags) {
    // One descriptor per list editor (see DrawTimeEstimatesSectionBody).
    if (!d.prefsFilter.ShowSetting("editing.work_log_templates.list")) {
        return;
    }
    ImGui::TextDisabled("Customize the quick comment templates displayed in the 'Templates' dropdown "
                        "next to the Log Work description field.");
    ImGui::Spacing();

    // Pillar 2 (#732): seed from live cfg + persist via debounced MarkPrefsDirty (see the
    // duration sub-tab above) instead of a synchronous per-click ConfigManager RMW+Save.
    static std::vector<std::string> s_templatesList;
    if (!flags.templatesLoaded) {
        s_templatesList = d.cfg.WorkLogCommentTemplates;
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
                d.cfg.WorkLogCommentTemplates = s_templatesList;
                MarkPrefsDirty(d);
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
                d.cfg.WorkLogCommentTemplates = s_templatesList;
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
            s_templatesList.erase(s_templatesList.begin() + i);
            d.cfg.WorkLogCommentTemplates = s_templatesList;
            MarkPrefsDirty(d);
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
    const bool templateEntered =
        ImGui::InputText("##PrefNewTemplate", s_prefNewTemplateBuf, sizeof(s_prefNewTemplateBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue); // Enter adds (P2-L10)
    ImGui::SameLine();
    if (ImGui::Button("Add Template", ImVec2(100.0f, 0.0f)) || templateEntered) {
        std::string newVal = s_prefNewTemplateBuf;
        if (!newVal.empty()) {
            if (std::find(s_templatesList.begin(), s_templatesList.end(), newVal) == s_templatesList.end()) {
                s_templatesList.push_back(newVal);
                d.cfg.WorkLogCommentTemplates = s_templatesList;
                MarkPrefsDirty(d);
            }
            s_prefNewTemplateBuf[0] = '\0';
        }
    }
    ImGui::SetItemTooltip("Enter template text, e.g. 'Investigated and resolved issue #123.'");
}

// Persistent per-sub-tab state for a comment-template editor (working-copy list, selection, and the
// detail-editor input buffers). One static instance per sub-tab keeps the two editors independent —
// exactly the behaviour the former separate function-static sets had.
struct CommentTemplateSubTabState {
    std::vector<CommentTemplate> workingList;
    int selectedIdx = -1;
    char titleBuf[64] = "";
    char idBuf[64] = "";
    char textBuf[4096] = ""; // 512 silently truncated longer bodies on the first keystroke (P2-L9)
    int lastSelectedIdx = -2;
    /// Row index whose two-step delete is armed (-1 = none). A member (not index-keyed
    /// ImGui storage) so reorder/delete can clear it — storage keyed by position would
    /// let "armed" drift onto whatever template lands in that slot.
    int armedDeleteIdx = -1;
};

// Scrollable list of templates with per-row move-up / move-down / delete. Extracted from the shared
// comment-template sub-tab body (over-100-line decomposition); behaviour-identical.
void DrawCommentTemplateList(UiDrawSession& d, const char* childId, std::vector<CommentTemplate>& cfgField,
                             CommentTemplateSubTabState& st) {
    // Render list of current comment templates on top
    ImGui::BeginChild(childId, ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    for (size_t i = 0; i < st.workingList.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        std::string displayName = st.workingList[i].Title + " (" + st.workingList[i].Id + ")";
        if (ImGui::Selectable(displayName.c_str(), st.selectedIdx == static_cast<int>(i))) {
            st.selectedIdx = static_cast<int>(i);
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 82.0f);
        if (i > 0) {
            if (ImGui::Button("▲")) {
                st.armedDeleteIdx = -1; // positions shift — never carry an armed delete across
                std::swap(st.workingList[i], st.workingList[i - 1]);
                if (st.selectedIdx == static_cast<int>(i))
                    st.selectedIdx = static_cast<int>(i - 1);
                else if (st.selectedIdx == static_cast<int>(i - 1))
                    st.selectedIdx = static_cast<int>(i);
                cfgField = st.workingList;
                MarkPrefsDirty(d);
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("▲");
            ImGui::EndDisabled();
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56.0f);
        if (i < st.workingList.size() - 1) {
            if (ImGui::Button("▼")) {
                st.armedDeleteIdx = -1;
                std::swap(st.workingList[i], st.workingList[i + 1]);
                if (st.selectedIdx == static_cast<int>(i))
                    st.selectedIdx = static_cast<int>(i + 1);
                else if (st.selectedIdx == static_cast<int>(i + 1))
                    st.selectedIdx = static_cast<int>(i);
                cfgField = st.workingList;
                MarkPrefsDirty(d);
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("▼");
            ImGui::EndDisabled();
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30.0f);
        // P2-L9: two-step delete — this tab autosaves, so an instant ✖ permanently
        // destroys a hand-authored template on one stray click. First click arms; the
        // second (now "✔?") confirms. A click anywhere else disarms.
        const bool armed = st.armedDeleteIdx == static_cast<int>(i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        const bool delClicked = ImGui::Button(armed ? "✔?" : "✖");
        const bool delHovered = ImGui::IsItemHovered();
        if (armed && delHovered) {
            ImGui::SetTooltip("Click again to delete this template");
        }
        if (delClicked && !armed) {
            st.armedDeleteIdx = static_cast<int>(i);
        } else if (armed && !delClicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !delHovered) {
            st.armedDeleteIdx = -1;
        }
        if (delClicked && armed) {
            st.armedDeleteIdx = -1;
            st.workingList.erase(st.workingList.begin() + i);
            if (st.selectedIdx == static_cast<int>(i)) {
                st.selectedIdx = -1;
            } else if (st.selectedIdx > static_cast<int>(i)) {
                st.selectedIdx--;
            }
            cfgField = st.workingList;
            MarkPrefsDirty(d);
            --i;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Detail editor for the selected template (Title / ID / Body) plus the "+ Add New Template" button.
// Extracted from the shared comment-template sub-tab body (over-100-line decomposition); behaviour-identical.
void DrawCommentTemplateDetailAndAdd(UiDrawSession& d, const char* titleWidgetId, const char* idWidgetId,
                                     const char* bodyWidgetId, std::vector<CommentTemplate>& cfgField,
                                     CommentTemplateSubTabState& st) {
    // Detail section / Add section
    if (st.selectedIdx >= 0 && st.selectedIdx < static_cast<int>(st.workingList.size())) {
        auto& t = st.workingList[st.selectedIdx];
        ImGui::TextDisabled("Edit Selected Template details:");

        // Copy to buffer if different to avoid typing overwrites
        if (st.lastSelectedIdx != st.selectedIdx) {
            std::strncpy(st.titleBuf, t.Title.c_str(), sizeof(st.titleBuf) - 1);
            st.titleBuf[sizeof(st.titleBuf) - 1] = '\0';
            std::strncpy(st.idBuf, t.Id.c_str(), sizeof(st.idBuf) - 1);
            st.idBuf[sizeof(st.idBuf) - 1] = '\0';
            std::strncpy(st.textBuf, t.Text.c_str(), sizeof(st.textBuf) - 1);
            st.textBuf[sizeof(st.textBuf) - 1] = '\0';
            st.lastSelectedIdx = st.selectedIdx;
        }

        ImGui::TextUnformatted("Title:");
        ImGui::SameLine(60.0f);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText(titleWidgetId, st.titleBuf, sizeof(st.titleBuf))) {
            t.Title = st.titleBuf;
            cfgField = st.workingList;
            MarkPrefsDirty(d);
        }

        ImGui::SameLine(280.0f);
        ImGui::TextUnformatted("ID:");
        ImGui::SameLine(310.0f);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText(idWidgetId, st.idBuf, sizeof(st.idBuf))) {
            t.Id = st.idBuf;
            cfgField = st.workingList;
            MarkPrefsDirty(d);
        }

        ImGui::TextUnformatted("Body:");
        if (ImGui::InputTextMultiline(bodyWidgetId, st.textBuf, sizeof(st.textBuf), ImVec2(-FLT_MIN, 60.0f))) {
            t.Text = st.textBuf;
            cfgField = st.workingList;
            MarkPrefsDirty(d);
        }
        // Make the cap visible instead of silently cutting the body off (P2-L9).
        ImGui::TextDisabled("%d / %d", static_cast<int>(std::strlen(st.textBuf)),
                            static_cast<int>(sizeof(st.textBuf) - 1));
        if (t.Text.size() > sizeof(st.textBuf) - 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(stored body is longer than the editor cap - editing here would truncate it)");
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
        st.workingList.push_back(t);
        st.selectedIdx = static_cast<int>(st.workingList.size()) - 1;
        cfgField = st.workingList;
        MarkPrefsDirty(d);
    }
}

// Shared body for a comment-template editor section. The Quick-Comments and Annotate-Comments
// editors were byte-identical apart from their labels, widget ids, and backing config field, so
// this collapses both for DRY while staying behaviour-identical, with the caller passing every
// ImGui id and label verbatim. The cfgField argument is the persisted vector and loadedFlag gates
// the one-time working-copy load. The owning PrefsSection draws the header.
void DrawCommentTemplateSectionBody(UiDrawSession& d, const char* placeholderHelp, const char* childId,
                                    const char* titleWidgetId, const char* idWidgetId, const char* bodyWidgetId,
                                    std::vector<CommentTemplate>& cfgField, bool& loadedFlag,
                                    CommentTemplateSubTabState& st) {
    ImGui::TextDisabled("%s", placeholderHelp);
    ImGui::Spacing();

    if (!loadedFlag) {
        st.workingList = cfgField;
        loadedFlag = true;
    }
    if (st.selectedIdx >= static_cast<int>(st.workingList.size())) {
        st.selectedIdx = static_cast<int>(st.workingList.size()) - 1;
    }

    DrawCommentTemplateList(d, childId, cfgField, st);
    ImGui::Spacing();
    DrawCommentTemplateDetailAndAdd(d, titleWidgetId, idWidgetId, bodyWidgetId, cfgField, st);
}

void DrawQuickCommentsSectionBody(UiDrawSession& d, SmatchetPreferencesUiTemplateFlags& flags) {
    // One descriptor per list editor (see DrawTimeEstimatesSectionBody).
    if (!d.prefsFilter.ShowSetting("editing.quick_comments.list")) {
        return;
    }
    static CommentTemplateSubTabState st;
    DrawCommentTemplateSectionBody(d,
                                   "Customize templates displayed when right-clicking issue cells in the grid. "
                                   "Placeholders: {key} (or {issueKey})",
                                   "QuickListChild", "##EditQuickTitle", "##EditQuickId", "##EditQuickText",
                                   d.cfg.QuickCommentTemplates, flags.quickTemplatesLoaded, st);
}

void DrawAnnotateCommentsSectionBody(UiDrawSession& d, SmatchetPreferencesUiTemplateFlags& flags) {
    // One descriptor per list editor (see DrawTimeEstimatesSectionBody).
    if (!d.prefsFilter.ShowSetting("editing.annotate_comments.list")) {
        return;
    }
    static CommentTemplateSubTabState st;
    DrawCommentTemplateSectionBody(d,
                                   "Customize templates displayed when clicking on the Annotate rows. "
                                   "Placeholders: {key}, {path}, {line}, {cl}, {user}, {function}",
                                   "AnnotateListChild", "##EditAnnotateTitle", "##EditAnnotateId", "##EditAnnotateText",
                                   d.cfg.AnnotateCommentTemplates, flags.annotateTemplatesLoaded, st);
}

} // namespace

void DrawEditingPreferencesTab(UiDrawSession& d, SmatchetPreferencesUiTemplateFlags& flags) {
    namespace Detail = SmatchetPreferencesUiDetail;
    Detail::PrefsSection(d, "editing.grid", [&] { DrawGridSectionBody(d); });
    Detail::PrefsSection(d, "editing.time_estimates", [&] { DrawTimeEstimatesSectionBody(d, flags); });
    Detail::PrefsSection(d, "editing.work_log_templates", [&] { DrawWorkLogTemplatesSectionBody(d, flags); });
    Detail::PrefsSection(d, "editing.quick_comments", [&] { DrawQuickCommentsSectionBody(d, flags); });
    Detail::PrefsSection(d, "editing.annotate_comments", [&] { DrawAnnotateCommentsSectionBody(d, flags); });
}
