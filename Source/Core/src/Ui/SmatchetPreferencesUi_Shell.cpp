// Preferences window shell helpers: the left category nav rail (or the
// narrow-width category combo) with its no-match dimming, and the collapsible
// PrefsSection chrome with per-section save-semantics hints.
// The positional Begin/End frame itself stays in drawPreferencesWindow
// (SmatchetPreferencesUi.cpp); nothing here splits a positional-ImGui pair
// across the call boundary.

#include "SmatchetPreferencesUi_detail.h"

#include "ConfigManager.h"
#include "PreferencesSchema.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"

#include <cfloat>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

namespace SmatchetPreferencesUiDetail {

namespace {

/// One-line footer-replacement hint drawn at the top of an open section.
const char* SaveHintForSemantics(SmatchetPrefsSchema::SaveSemantics save) {
    switch (save) {
    case SmatchetPrefsSchema::SaveSemantics::SaveAndSync:
        return "Changes here apply after Save & Sync.";
    case SmatchetPrefsSchema::SaveSemantics::Autosave:
        return "Changes save automatically.";
    case SmatchetPrefsSchema::SaveSemantics::Immediate:
        return "Changes apply immediately.";
    case SmatchetPrefsSchema::SaveSemantics::Restart:
        return "Changes take effect after restart.";
    case SmatchetPrefsSchema::SaveSemantics::ExplicitSave:
        return "Use this section's Save button to apply.";
    case SmatchetPrefsSchema::SaveSemantics::AnnotateDetached:
        return "Changes save via the Annotate section's own Save buttons.";
    }
    return "";
}

/// Parse the comma-joined cfg string into the session set once per app run.
/// cfg is loaded after session construction, so this must run lazily on the
/// first Preferences draw rather than in the UiDrawSession constructor.
void EnsureCollapsedSectionsLoaded(UiDrawSession& d) {
    if (d.prefsCollapsedLoaded) {
        return;
    }
    d.prefsCollapsedSections.clear();
    const std::string& joined = d.cfg.PreferencesCollapsedSections;
    std::size_t start = 0;
    while (start <= joined.size()) {
        std::size_t comma = joined.find(',', start);
        if (comma == std::string::npos) {
            comma = joined.size();
        }
        if (comma > start) {
            d.prefsCollapsedSections.insert(joined.substr(start, comma - start));
        }
        start = comma + 1;
    }
    d.prefsCollapsedLoaded = true;
}

/// Re-join the session set into the persisted cfg string and schedule the
/// debounced save.
void PersistCollapsedSections(UiDrawSession& d) {
    std::string joined;
    for (const std::string& id : d.prefsCollapsedSections) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += id;
    }
    d.cfg.PreferencesCollapsedSections = joined;
    MarkPrefsDirty(d);
}

/// One nav entry. navId is the stable "###"-suffixed ImGui id so tests and
/// translations never shift the item id; dirty appends a " *" marker to the
/// visible part only. SchemaId keys the descriptor-table category so the rail
/// can dim entries holding no search match.
struct PrefsNavEntry {
    PreferencesCategory Category;
    const char* SchemaId;
    const char* TitleEn;
    const char* NavId;
    bool Dirty;
};

/// Compose "Title###navId" / "Title *###navId" into buf and return it.
const char* ComposeNavLabel(char* buf, std::size_t bufSize, const PrefsNavEntry& e) {
    std::snprintf(buf, bufSize, "%s%s###%s", e.TitleEn, e.Dirty ? " *" : "", e.NavId);
    return buf;
}

/// True when the filter is active and this category holds nothing that matches.
/// Such an entry draws disabled rather than merely dimmed: selecting it lands on
/// an empty pane, which reads as "the category is broken" instead of "your query
/// does not reach here".
bool CategoryIsEmptyUnderFilter(UiDrawSession& d, bool filtering, const char* schemaId) {
    return filtering && d.prefsFilter.CategoryMatchCount(schemaId) == 0;
}

} // namespace

bool PrefsSectionBegin(UiDrawSession& d, const char* sectionId) {
    ImGui::PushID(sectionId);
    if (d.prefsFilter.Active() && !d.prefsFilter.SectionHasMatch(sectionId)) {
        ImGui::PopID();
        return false;
    }
    EnsureCollapsedSectionsLoaded(d);
    const SmatchetPrefsSchema::PrefsSectionDesc* desc = SmatchetPrefsSchema::FindSection(sectionId);
    const char* title = desc != nullptr ? desc->TitleEn : sectionId;
    bool open = false;
    if (d.prefsFilter.Active()) {
        // While filtering, matched sections force-open; never persist that.
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        open = ImGui::CollapsingHeader(title);
    } else {
        const bool wasOpen = d.prefsCollapsedSections.count(sectionId) == 0;
        ImGui::SetNextItemOpen(wasOpen, ImGuiCond_Always);
        open = ImGui::CollapsingHeader(title);
        if (open != wasOpen) {
            if (open) {
                d.prefsCollapsedSections.erase(sectionId);
            } else {
                d.prefsCollapsedSections.insert(sectionId);
            }
            PersistCollapsedSections(d);
        }
    }
    if (!open) {
        ImGui::PopID();
        return false;
    }
    if (desc != nullptr) {
        // "%s" indirection is mandatory: ImGui here is SmatchetLocalizedImGui, whose
        // TextDisabled translates the format string first — a translator's stray '%'
        // would then read an empty va_list.
        ImGui::TextDisabled("%s", SaveHintForSemantics(desc->Save));
    }
    return true;
}

void PrefsSectionEnd(UiDrawSession& d, const char* sectionId) {
    (void)d;
    (void)sectionId;
    ImGui::Spacing();
    ImGui::PopID();
}

void DrawPrefsNav(UiDrawSession& d, bool trackerDirty, bool assistantDirty, float bodyHeight) {
    (void)assistantDirty; // only read when the AI feature is compiled in
    // Order mirrors SmatchetPrefsSchema::Categories(); AI & Voice is compiled
    // out entirely when neither feature is built, matching the schema's guard.
    const PrefsNavEntry entries[] = {
        {PreferencesCategory::General, "general", "General", "prefsNavGeneral", false},
        {PreferencesCategory::Appearance, "appearance", "Appearance", "prefsNavAppearance", false},
        {PreferencesCategory::Tracker, "tracker", "Tracker", "prefsNavTracker", trackerDirty},
        {PreferencesCategory::Connections, "connections", "Connections", "prefsNavConnections", false},
#if defined(SMATCHET_WITH_AI) || defined(SMATCHET_WITH_WHISPER)
        {PreferencesCategory::AiVoice, "ai_voice", "AI & Voice", "prefsNavAiVoice", assistantDirty},
#endif
        {PreferencesCategory::Editing, "editing", "Editing", "prefsNavEditing", false},
        {PreferencesCategory::Shortcuts, "shortcuts", "Shortcuts", "prefsNavShortcuts", false},
        {PreferencesCategory::Annotate, "annotate", "Annotate", "prefsNavAnnotate", false},
    };
    // While filtering, a category with zero matching settings stays in place but
    // draws disabled — hiding it would make the rail jump under the cursor
    // mid-type, and leaving it clickable offers a pane that can only ever be
    // empty.
    const bool filtering = d.prefsFilter.Active();
    // Re-home the selection on every query edit (not every frame): the results
    // the user is typing towards are useless behind a category that no longer
    // matches. Between edits the selection is theirs to move.
    if (filtering && d.prefsFilter.QueryChangedThisUpdate()) {
        for (const PrefsNavEntry& e : entries) {
            if (d.prefsFilter.CategoryMatchCount(e.SchemaId) > 0) {
                d.preferencesCategory = e.Category;
                break;
            }
        }
    }
    char label[96];
    if (d.prefsNavCombo) {
        const PrefsNavEntry* current = &entries[0];
        for (const PrefsNavEntry& e : entries) {
            if (e.Category == d.preferencesCategory) {
                current = &e;
                break;
            }
        }
        // Preview text renders verbatim (no "###" id-splitting), so compose
        // the visible part only.
        char preview[96];
        std::snprintf(preview, sizeof(preview), "%s%s", current->TitleEn, current->Dirty ? " *" : "");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("###prefsNavCombo", preview)) {
            for (const PrefsNavEntry& e : entries) {
                const bool selected = e.Category == d.preferencesCategory;
                const bool empty = CategoryIsEmptyUnderFilter(d, filtering, e.SchemaId);
                if (empty) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Selectable(ComposeNavLabel(label, sizeof(label), e), selected)) {
                    d.preferencesCategory = e.Category;
                }
                if (empty) {
                    ImGui::EndDisabled();
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return;
    }
    const float railWidth = 11.0f * ImGui::GetFontSize();
    if (ImGui::BeginChild("PrefsNavRail", ImVec2(railWidth, bodyHeight))) {
        for (const PrefsNavEntry& e : entries) {
            const bool selected = e.Category == d.preferencesCategory;
            const bool empty = CategoryIsEmptyUnderFilter(d, filtering, e.SchemaId);
            if (empty) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Selectable(ComposeNavLabel(label, sizeof(label), e), selected)) {
                d.preferencesCategory = e.Category;
            }
            if (empty) {
                ImGui::EndDisabled();
            }
        }
    }
    ImGui::EndChild();
}

} // namespace SmatchetPreferencesUiDetail
