#include "PreferencesSchema.h"

#include <cstring>

// Descriptor tables for the Preferences window. Row order mirrors draw order
// within each section; LabelEn strings are verbatim copies of what the draw
// code renders (for T(key, fallback) widgets the English fallback IS LabelEn).
// Keywords seed each row with its pre-resegmentation tab name so searches like
// "integrations" or "user info" still land on relocated settings.

namespace SmatchetPrefsSchema {

namespace {

const PrefsCategoryDesc kCategories[] = {
    {"general", "General"},     {"appearance", "Appearance"}, {"tracker", "Tracker"},   {"connections", "Connections"},
#if defined(SMATCHET_WITH_AI) || defined(SMATCHET_WITH_WHISPER)
    {"ai_voice", "AI & Voice"},
#endif
    {"editing", "Editing"},     {"shortcuts", "Shortcuts"},   {"annotate", "Annotate"},
};

const PrefsSectionDesc kSections[] = {
    // -- General ------------------------------------------------------------
    {"general.updates", "general", "Updates", SaveSemantics::Immediate},
    {"general.language_region", "general", "Language & region", SaveSemantics::Immediate},
    {"general.storage", "general", "Storage", SaveSemantics::Restart},
    {"general.local_database", "general", "Local database", SaveSemantics::Immediate},
    // -- Appearance ---------------------------------------------------------
    {"appearance.theme_font", "appearance", "Theme & font", SaveSemantics::Immediate},
    {"appearance.layout_density", "appearance", "Layout & density", SaveSemantics::Immediate},
    {"appearance.display", "appearance", "Display", SaveSemantics::Immediate},
    // -- Tracker ------------------------------------------------------------
    {"tracker.backend", "tracker", "Backend & credentials", SaveSemantics::SaveAndSync},
    {"tracker.recent_projects", "tracker", "Recent projects", SaveSemantics::Immediate},
    // Not in the plan's section list — hosts the ticket-change monitor that
    // previously lived at the bottom of the Appearance tab (deviation noted in
    // the plan doc).
    {"tracker.notifications", "tracker", "Change notifications", SaveSemantics::Immediate},
// -- Connections --------------------------------------------------------
#if defined(SMATCHET_WITH_MCP)
    {"connections.mcp", "connections", "MCP server", SaveSemantics::Immediate},
#endif
    {"connections.perforce", "connections", "Perforce", SaveSemantics::AnnotateDetached},
    {"connections.activity", "connections", "Activity sources", SaveSemantics::Immediate},
// -- AI & Voice ---------------------------------------------------------
#if defined(SMATCHET_WITH_AI)
    {"ai_voice.assistant", "ai_voice", "Assistant", SaveSemantics::ExplicitSave},
    // AgentsMd* fields ride the same working-copy Save/Discard flow as the
    // Assistant section (CopyAssistantAiFields includes them).
    {"ai_voice.agent_context", "ai_voice", "Agent context", SaveSemantics::ExplicitSave},
#endif
#if defined(SMATCHET_WITH_WHISPER)
    {"ai_voice.dictation", "ai_voice", "Voice dictation", SaveSemantics::Autosave},
    {"ai_voice.diagnostics", "ai_voice", "Voice diagnostics", SaveSemantics::Autosave},
#endif
    // -- Editing ------------------------------------------------------------
    {"editing.grid", "editing", "Grid behaviour", SaveSemantics::Autosave},
    {"editing.time_estimates", "editing", "Time estimates", SaveSemantics::Autosave},
    {"editing.work_log_templates", "editing", "Work log templates", SaveSemantics::Autosave},
    {"editing.quick_comments", "editing", "Quick comments", SaveSemantics::Autosave},
    {"editing.annotate_comments", "editing", "Annotate comments", SaveSemantics::Autosave},
#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    {"editing.quick_create", "editing", "Unreal quick create", SaveSemantics::Autosave},
#endif
    // -- Shortcuts ----------------------------------------------------------
    {"shortcuts.keyboard", "shortcuts", "Keyboard shortcuts", SaveSemantics::Autosave},
    {"shortcuts.system", "shortcuts", "System shortcuts", SaveSemantics::Immediate},
    // -- Annotate -----------------------------------------------------------
    {"annotate.analysis", "annotate", "Analysis", SaveSemantics::AnnotateDetached},
    {"annotate.field_mapping", "annotate", "Tracker field mapping", SaveSemantics::AnnotateDetached},
    {"annotate.colors", "annotate", "Colors", SaveSemantics::AnnotateDetached},
};

const PrefsSettingDesc kSettings[] = {
    // -- general.updates (was: Appearance tab) ------------------------------
    {"general.updates.check_enabled", "general.updates", "Check for updates automatically",
     "appearance update upgrade version"},
    {"general.updates.include_prerelease", "general.updates", "Include prerelease builds",
     "appearance update beta preview"},
    {"general.updates.check_now", "general.updates", "Check for Updates Now", "appearance update manual"},

    // -- general.language_region (was: Appearance tab) ----------------------
    {"general.language_region.language", "general.language_region", "Language",
     "appearance locale translation ui language"},
    {"general.language_region.date_format", "general.language_region", "Date Format Style",
     "appearance date format relative absolute iso"},
    // ConditionalDraw: drawn only while "Date Format Style" is Compact (index 0).
    {"general.language_region.date_compact_threshold", "general.language_region", "Compact Relative Threshold (Days)",
     "appearance date format compact relative days", true},

    // -- general.storage (was: Local data tab) ------------------------------
    {"general.storage.mode", "general.storage", "Storage mode", "local data cache database offline sqlite"},

    // -- general.local_database (was: Local data tab) ------------------------
    {"general.local_database.recreate", "general.local_database", "Recreate database...",
     "local data cache database offline sqlite clear reset"},

    // -- appearance.theme_font ----------------------------------------------
    {"appearance.theme_font.font", "appearance.theme_font", "Application Font", "appearance font size typeface"},

    // -- appearance.layout_density ------------------------------------------
    {"appearance.layout_density.ui_mode", "appearance.layout_density", "UI mode",
     "appearance density compact zoom mobile desktop"},
    // Radio pair under the "Mobile layout" group heading; shown always (not
    // gated on UiMode), so NOT ConditionalDraw. Label check targets the heading.
    {"appearance.layout_density.mobile_density", "appearance.layout_density", "Mobile layout",
     "appearance mobile density compact comfortable touch"},
    {"appearance.layout_density.mobile_nav_pages", "appearance.layout_density", "Bottom navigation",
     "appearance mobile nav pages tabs"},
    {"appearance.layout_density.mobile_home_page", "appearance.layout_density", "Home page",
     "appearance mobile start page"},

    // -- appearance.display -------------------------------------------------
    {"appearance.display.vsync", "appearance.display", "Enable vsync", "appearance vsync frame rate refresh"},
    {"appearance.display.layout_reset_confirm", "appearance.display", "Ask before layout-resetting changes",
     "appearance layout reset confirm skip"},

    // -- tracker.backend ----------------------------------------------------
    {"tracker.backend.read_only", "tracker.backend", "Read-only mode",
     "tracker jira plane github linear read-only sync"},
    {"tracker.backend.type", "tracker.backend", "Tracker Backend",
     "tracker jira plane github linear connection backend"},
    // ConditionalDraw group: each credential row draws only while its backend
    // is the one selected in "Tracker Backend".
    // Jira:
    {"tracker.backend.jira_domain", "tracker.backend", "Domain", "tracker jira credentials atlassian url", true},
    {"tracker.backend.jira_email", "tracker.backend", "Email", "tracker jira credentials atlassian", true},
    {"tracker.backend.jira_api_token", "tracker.backend", "API Token", "tracker jira credentials token", true},
    {"tracker.backend.jira_inherit", "tracker.backend", "New issue: inherit fields from last row (Jira)",
     "tracker jira new issue inherit default", true},
    // Plane:
    {"tracker.backend.plane_url", "tracker.backend", "URL", "tracker plane credentials", true},
    {"tracker.backend.plane_workspace", "tracker.backend", "Workspace Slug", "tracker plane credentials workspace",
     true},
    {"tracker.backend.plane_api_key", "tracker.backend", "API Key", "tracker plane credentials key", true},
    {"tracker.backend.plane_inherit", "tracker.backend", "New issue: inherit fields from last row (Plane)",
     "tracker plane new issue inherit default", true},
    // GitHub Projects:
    {"tracker.backend.gh_base_url", "tracker.backend", "Base URL", "tracker github credentials enterprise", true},
    {"tracker.backend.gh_pat", "tracker.backend", "Personal Access Token", "tracker github credentials pat token",
     true},
    {"tracker.backend.gh_owner", "tracker.backend", "Owner", "tracker github credentials org", true},
    {"tracker.backend.gh_repo", "tracker.backend", "Repo", "tracker github credentials repository", true},
    {"tracker.backend.gh_project_number", "tracker.backend", "Project number", "tracker github credentials project",
     true},
    {"tracker.backend.gh_inherit", "tracker.backend", "New issue: inherit fields from last row (GitHub)",
     "tracker github new issue inherit default", true},
    // Linear:
    {"tracker.backend.linear_api_key", "tracker.backend", "API Key", "tracker linear credentials key", true},
    {"tracker.backend.linear_team_key", "tracker.backend", "Team Key", "tracker linear credentials", true},
    {"tracker.backend.linear_team", "tracker.backend", "Team", "tracker linear credentials", true},
    {"tracker.backend.linear_base_url", "tracker.backend", "Base URL", "tracker linear credentials", true},
    {"tracker.backend.linear_workspace_url", "tracker.backend", "Workspace URL", "tracker linear credentials workspace",
     true},
    {"tracker.backend.linear_inherit", "tracker.backend", "New issue: inherit fields from last row (Linear)",
     "tracker linear new issue inherit default", true},
    {"tracker.backend.test_connection", "tracker.backend", "Test connection", "tracker credentials verify check"},

    // -- tracker.recent_projects --------------------------------------------
    // One descriptor for the whole list editor; per-row widgets are not
    // individually filterable.
    {"tracker.recent_projects.list", "tracker.recent_projects", "Recently used projects",
     "tracker recent projects history"},
    // Drawn under Recent projects (that is where the button lives), so it is
    // parented there — a descriptor in a section its widget does not draw in
    // would surface the row while its section stays hidden.
    {"tracker.recent_projects.open_views_dashboard", "tracker.recent_projects", "Open Views Dashboard",
     "tracker views dashboard"},

    // -- tracker.notifications (was: Appearance tab) -------------------------
    {"tracker.notifications.enabled", "tracker.notifications", "Notify me when tracked issues change",
     "appearance ticket monitor notify watch"},
    // Wrapped in BeginDisabled while the toggle above is off — still drawn, so
    // NOT ConditionalDraw.
    {"tracker.notifications.interval", "tracker.notifications", "Check interval (seconds)",
     "appearance ticket monitor poll interval"},

#if defined(SMATCHET_WITH_MCP)
    // -- connections.mcp (was: Integrations tab) -----------------------------
    {"connections.mcp.enabled", "connections.mcp", "Enable MCP server",
     "integrations mcp model context protocol server automation agent"},
    {"connections.mcp.port", "connections.mcp", "MCP Port", "integrations mcp server port"},
    {"connections.mcp.allow_remote", "connections.mcp", "Bind on all interfaces (LAN)",
     "integrations mcp remote lan bind network"},
    {"connections.mcp.auth_token", "connections.mcp", "MCP auth token (optional)",
     "integrations mcp auth token security"},
    {"connections.mcp.allow_lua", "connections.mcp", "Allow MCP run_lua tool (dangerous)",
     "integrations mcp lua automation script"},
#endif

    // -- connections.perforce (was: Annotate tab) ----------------------------
    {"connections.perforce.p4_exe", "connections.perforce", "p4 executable", "annotate perforce p4 path"},
    {"connections.perforce.p4vc_exe", "connections.perforce", "p4vc executable", "annotate perforce p4vc path"},
    {"connections.perforce.timelapse_cmd", "connections.perforce", "Timelapse command (optional)",
     "annotate perforce timelapse"},
    {"connections.perforce.change_cmd", "connections.perforce", "Changelist command (optional)",
     "annotate perforce changelist"},
    {"connections.perforce.path_remaps", "connections.perforce", "Path remaps", "annotate perforce path remap depot"},

    // -- connections.activity (was: User Info tab) ---------------------------
    {"connections.activity.git_repos", "connections.activity", "Git commit repos",
     "user info git commit repos activity feed vcs"},
    {"connections.activity.production_keyword", "connections.activity", "Production group keyword",
     "user info production group"},
    {"connections.activity.day_window", "connections.activity", "Activity day window", "user info activity feed days"},
    {"connections.activity.max_changes", "connections.activity", "Max changes per source",
     "user info activity feed changes limit"},
    {"connections.activity.vcs_layout", "connections.activity", "VCS feed layout",
     "user info activity feed vcs layout"},

#if defined(SMATCHET_WITH_AI)
    // -- ai_voice.assistant (was: Assistant tab) -----------------------------
    {"ai_voice.assistant.provider", "ai_voice.assistant", "AI provider",
     "assistant ai openai anthropic claude ollama deepseek chat"},
    {"ai_voice.assistant.test_connection", "ai_voice.assistant", "Test connection", "assistant ai verify check"},
    // ConditionalDraw group: provider-specific rows draw only while their
    // provider is selected in "AI provider".
    {"ai_voice.assistant.openai_api_key", "ai_voice.assistant", "OpenAI API key", "assistant ai openai credentials key",
     true},
    {"ai_voice.assistant.model_openai", "ai_voice.assistant", "OpenAI model", "assistant ai openai gpt", true},
    // One descriptor: OpenAI and Anthropic share the same custom-endpoint
    // buffer (b.baseUrlBuf), so their "Base URL" rows are one value.
    {"ai_voice.assistant.base_url", "ai_voice.assistant", "Base URL", "assistant ai endpoint proxy gateway custom",
     true},
    {"ai_voice.assistant.allow_custom_openai", "ai_voice.assistant", "Allow custom endpoint (proxy / gateway)",
     "assistant ai openai endpoint proxy", true},
    {"ai_voice.assistant.anthropic_api_key", "ai_voice.assistant", "Anthropic API key",
     "assistant ai anthropic claude credentials key", true},
    {"ai_voice.assistant.model_anthropic", "ai_voice.assistant", "Anthropic model", "assistant ai anthropic claude",
     true},
    {"ai_voice.assistant.allow_custom_anthropic", "ai_voice.assistant", "Allow custom endpoint (proxy / gateway)",
     "assistant ai anthropic endpoint proxy", true},
    {"ai_voice.assistant.model_ollama", "ai_voice.assistant", "Ollama model", "assistant ai ollama local", true},
    {"ai_voice.assistant.ollama_base_url", "ai_voice.assistant", "Ollama base URL",
     "assistant ai ollama endpoint local", true},
    {"ai_voice.assistant.deepseek_api_key", "ai_voice.assistant", "DeepSeek API key",
     "assistant ai deepseek credentials key", true},
    {"ai_voice.assistant.model_deepseek", "ai_voice.assistant", "DeepSeek model", "assistant ai deepseek", true},
    // DeepSeek's endpoint row has its own buffer, distinct from the shared
    // OpenAI/Anthropic one above.
    {"ai_voice.assistant.deepseek_base_url", "ai_voice.assistant", "Base URL", "assistant ai deepseek endpoint", true},
    {"ai_voice.assistant.reasoning_effort", "ai_voice.assistant", "Default reasoning effort",
     "assistant ai reasoning effort thinking"},

    // -- ai_voice.agent_context (was: Assistant tab) -------------------------
    {"ai_voice.agent_context.global_path", "ai_voice.agent_context", "Global agents.md path",
     "assistant ai agents.md harness context"},
    {"ai_voice.agent_context.project_path", "ai_voice.agent_context", "Project agents.md path (override)",
     "assistant ai agents.md harness context project"},
    {"ai_voice.agent_context.auto_discover", "ai_voice.agent_context",
     "Auto-discover project agents.md (walk up from cwd)", "assistant ai agents.md harness discover"},
#endif

#if defined(SMATCHET_WITH_WHISPER)
    // -- ai_voice.dictation (was: Whisper tab) -------------------------------
    {"ai_voice.dictation.enabled", "ai_voice.dictation", "Enable voice dictation",
     "whisper voice dictation speech microphone"},
    {"ai_voice.dictation.mode", "ai_voice.dictation", "Mode", "whisper voice dictation local cloud"},
    {"ai_voice.dictation.model", "ai_voice.dictation", "Speech model", "whisper voice dictation model"},
    // ConditionalDraw: safe-true — appears only while the selected local model
    // is not yet downloaded.
    {"ai_voice.dictation.download_model", "ai_voice.dictation", "Download model",
     "whisper voice dictation model download", true},
    {"ai_voice.dictation.hotkey", "ai_voice.dictation", "Push-to-talk hotkey",
     "whisper voice dictation push to talk hotkey recording"},
    {"ai_voice.dictation.api_key", "ai_voice.dictation", "OpenAI API key (cloud mode)",
     "whisper voice dictation cloud credentials key"},
    {"ai_voice.dictation.language", "ai_voice.dictation", "Language:", "whisper voice dictation transcribe language"},
    {"ai_voice.dictation.trim", "ai_voice.dictation", "Trim leading/trailing silence",
     "whisper voice dictation audio silence"},
    {"ai_voice.dictation.max_clip", "ai_voice.dictation",
     "Max clip length:", "whisper voice dictation audio recording length"},
    {"ai_voice.dictation.auto_send", "ai_voice.dictation", "Auto-send AI chat on punctuation (\".\", \"!\", \"?\")",
     "whisper voice dictation auto send chat punctuation"},
    // ConditionalDraw: safe-true — icon button appears only after the setup
    // banner has been dismissed.
    {"ai_voice.dictation.rerun_setup", "ai_voice.dictation", "Re-run setup banner",
     "whisper voice dictation setup wizard", true},

    // -- ai_voice.diagnostics (was: Whisper tab) -----------------------------
    {"ai_voice.diagnostics.test_connection", "ai_voice.diagnostics", "Test connection",
     "whisper voice dictation verify check"},
    {"ai_voice.diagnostics.test_microphone", "ai_voice.diagnostics", "Test microphone (3 s)",
     "whisper voice dictation microphone audio"},
    {"ai_voice.diagnostics.test_e2e", "ai_voice.diagnostics", "Test end-to-end (capture 4 s + transcribe)",
     "whisper voice dictation transcribe microphone audio"},
#endif

    // -- editing.grid (was: Grid + Appearance tabs) --------------------------
    {"editing.grid.single_click", "editing.grid", "Single-click to edit grid cells", "grid rows editing click"},
    {"editing.grid.long_text_preview", "editing.grid", "Open long-text editor in preview mode",
     "grid editing markdown preview"},
    {"editing.grid.overflow_tooltips", "editing.grid", "Show tooltips when text overflows",
     "appearance grid tooltip overflow"},
    {"editing.grid.wheel_ticks", "editing.grid", "Wheel ticks before horizontal scroll",
     "appearance grid wheel scroll horizontal"},

    // -- editing.time_estimates (was: Fields Inputs tab) ---------------------
    {"editing.time_estimates.duration_suggestions", "editing.time_estimates", "Duration Suggestions",
     "fields inputs time estimate duration suggestions"},

    // -- editing.work_log_templates (was: Fields Inputs tab) -----------------
    {"editing.work_log_templates.list", "editing.work_log_templates", "Work Log Description Templates",
     "fields inputs worklog work log template"},

    // -- editing.quick_comments (was: Fields Inputs tab) ---------------------
    {"editing.quick_comments.list", "editing.quick_comments", "Grid Right-Click Quick Comments",
     "fields inputs quick comment grid right-click"},

    // -- editing.annotate_comments (was: Fields Inputs tab) ------------------
    {"editing.annotate_comments.list", "editing.annotate_comments", "Annotate Quick Comments",
     "fields inputs annotate quick comment"},

#if defined(SMATCHET_EMBEDDED_IN_UNREAL)
    // -- editing.quick_create (was: Unreal tab) ------------------------------
    {"editing.quick_create.engine_version", "editing.quick_create", "Engine version", "unreal quick create context"},
    {"editing.quick_create.project", "editing.quick_create", "Project name and directory",
     "unreal quick create context"},
    {"editing.quick_create.platform", "editing.quick_create", "Platform, OS, and build configuration",
     "unreal quick create context"},
    {"editing.quick_create.level", "editing.quick_create", "Current level / map", "unreal quick create context"},
    {"editing.quick_create.pie_state", "editing.quick_create", "Play-in-editor (PIE) state",
     "unreal quick create context pie"},
    {"editing.quick_create.selected_actors", "editing.quick_create", "Selected actors", "unreal quick create context"},
    {"editing.quick_create.log_tail", "editing.quick_create", "Output Log tail", "unreal quick create context log"},
    // ConditionalDraw: drawn only while "Output Log tail" is enabled
    // (d.cfg.QuickCreateCtxLogTail).
    {"editing.quick_create.log_lines", "editing.quick_create", "Log lines", "unreal quick create context log lines",
     true},
#endif

    // -- shortcuts.keyboard --------------------------------------------------
    // One descriptor for the whole bindings table; rows are dynamic command
    // entries, so the drift guard's label check exempts this id.
    {"shortcuts.keyboard.bindings", "shortcuts.keyboard", "Keyboard shortcuts",
     "keyboard shortcut hotkey binding chord rebind keys keybindings reset add command"},

    // -- shortcuts.system ----------------------------------------------------
    {"shortcuts.system.list", "shortcuts.system", "System shortcuts (not rebindable)",
     "keyboard shortcut hotkey system fixed"},

    // -- annotate.analysis ---------------------------------------------------
    {"annotate.analysis.max_frames", "annotate.analysis", "Max frames", "annotate perforce p4 blame callstack"},
    {"annotate.analysis.ignore_keywords", "annotate.analysis", "Ignore keywords (comma or newline)",
     "annotate perforce filter"},
    // Kept under Annotate (not AI & Voice): this URL feeds the annotate
    // workflow's "open AI chat" hand-off, not the in-app assistant.
    {"annotate.analysis.ai_chat_url", "annotate.analysis", "AI chat URL (optional)", "annotate perforce ai chat link"},
    {"annotate.analysis.cache_size", "annotate.analysis", "Changelist cache size",
     "annotate perforce changelist cache"},

    // -- annotate.field_mapping ----------------------------------------------
    {"annotate.field_mapping.callstack_field", "annotate.field_mapping", "Callstack source field",
     "annotate tracker field callstack"},
    {"annotate.field_mapping.before_cl_field", "annotate.field_mapping", "Before-changelist field",
     "annotate tracker field changelist"},
    {"annotate.field_mapping.last_occurrences_field", "annotate.field_mapping", "Last-occurrences date field",
     "annotate tracker field date"},

    // -- annotate.colors -----------------------------------------------------
    {"annotate.colors.status_info", "annotate.colors", "Status info", "annotate color theme"},
    {"annotate.colors.status_error", "annotate.colors", "Status error", "annotate color theme"},
    {"annotate.colors.status_warning", "annotate.colors", "Status warning", "annotate color theme"},
    {"annotate.colors.find_highlight", "annotate.colors", "Find highlight", "annotate color theme search"},
    {"annotate.colors.text_disabled", "annotate.colors", "Text disabled", "annotate color theme"},
    {"annotate.colors.import_existing", "annotate.colors", "Import existing", "annotate color theme"},
    {"annotate.colors.cl_tooltip_title", "annotate.colors", "CL tooltip title",
     "annotate color theme changelist tooltip"},
};

} // namespace

const PrefsCategoryDesc* Categories(std::size_t& count) {
    count = sizeof(kCategories) / sizeof(kCategories[0]);
    return kCategories;
}

const PrefsSectionDesc* Sections(std::size_t& count) {
    count = sizeof(kSections) / sizeof(kSections[0]);
    return kSections;
}

const PrefsSettingDesc* Settings(std::size_t& count) {
    count = sizeof(kSettings) / sizeof(kSettings[0]);
    return kSettings;
}

const PrefsCategoryDesc* FindCategory(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const PrefsCategoryDesc& c : kCategories) {
        if (std::strcmp(c.Id, id) == 0) {
            return &c;
        }
    }
    return nullptr;
}

const PrefsSectionDesc* FindSection(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const PrefsSectionDesc& s : kSections) {
        if (std::strcmp(s.Id, id) == 0) {
            return &s;
        }
    }
    return nullptr;
}

const PrefsSettingDesc* FindSetting(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const PrefsSettingDesc& s : kSettings) {
        if (std::strcmp(s.Id, id) == 0) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace SmatchetPrefsSchema
