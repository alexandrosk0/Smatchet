// Preferences window shell helpers: the left category nav rail (or the
// narrow-width category combo), the collapsible PrefsSection chrome with
// per-section save-semantics hints, and the search-chip name -> category map.
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
/// visible part only.
struct PrefsNavEntry {
    PreferencesCategory Category;
    const char* TitleEn;
    const char* NavId;
    bool Dirty;
};

/// Compose "Title###navId" / "Title *###navId" into buf and return it.
const char* ComposeNavLabel(char* buf, std::size_t bufSize, const PrefsNavEntry& e) {
    std::snprintf(buf, bufSize, "%s%s###%s", e.TitleEn, e.Dirty ? " *" : "", e.NavId);
    return buf;
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
        ImGui::TextDisabled(SaveHintForSemantics(desc->Save));
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
        {PreferencesCategory::General, "General", "prefsNavGeneral", false},
        {PreferencesCategory::Appearance, "Appearance", "prefsNavAppearance", false},
        {PreferencesCategory::Tracker, "Tracker", "prefsNavTracker", trackerDirty},
        {PreferencesCategory::Connections, "Connections", "prefsNavConnections", false},
#if defined(SMATCHET_WITH_AI) || defined(SMATCHET_WITH_WHISPER)
        {PreferencesCategory::AiVoice, "AI & Voice", "prefsNavAiVoice", assistantDirty},
#endif
        {PreferencesCategory::Editing, "Editing", "prefsNavEditing", false},
        {PreferencesCategory::Shortcuts, "Shortcuts", "prefsNavShortcuts", false},
        {PreferencesCategory::Annotate, "Annotate", "prefsNavAnnotate", false},
    };
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
        std::snprintf(preview, sizeof(preview), "%s%s", current->TitleEn,
                      current->Dirty ? " *" : "");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("###prefsNavCombo", preview)) {
            for (const PrefsNavEntry& e : entries) {
                const bool selected = e.Category == d.preferencesCategory;
                if (ImGui::Selectable(ComposeNavLabel(label, sizeof(label), e), selected)) {
                    d.preferencesCategory = e.Category;
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
            if (ImGui::Selectable(ComposeNavLabel(label, sizeof(label), e), selected)) {
                d.preferencesCategory = e.Category;
            }
        }
    }
    ImGui::EndChild();
}

bool PrefsCategoryForChipName(const std::string& name, PreferencesCategory& out) {
    struct ChipRow {
        const char* Name;
        PreferencesCategory Category;
    };
    // Chip names are the pre-resegmentation tab names — kept so the legacy
    // search chips still land somewhere sane until slice 3 removes them.
    // A const name->enum table plus a linear scan matches CodeColorView.cpp's tag-alias table by
    // shape only; the two share no domain, and this whole function is transient scaffolding that
    // slice 3 deletes with the legacy chips, so a shared generic lookup would couple independent
    // subsystems for the sake of dead code.
    // SMATCHET_DEVIATION(rule=duplication; reason=shape-only, dies slice 3; owner=prefs-ia; revisit=2026-09-05)
    static const ChipRow kRows[] = {
        {"Tracker", PreferencesCategory::Tracker},
        {"User Info", PreferencesCategory::Connections},
        {"Integrations", PreferencesCategory::Connections},
        {"Assistant", PreferencesCategory::AiVoice},
        {"Whisper", PreferencesCategory::AiVoice},
        {"Local data", PreferencesCategory::General},
        {"Appearance", PreferencesCategory::Appearance},
        {"Keyboard Shortcuts", PreferencesCategory::Shortcuts},
        {"Grid", PreferencesCategory::Editing},
        {"Fields Inputs", PreferencesCategory::Editing},
        {"Annotate", PreferencesCategory::Annotate},
    };
    for (const ChipRow& row : kRows) {
        if (name == row.Name) {
            out = row.Category;
            return true;
        }
    }
    return false;
}

} // namespace SmatchetPreferencesUiDetail
