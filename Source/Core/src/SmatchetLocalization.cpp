#include "SmatchetLocalization.h"

#include "ConfigManager.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct TranslationEntry {
    const char* Key;
    const char* English;
    const char* French;
};

const TranslationEntry kEntries[] = {
    {"common.save", "Save", u8"Enregistrer"},
    {"common.save_sync", "Save & Sync", u8"Enregistrer et synchroniser"},
    {"common.cancel", "Cancel", u8"Annuler"},
    {"common.close", "Close", u8"Fermer"},
    {"common.clear", "Clear", u8"Effacer"},
    {"common.apply", "Apply", u8"Appliquer"},
    {"common.refresh", "Refresh", u8"Actualiser"},
    {"common.reload", "Reload", u8"Recharger"},
    {"common.copy", "Copy", u8"Copier"},
    {"common.create", "Create", u8"Créer"},
    {"common.delete", "Delete", u8"Supprimer"},
    {"common.add", "Add", u8"Ajouter"},
    {"common.loading", "Loading...", u8"Chargement..."},
    {"common.loading_ellipsis", "(loading...)", u8"(chargement...)"},
    {"common.none", "(none)", u8"(aucun)"},
    {"common.unset", "(unset)", u8"(non défini)"},
    {"common.new", "new", u8"nouveau"},
    {"common.ok", "OK", u8"OK"},
    {"common.fail", "FAIL", u8"ÉCHEC"},
    {"common.no_changes", "no changes", u8"aucun changement"},
    {"common.not_in_cache", "not in cache", u8"absent du cache"},

    {"toast.tracker", "Tracker", u8"Suivi"},
    {"toast.active_project", "Active Project", u8"Projet actif"},
    {"toast.lua_action", "Lua Action", u8"Action Lua"},
    {"toast.comment_posted", "Comment Posted", u8"Commentaire publié"},
    {"toast.comment_failed", "Comment Failed", u8"Échec du commentaire"},
    {"toast.queued_offline", "Queued Offline", u8"Mis en file hors ligne"},

    // remove-global-project-key.md: badge for dead-letter rows whose draft
    // was missing a project after the legacy-project sweep.
    {"offlineQueue.badge.missingProject", "missing project", u8"projet manquant"},
    {"offlineQueue.badge.missingProject.tooltip",
     "This queued create was missing a project after the project-key migration. Restore and pick a project to retry.",
     u8"Cette création en file d'attente n'avait pas de projet après la migration de la clé de projet. "
     u8"Restaurez-la et choisissez un projet pour réessayer."},

    {"toast.success", "Success", u8"Succès"},
    {"toast.import_error", "Import Error", u8"Erreur d'import"},
    {"toast.sync_failed", "Sync Failed", u8"Échec de la synchronisation"},
    {"toast.sync_warning", "Sync Warning", u8"Avertissement de synchronisation"},
    {"toast.sync_complete", "Sync Complete", u8"Synchronisation terminée"},
    {"toast.syncing", "Syncing", u8"Synchronisation"},
    {"toast.field_update_saved", "Field update saved to Tracker.", u8"Mise à jour du champ enregistrée dans le suivi."},
    {"toast.refreshing_issues", "Refreshing issues from Tracker...", u8"Actualisation des tickets depuis le suivi..."},
    {"toast.failed_jira_comment", "Failed to post Jira comment.", u8"Impossible de publier le commentaire Jira."},
    {"toast.tracker_reachable_required",
     "Grid edits and quick comment actions stay disabled until Tracker is reachable.",
     u8"Les modifications de grille et les commentaires rapides restent désactivés tant que le suivi n'est pas "
     u8"joignable."},

    {"menu.settings", "Settings", u8"Paramètres"},
    {"menu.workspace", "Workspace", u8"Espace de travail"},
    {"menu.grid", "Grid", u8"Grille"},
    {"menu.views_queries", "Views & Queries...", u8"Vues et requêtes..."},
    {"menu.reset_workspace_layout", "Reset Workspace Layout", u8"Réinitialiser la disposition"},
    {"menu.selection", "Selection", u8"Sélection"},
    {"menu.select_all", "Select All", u8"Tout sélectionner"},
    {"menu.clear_selection", "Clear Selection", u8"Effacer la sélection"},
    {"menu.copy_selection", "Copy Selection", u8"Copier la sélection"},
    {"menu.issues", "Issues", u8"Tickets"},
    {"menu.import_issues", "Import Issues...", u8"Importer des tickets..."},
    {"menu.export_issues", "Export Issues...", u8"Exporter des tickets..."},
    {"menu.automation", "Automation", u8"Automatisation"},
    {"menu.scripts_actions", "Scripts & Actions...", u8"Scripts et actions..."},
    {"menu.agent_bridge_mcp", "Agent Bridge (MCP)...", u8"Passerelle agent (MCP)..."},
    {"menu.inspect", "Inspect", u8"Inspection"},
    {"menu.source_annotate", "Annotate...", u8"Annoter..."},
    {"menu.sync_audit", "Sync Audit...", u8"Audit de synchronisation..."},
    {"menu.runtime_log", "Runtime Log", u8"Journal d'exécution"},
    {"menu.performance_monitor", "Performance Monitor...", u8"Moniteur de performances..."},
    {"menu.read_only_mode", "Read-only Mode", u8"Mode lecture seule"},
    {"menu.preferences", "Preferences...", u8"Préférences..."},
    {"menu.performance", "Performance...", u8"Performances..."},
    {"menu.windows", "Windows", u8"Fenêtres"},
    {"menu.open_views", "Open Views...", u8"Ouvrir les vues..."},
    {"menu.annotate_analysis", "Annotate...", u8"Annoter..."},
    {"menu.show_log", "Show Log", u8"Afficher le journal"},
    {"menu.backend_audit", "Backend audit...", u8"Audit du backend..."},
    {"menu.scripting", "Scripting...", u8"Scripts..."},
    {"menu.mcp_server", "MCP Server...", u8"Serveur MCP..."},
    {"menu.bulk", "Bulk", u8"Lot"},
    {"menu.bulk_import", "Bulk import...", u8"Import en lot..."},
    {"menu.bulk_export", "Bulk export...", u8"Export en lot..."},

    {"window.preferences", "Preferences", u8"Préférences"},
    {"window.views", "Views", u8"Vues"},
    {"window.views_backend", "Views - %s", u8"Vues - %s"},
    {"window.active_project", "Smatchet - Active Project", u8"Smatchet - Projet actif"},
    {"window.bulk_import", "Bulk import tickets", u8"Importer des tickets en lot"},
    {"window.bulk_export", "Bulk export tickets", u8"Exporter des tickets en lot"},
    {"window.backend_audit", "Backend Audit", u8"Audit du backend"},
    {"window.attachment_preview", "Attachment Preview", u8"Aperçu des pièces jointes"},
    {"window.log", "Log", u8"Journal"},
    {"window.watchers", "Watchers", u8"Observateurs"},
    {"window.votes", "Votes", u8"Votes"},
    {"window.mcp_server", "MCP Server", u8"Serveur MCP"},

    {"prefs.tab.tracker", "Tracker", u8"Suivi"},
    {"prefs.tab.integrations", "Integrations", u8"Intégrations"},
    {"prefs.tab.appearance", "Appearance", u8"Apparence"},
    {"prefs.tab.fields_inputs", "Fields Inputs", u8"Saisie des champs"},
    {"prefs.tab.annotate_analysis", "Annotate", u8"Annoter"},
    {"prefs.tab.local_data", "Local data", u8"Données locales"},
    {"prefs.tab.keybindings", "Keyboard Shortcuts", u8"Raccourcis clavier"},

    // Preferences "Keyboard Shortcuts" tab + the shared capture widget + quick-bind popup.
    // See SmatchetPreferencesUi_Keybindings.cpp / SmatchetHotkeyCapture.cpp and
    // docs/plans/shipped/keyboard-shortcuts-rebindable.md.
    {"keybindings.editor.intro",
     "Rebind in-app keyboard shortcuts. Click a shortcut to capture a new key combo (Esc cancels). "
     "Changes save automatically.",
     u8"Réattribuez les raccourcis clavier de l'application. Cliquez sur un raccourci pour capturer une "
     u8"nouvelle combinaison de touches (Échap annule). Les changements sont enregistrés automatiquement."},
    {"keybindings.editor.searchHint", "Filter shortcuts...", u8"Filtrer les raccourcis..."},
    {"keybindings.editor.colCommand", "Action", u8"Action"},
    {"keybindings.editor.colShortcut", "Shortcut", u8"Raccourci"},
    {"keybindings.editor.colActions", "", ""},
    {"keybindings.editor.conflict", "conflicts with", u8"en conflit avec"},
    {"keybindings.editor.enabled", "On", u8"Activé"},
    {"keybindings.editor.clear", "Clear", u8"Effacer"},
    {"keybindings.editor.resetDefaults", "Reset all to defaults", u8"Tout réinitialiser par défaut"},
    {"keybindings.editor.addCommand", "Add shortcut for a command...", u8"Ajouter un raccourci pour une commande..."},
    {"keybindings.editor.addSearchHint", "Search commands...", u8"Rechercher des commandes..."},
    {"keybindings.editor.addNoneLeft", "All commands already have a shortcut row.",
     u8"Toutes les commandes ont déjà une ligne de raccourci."},
    {"keybindings.editor.systemHeader", "System shortcuts (not rebindable)", u8"Raccourcis système (non modifiables)"},
    {"keybindings.editor.unbound", "(unbound)", u8"(non défini)"},
    {"keybindings.editor.rebindButton", "Click to rebind", u8"Cliquer pour modifier"},
    {"keybindings.editor.capturing", "Press a key combo... (Esc to cancel)",
     u8"Appuyez sur une combinaison... (Échap pour annuler)"},
    {"keybindings.system.zenToggle", "Toggle Zen mode", u8"Basculer le mode Zen"},
    {"keybindings.system.zenExit", "Exit Zen mode", u8"Quitter le mode Zen"},
    {"keybindings.system.paletteNav", "Command palette navigation", u8"Navigation dans la palette de commandes"},
    {"keybindings.quickbind.title", "Set shortcut", u8"Définir le raccourci"},
    {"keybindings.quickbind.forCommand", "Shortcut for:", u8"Raccourci pour :"},
    {"keybindings.quickbind.conflict", "Already bound to:", u8"Déjà attribué à :"},
    {"keybindings.quickbind.set", "Set", u8"Définir"},
    {"keybindings.quickbind.clear", "Clear", u8"Effacer"},
    {"keybindings.quickbind.cancel", "Cancel", u8"Annuler"},
    {"prefs.local_data.help",
     "Stored tickets, offline create queues, and pending field edits live in a local SQLite file. "
     "Recreating it clears that data only; tracker credentials and views are not removed. A full "
     "issue refresh runs afterward.",
     u8"Les tickets en cache, les files de création hors ligne et les modifications de champs en "
     u8"attente sont stockés dans un fichier SQLite local. Le recréer efface uniquement ces données "
     u8"— pas les identifiants du suivi ni les vues. Une actualisation complète des tickets suit."},
    {"prefs.local_data.file_label", "Cache file:", u8"Fichier de cache :"},
    {"prefs.local_data.recreate", "Recreate database...", u8"Recréer la base..."},
    {"prefs.local_data.recreate_tooltip", "Permanently delete the local cache file and start with an empty database.",
     u8"Supprime définitivement le fichier de cache local et repart d'une base vide."},
    {"prefs.local_data.confirm_title", "Delete local database?", u8"Supprimer la base locale ?"},
    {"prefs.local_data.confirm_body",
     "This removes cached issues and any queued offline writes stored on this machine. It does not "
     "delete anything on the tracker. Continue?",
     u8"Cela supprime les tickets mis en cache et les écritures hors ligne en file sur cet ordinateur. "
     u8"Rien n'est supprimé sur le suivi. Continuer ?"},
    {"prefs.local_data.confirm_go", "Delete and recreate", u8"Supprimer et recréer"},
    {"toast.local_db_recreated", "Local database recreated; refreshing issues.",
     u8"Base locale recréée ; actualisation des tickets."},
    {"toast.local_db_error_title", "Local database", u8"Base locale"},
    {"toast.local_db_recreate_failed_detail", "Could not recreate the local database.",
     u8"Impossible de recréer la base locale."},
    {"prefs.backend_selection", "Backend Selection", u8"Sélection du backend"},
    {"prefs.read_only_mode", "Read-only mode", u8"Mode lecture seule"},
    {"prefs.tracker_backend", "Tracker Backend", u8"Backend de suivi"},
    {"prefs.jira_config", "Jira Configuration (Atlassian Cloud)", u8"Configuration Jira (Atlassian Cloud)"},
    {"prefs.plane_config", "Plane Configuration (plane.so)", u8"Configuration Plane (plane.so)"},
    {"prefs.domain", "Domain", u8"Domaine"},
    {"prefs.email", "Email", u8"E-mail"},
    {"prefs.api_token", "API Token", u8"Jeton d'API"},
    {"prefs.url", "URL", u8"URL"},
    {"prefs.workspace_slug", "Workspace Slug", u8"Slug de l'espace de travail"},
    // "Recently used projects" section in Preferences (replaces the deleted rows).
    {"prefs.recentProjects", "Recently used projects", u8"Projets récemment utilisés"},
    {"prefs.recentProjects.forget", "Forget", u8"Oublier"},
    {"prefs.recentProjects.empty", "(none yet)", u8"(aucun pour l'instant)"},
    {"prefs.api_key", "API Key", u8"Clé d'API"},
    {"prefs.new_issue_inherit_jira", "New issue: inherit fields from last row (Jira)",
     u8"Nouveau ticket : hériter des champs de la dernière ligne (Jira)"},
    {"prefs.new_issue_inherit_plane", "New issue: inherit fields from last row (Plane)",
     u8"Nouveau ticket : hériter des champs de la dernière ligne (Plane)"},
    {"prefs.views_note", "Query/JQL and column fields are configured in the Views dashboard.",
     u8"La requête/JQL et les colonnes se configurent dans le tableau de bord des vues."},
    {"prefs.open_views_dashboard", "Open Views Dashboard", u8"Ouvrir le tableau des vues"},
    {"prefs.model", "Model", u8"Modèle"},
    {"prefs.base_url", "Base URL", u8"URL de base"},
    {"prefs.mcp_header", "MCP (Model Context Protocol)", u8"MCP (Model Context Protocol)"},
    {"prefs.enable_mcp", "Enable MCP server", u8"Activer le serveur MCP"},
    {"prefs.mcp_port", "MCP Port", u8"Port MCP"},
    {"prefs.mcp_bind_lan", "Bind on all interfaces (LAN)", u8"Écouter sur toutes les interfaces (LAN)"},
    {"prefs.mcp_auth_token", "MCP auth token (optional)", u8"Jeton d'authentification MCP (facultatif)"},
    {"prefs.mcp_allow_lua", "Allow MCP run_lua tool (dangerous)", u8"Autoriser l'outil MCP run_lua (dangereux)"},
    {"prefs.mcp_saved", "MCP settings saved to disk.", u8"Paramètres MCP enregistrés sur le disque."},
    {"prefs.typography", "Application Typography", u8"Typographie de l'application"},
    {"prefs.font", "Application Font", u8"Police de l'application"},
    {"prefs.language", "Language", u8"Langue"},
    {"language.en_us", "English", u8"Anglais"},
    {"language.fr_fr", "French", u8"Français"},
    {"language.fr_native", "Français", u8"Français"},
    {"prefs.grid_text", "Grid and field text", u8"Grille et texte des champs"},
    {"prefs.field_overflow_tooltips", "Show tooltips when text overflows",
     u8"Afficher une infobulle quand le texte déborde"},
    {"prefs.wheel_ticks", "Wheel ticks before horizontal scroll", u8"Crans de molette avant le défilement horizontal"},
    {"prefs.date_formatting", "Date Formatting", u8"Format des dates"},
    {"prefs.date_format_style", "Date Format Style", u8"Style de format de date"},
    {"prefs.date_relative_compact", "Relative / Compact", u8"Relatif / compact"},
    {"prefs.date_always_relative", "Always Relative", u8"Toujours relatif"},
    {"prefs.date_absolute_iso", "Absolute ISO", u8"ISO absolu"},
    {"prefs.date_absolute_friendly", "Absolute Friendly", u8"Absolu lisible"},
    {"prefs.compact_threshold", "Compact Relative Threshold (Days)", u8"Seuil relatif compact (jours)"},
    {"prefs.duration_suggestions", "Duration Suggestions", u8"Suggestions de durée"},
    {"prefs.time_estimates", "Time Estimates", u8"Estimations de temps"},
    {"prefs.work_log_templates", "Work Log Templates", u8"Modèles de journal de travail"},
    {"prefs.quick_comments", "Quick Comments", u8"Commentaires rapides"},
    {"prefs.annotate_comments", "Annotate Comments", u8"Commentaires d’annotation"},
    {"prefs.current_suggestions", "Current Suggestions:", u8"Suggestions actuelles :"},
    {"prefs.add_option", "Add Option", u8"Ajouter une option"},
    {"prefs.add_template", "Add Template", u8"Ajouter un modèle"},
    {"prefs.add_new_template", "+ Add New Template", u8"+ Ajouter un modèle"},
    {"prefs.edit_template", "Edit Selected Template details:", u8"Modifier les détails du modèle sélectionné :"},
    {"prefs.select_template", "Select a template above to view or edit its details.",
     u8"Sélectionnez un modèle ci-dessus pour l'afficher ou le modifier."},
    {"prefs.title", "Title:", u8"Titre :"},
    {"prefs.id", "ID:", u8"ID :"},
    {"prefs.body", "Body:", u8"Corps :"},

    // Assistant custom-endpoint consent modal (short visible line + (?) help body).
    {"prefs.assistant.endpoint_consent.short",
     "A custom endpoint can receive your %s API key - possibly in cleartext. Enable only if you trust it.",
     u8"Un point d'accès personnalisé peut recevoir votre clé d'API %s — parfois en clair. "
     u8"N'activez que si vous lui faites confiance."},
    {"prefs.assistant.endpoint_consent.help",
     "By default Smatchet sends your %s API key only to %s over HTTPS. Enabling custom "
     "endpoints lets a proxy / gateway host (Azure OpenAI, LiteLLM, openrouter) receive "
     "that key, and permits plain http:// to non-loopback hosts - sending the key in "
     "cleartext. Enable only if you trust the endpoint you configure.",
     u8"Par défaut, Smatchet n'envoie votre clé d'API %s qu'à %s via HTTPS. Autoriser un point "
     u8"d'accès personnalisé permet à un hôte proxy / passerelle (Azure OpenAI, LiteLLM, "
     u8"openrouter) de recevoir cette clé, et autorise le http:// en clair vers des hôtes non "
     u8"locaux — la clé transite alors en clair. N'activez que si vous faites confiance au point "
     u8"d'accès configuré."},

    // Assistant tab — shortened tooltips + (?) help bodies.
    {"prefs.assistant.reasoning_effort.short", "OpenAI `reasoning_effort` request parameter.",
     u8"Paramètre de requête OpenAI « reasoning_effort »."},
    {"prefs.assistant.reasoning_effort.help",
     "OpenAI `reasoning_effort` body parameter for o-series / reasoning-tuned "
     "models. LM Studio + LocalAI pass it through to local reasoning models "
     "(Qwen3, gemma-3, etc.). Providers that don't understand the parameter "
     "ignore it.",
     u8"Paramètre de corps OpenAI « reasoning_effort » pour les modèles de raisonnement "
     u8"(série o). LM Studio + LocalAI le transmettent aux modèles locaux (Qwen3, gemma-3, "
     u8"etc.). Les fournisseurs qui ne le comprennent pas l'ignorent."},
    {"prefs.assistant.agents_md.help",
     "Layered system prompt injected into every Assistant turn. Global layer "
     "defaults to %LOCALAPPDATA%/Smatchet/agents.md when blank. Each layer "
     "capped at 64 KB.",
     u8"Invite système en couches injectée à chaque tour de l'Assistant. La couche globale "
     u8"vaut %LOCALAPPDATA%/Smatchet/agents.md si vide. Chaque couche est limitée à 64 Ko."},
    {"prefs.assistant.agents_md_global.short", "Override the global agents.md location.",
     u8"Remplace l'emplacement global d'agents.md."},
    {"prefs.assistant.agents_md_global.help",
     "Default %LOCALAPPDATA%/Smatchet/agents.md when blank. Override to point "
     "at a checked-in shared file.",
     u8"Par défaut %LOCALAPPDATA%/Smatchet/agents.md si vide. Remplacez pour pointer vers un "
     u8"fichier partagé versionné."},
    {"prefs.assistant.agents_md_project.short", "Explicit project-layer path.",
     u8"Chemin explicite de la couche projet."},
    {"prefs.assistant.agents_md_project.help",
     "When set, this exact path is used as the project layer. Leave blank to "
     "disable the project layer entirely unless Auto-discover is enabled below.",
     u8"Si renseigné, ce chemin exact sert de couche projet. Laissez vide pour désactiver la "
     u8"couche projet, sauf si la découverte automatique est activée ci-dessous."},
    {"prefs.assistant.agents_md_autodiscover.short", "Walk up from cwd looking for agents.md.",
     u8"Remonte depuis le répertoire courant à la recherche d'agents.md."},
    {"prefs.assistant.agents_md_autodiscover.help",
     "OFF (default): only the Global file + explicit Project path are used. ON: "
     "walks up the cwd chain looking for agents.md / AGENTS.md.",
     u8"OFF (défaut) : seuls le fichier global et le chemin projet explicite sont utilisés. "
     u8"ON : remonte la chaîne de répertoires à la recherche d'agents.md / AGENTS.md."},
    {"prefs.assistant.saved_title", "Assistant settings", u8"Paramètres de l'assistant"},
    {"prefs.assistant.saved_body", "Saved to disk.", u8"Enregistré sur le disque."},
    {"prefs.assistant.unsaved", "Unsaved changes", u8"Modifications non enregistrées"},
    {"prefs.assistant.no_changes", "No unsaved changes", u8"Aucune modification non enregistrée"},

    // Whisper tab — shortened PTT intro + (?) help body.
    {"prefs.whisper.ptt.short", "Push-to-talk dictation: hold the hotkey, speak, release.",
     u8"Dictée en appui-pour-parler : maintenez le raccourci, parlez, relâchez."},
    {"prefs.whisper.ptt.help",
     "Push-to-talk dictation. Hold the configured hotkey, speak, release. "
     "Transcription runs locally when a Whisper model is on disk; falls back "
     "to OpenAI Whisper API when no model is present (cloud mode requires an "
     "API key).",
     u8"Dictée en appui-pour-parler. Maintenez le raccourci configuré, parlez, relâchez. La "
     u8"transcription s'exécute localement quand un modèle Whisper est sur disque ; bascule "
     u8"vers l'API OpenAI Whisper sinon (le mode cloud exige une clé d'API)."},

    // Local-data tab — shortened intro/storage text + (?) help bodies.
    {"prefs.local_data.recreate_intro.short", "Local SQLite cache: tickets, offline queues, pending edits.",
     u8"Cache SQLite local : tickets, files hors-ligne, modifications en attente."},
    {"prefs.local_data.recreate_intro.help",
     "Stored tickets, offline create queues, and pending field edits live in a "
     "local SQLite file. Recreating it clears that data only; tracker credentials "
     "and views are not removed. A full issue refresh runs afterward.",
     u8"Les tickets stockés, les files de création hors-ligne et les modifications de champs en "
     u8"attente vivent dans un fichier SQLite local. Le recréer n'efface que ces données ; les "
     u8"identifiants du tracker et les vues sont conservés. Un rafraîchissement complet des "
     u8"tickets s'exécute ensuite."},
    {"prefs.local_data.storage_unreal.help",
     "Plugin default: writable files (config / views / SQLite cache / ImGui "
     "layout) live in <UnrealProject>/Saved next to the runtime cache. Switch to "
     "Shared when the project dir is read-only (source-controlled, network share, "
     "sandboxed runner) and Smatchet should instead use your OS user-data folder. "
     "Change takes effect on next launch.",
     u8"Défaut plugin : les fichiers inscriptibles (config / vues / cache SQLite / disposition "
     u8"ImGui) vivent dans <ProjetUnreal>/Saved à côté du cache d'exécution. Passez à Partagé "
     u8"quand le dossier projet est en lecture seule (géré en version, partage réseau, exécuteur "
     u8"sandboxé) et que Smatchet doit utiliser le dossier utilisateur de l'OS. Prend effet au "
     u8"prochain lancement."},
    {"prefs.local_data.storage_standalone.help",
     "Standalone default: writable files live in your OS user-data folder, shared "
     "across exes / installs. Switch to Portable to keep all writable files next "
     "to the executable instead — useful when running from a thumb drive or "
     "testing parallel builds. Change takes effect on next launch.",
     u8"Défaut autonome : les fichiers inscriptibles vivent dans le dossier utilisateur de "
     u8"l'OS, partagé entre exécutables / installations. Passez à Portable pour garder tous les "
     u8"fichiers inscriptibles à côté de l'exécutable — utile depuis une clé USB ou pour tester "
     u8"des builds parallèles. Prend effet au prochain lancement."},

    // Appearance tab — shortened tooltips + (?) help bodies.
    {"prefs.appearance.font.short", "Application-wide font; applies instantly.",
     u8"Police de toute l'application ; appliquée instantanément."},
    {"prefs.appearance.font.help",
     "Select the typography for the entire application. Rebuilds and reloads the "
     "font atlas instantly.",
     u8"Choisit la typographie de toute l'application. Reconstruit et recharge l'atlas de "
     u8"polices instantanément."},
    {"prefs.appearance.language.short", "UI language; applies immediately.",
     u8"Langue de l'interface ; appliquée immédiatement."},
    {"prefs.appearance.language.help",
     "Select the UI language. App-owned UI text changes immediately; tracker data "
     "is shown as-is.",
     u8"Choisit la langue de l'interface. Le texte de l'application change immédiatement ; les "
     u8"données du tracker sont affichées telles quelles."},
    {"prefs.appearance.overflow_tooltips.short", "Hover truncated cells to read the full text.",
     u8"Survolez les cellules tronquées pour lire le texte complet."},
    {"prefs.appearance.overflow_tooltips.help",
     "When a value is truncated to fit the cell, or spans multiple lines, hover "
     "to read the full text in a tooltip.",
     u8"Quand une valeur est tronquée pour tenir dans la cellule, ou s'étend sur plusieurs "
     u8"lignes, survolez pour lire le texte complet dans une infobulle."},
    {"prefs.appearance.wheel_swallow.short", "Wheel ticks swallowed at the grid edge.",
     u8"Crans de molette absorbés au bord de la grille."},
    {"prefs.appearance.wheel_swallow.help",
     "At top/bottom of the ticket grid, vertical wheel starts horizontal "
     "scrolling after this many wheel ticks. 0 routes immediately.",
     u8"En haut/bas de la grille de tickets, la molette verticale déclenche le défilement "
     u8"horizontal après ce nombre de crans. 0 bascule immédiatement."},
    {"prefs.appearance.date_format.short", "How date values render across the UI.",
     u8"Rendu des dates dans toute l'interface."},
    {"prefs.appearance.date_format.help",
     "Select the how date and datetime values are rendered in the grids and UI "
     "panels.",
     u8"Choisit comment les valeurs de date et d'heure sont rendues dans les grilles et les "
     u8"panneaux de l'interface."},
    {"prefs.appearance.date_threshold.short", "Days before compact dates switch to absolute.",
     u8"Jours avant le passage des dates compactes en absolu."},
    {"prefs.appearance.date_threshold.help",
     "Threshold in days where the compact view transitions from relative "
     "(e.g. -3d) to short absolute (e.g. May 07 '26).",
     u8"Seuil en jours où la vue compacte passe du relatif (ex. -3j) à l'absolu court "
     u8"(ex. 07 mai 26)."},
    {"prefs.appearance.vsync.short", "Sync rendering with the monitor refresh rate.",
     u8"Synchronise le rendu avec le taux de rafraîchissement de l'écran."},
    {"prefs.appearance.vsync.help",
     "Synchronize rendering with the monitor refresh rate. Disabling uncaps the "
     "frame rate (higher CPU/GPU usage).",
     u8"Synchronise le rendu avec le taux de rafraîchissement de l'écran. Désactiver libère la "
     u8"cadence d'images (usage CPU/GPU plus élevé)."},

    // Tracker tab — shortened tooltips + (?) help bodies.
    {"prefs.tracker.read_only.short", "Disables all tracker-changing actions.",
     u8"Désactive toute action modifiant le tracker."},
    {"prefs.tracker.read_only.help",
     "Disables tracker-changing actions such as field edits, issue creation, "
     "comments, worklogs, and offline write replay. Enabled by default on first "
     "launch before setup.",
     u8"Désactive les actions modifiant le tracker : édition de champs, création de tickets, "
     u8"commentaires, temps passés et rejeu des écritures hors-ligne. Activé par défaut au "
     u8"premier lancement, avant la configuration."},
    {"prefs.tracker.inherit_jira.short", "Comma-separated Jira field ids.",
     u8"Identifiants de champs Jira séparés par des virgules."},
    {"prefs.tracker.inherit_jira.help",
     "Comma-separated Jira field ids copied from the last grid row when you "
     "click + New issue (e.g. description, priority, assignee, labels, "
     "components).",
     u8"Identifiants de champs Jira séparés par des virgules, copiés depuis la dernière ligne "
     u8"de la grille au clic sur + Nouveau ticket (ex. description, priority, assignee, labels, "
     u8"components)."},
    {"prefs.tracker.inherit_plane.short", "Comma-separated Plane field ids.",
     u8"Identifiants de champs Plane séparés par des virgules."},
    {"prefs.tracker.inherit_plane.help",
     "Comma-separated Plane field ids copied from the last grid row when you "
     "click + New issue (e.g. description, priority, assignee, labels).",
     u8"Identifiants de champs Plane séparés par des virgules, copiés depuis la dernière ligne "
     u8"de la grille au clic sur + Nouveau ticket (ex. description, priority, assignee, "
     u8"labels)."},
    {"prefs.tracker.inherit_github.short", "Comma-separated GitHub field ids.",
     u8"Identifiants de champs GitHub séparés par des virgules."},
    {"prefs.tracker.inherit_github.help",
     "Comma-separated GitHub field ids copied from the last grid row when you "
     "click + New issue (e.g. body, labels, assignees, milestone).",
     u8"Identifiants de champs GitHub séparés par des virgules, copiés depuis la dernière "
     u8"ligne de la grille au clic sur + Nouveau ticket (ex. body, labels, assignees, "
     u8"milestone)."},
    {"prefs.tracker.github_repo.short", "Repository name, e.g. \"Smatchet\".", u8"Nom du dépôt, ex. « Smatchet »."},
    {"prefs.tracker.github_repo.help",
     "Repository name, e.g. \"Smatchet\". Combined with Owner: fetches issues "
     "from github.com/<owner>/<repo>. Leave both empty for cross-repo "
     "/search/issues.",
     u8"Nom du dépôt, ex. « Smatchet ». Combiné avec Propriétaire : récupère les tickets de "
     u8"github.com/<owner>/<repo>. Laissez les deux vides pour la recherche multi-dépôts "
     u8"/search/issues."},
    {"prefs.tracker.views_note.help", "Query/JQL and column fields are configured in the Views dashboard.",
     u8"La requête/JQL et les champs de colonnes se configurent dans le tableau de bord des "
     u8"vues."},

    // Integrations tab — shortened MCP tooltips + (?) help bodies.
    {"prefs.integrations.mcp_bind.short", "Off: localhost only. On: binds 0.0.0.0.",
     u8"Off : localhost uniquement. On : écoute sur 0.0.0.0."},
    {"prefs.integrations.mcp_bind.help",
     "When off, MCP listens on localhost only (127.0.0.1). When on, it binds "
     "0.0.0.0 — reachable on your network. Set an auth token below if you enable "
     "this.",
     u8"Désactivé, MCP n'écoute que sur localhost (127.0.0.1). Activé, il écoute sur 0.0.0.0 — "
     u8"joignable sur votre réseau. Définissez un jeton d'authentification ci-dessous si vous "
     u8"l'activez."},
    {"prefs.integrations.mcp_token.short", "Clients must send the X-Smatchet-Token header.",
     u8"Les clients doivent envoyer l'en-tête X-Smatchet-Token."},
    {"prefs.integrations.mcp_token.help",
     "If set, clients must send header X-Smatchet-Token with this value. If empty "
     "and bind is localhost-only, only loopback clients may connect.",
     u8"Si défini, les clients doivent envoyer l'en-tête X-Smatchet-Token avec cette valeur. "
     u8"Si vide et que l'écoute est limitée à localhost, seuls les clients en boucle locale "
     u8"peuvent se connecter."},
    {"prefs.integrations.mcp_lua.short", "Lets MCP clients execute Lua. Off by default.",
     u8"Permet aux clients MCP d'exécuter du Lua. Désactivé par défaut."},
    {"prefs.integrations.mcp_lua.help",
     "Off by default. When enabled, MCP clients can execute Lua snippets or "
     "Scripts/*.lua via the built-in run_lua tool.",
     u8"Désactivé par défaut. Une fois activé, les clients MCP peuvent exécuter des extraits "
     u8"Lua ou Scripts/*.lua via l'outil intégré run_lua."},

    // Preferences footer — per-tab save-semantics line + (?) full explanation.
    {"prefs.footer.tracker.short", "Save & Sync writes this tab to disk and refreshes the tracker connection.",
     u8"Enregistrer & synchroniser écrit cet onglet sur le disque et actualise la connexion au "
     u8"tracker."},
    {"prefs.footer.integrations.short",
     "MCP settings save when changed. Runtime status: Automation -> Agent Bridge (MCP)...",
     u8"Les réglages MCP s'enregistrent dès modification. État d'exécution : Automation -> Agent "
     u8"Bridge (MCP)..."},
    {"prefs.footer.assistant.short",
     "Assistant settings use explicit Save / Discard. Unsaved edits show an * on the tab.",
     u8"Les réglages de l'assistant utilisent un Enregistrer / Annuler explicite. Les modifications "
     u8"non enregistrées affichent un * sur l'onglet."},
    {"prefs.footer.autosave.short", "Settings on this tab save automatically when changed.",
     u8"Les réglages de cet onglet s'enregistrent automatiquement dès modification."},
    {"prefs.footer.immediate.short", "Options on this tab apply and save immediately.",
     u8"Les options de cet onglet s'appliquent et s'enregistrent immédiatement."},
    {"prefs.footer.annotate.short", "This tab has its own Save settings and Reload settings buttons.",
     u8"Cet onglet possède ses propres boutons Save settings et Reload settings."},
    {"prefs.footer.save_sync.help",
     "Save & Sync writes the Tracker tab (and optional Integrations tab when enabled in this build) to "
     "disk and refreshes the tracker connection. Assistant, Whisper, Local data, Appearance, and template "
     "settings save automatically when changed. MCP runtime status: Automation -> Agent Bridge (MCP)... "
     "Log level and verbose logging: Inspect -> Runtime Log. The Annotate Analysis tab has its own Save "
     "settings and Reload settings buttons.",
     u8"Enregistrer & synchroniser écrit l'onglet Tracker (et l'onglet Integrations si activé dans "
     u8"cette version) sur le disque et actualise la connexion au tracker. Les réglages Assistant, "
     u8"Whisper, Données locales, Apparence et modèles s'enregistrent automatiquement dès "
     u8"modification. État d'exécution MCP : Automation -> Agent Bridge (MCP)... Niveau de "
     u8"journalisation et journaux détaillés : Inspect -> Runtime Log. L'onglet Annotate Analysis "
     u8"possède ses propres boutons Save settings et Reload settings."},

    // Templates tab — shortened tooltips + (?) help bodies.
    {"prefs.templates.longtext_preview.short", "Long-text editor opens in preview mode.",
     u8"L'éditeur de texte long s'ouvre en mode aperçu."},
    {"prefs.templates.longtext_preview.help",
     "When on, the long-text edit modal (description, callstack, custom "
     "textarea fields) opens showing the rendered preview. When off (default) "
     "it opens in edit mode. Ctrl+P cycles Edit/Split/Preview either way.",
     u8"Activé, la fenêtre d'édition de texte long (description, callstack, champs textarea "
     u8"personnalisés) s'ouvre sur l'aperçu rendu. Désactivé (défaut), elle s'ouvre en mode "
     u8"édition. Ctrl+P alterne Édition/Partagé/Aperçu dans les deux cas."},
    {"prefs.templates.duration_suggestions.short", "Default options for time-estimate dropdowns.",
     u8"Options par défaut des menus d'estimation de temps."},
    {"prefs.templates.duration_suggestions.help",
     "Customize the default options displayed in the dropdown menus for "
     "Original Estimate, Remaining Estimate, and Time Spent fields.",
     u8"Personnalise les options par défaut affichées dans les menus déroulants des champs "
     u8"Estimation initiale, Estimation restante et Temps passé."},

    // Perf panel — shortened tab intros + (?) help bodies.
    {"perf.cpu.intro.short", "Wall time per scoped UI path (smoothed).",
     u8"Temps réel par chemin UI instrumenté (lissé)."},
    {"perf.cpu.intro.help",
     "Wall time per scoped UI path; nested scopes overlap (not additive). "
     "Values use heavy time-based smoothing (~few second response) and 2 decimal places. "
     "Row order for Last (ms) uses 0.1 ms buckets so nearby scopes do not swap every frame.",
     u8"Temps réel par chemin UI instrumenté ; les portées imbriquées se recouvrent (non "
     u8"additives). Les valeurs utilisent un fort lissage temporel (réponse de quelques "
     u8"secondes) et 2 décimales. L'ordre des lignes pour Last (ms) utilise des paliers de "
     u8"0,1 ms pour éviter que des portées proches permutent à chaque image."},
    {"perf.network.intro.short", "HTTP traffic from this session of Smatchet.",
     u8"Trafic HTTP de cette session de Smatchet."},
    {"perf.network.intro.help",
     "Downloaded = response bodies; uploaded = request bodies for POST/PUT, or ~%llu B per "
     "Jira GET (estimate).",
     u8"Téléchargé = corps des réponses ; envoyé = corps des requêtes POST/PUT, ou ~%llu o par "
     u8"GET Jira (estimation)."},

    // Offline-queue unknown-conflict pane — shortened body + (?) help.
    {"offline.conflict_unknown.corrupt.short", "Conflict details could not be read.",
     u8"Les détails du conflit n'ont pas pu être lus."},
    {"offline.conflict_unknown.action.short", "Close keeps the edit queued; Discard drops it.",
     u8"Fermer garde la modification en file ; Abandonner la supprime."},
    {"offline.conflict_unknown.help",
     "This offline edit's conflict details could not be read (corrupt or stale data). "
     "To avoid overwriting either version with empty content, this conflict can't be "
     "resolved automatically. Close to leave the edit queued, or discard it to drop the "
     "queued change.",
     u8"Les détails du conflit de cette modification hors-ligne n'ont pas pu être lus (données "
     u8"corrompues ou périmées). Pour éviter d'écraser l'une ou l'autre version avec un contenu "
     u8"vide, ce conflit ne peut pas être résolu automatiquement. Fermez pour laisser la "
     u8"modification en file, ou abandonnez-la pour supprimer le changement en attente."},

    // AI assistant per-turn effort — shortened tooltip + (?) help.
    {"ai.turn_effort.short", "Per-turn OpenAI `reasoning_effort` override.",
     u8"Surcharge `reasoning_effort` OpenAI pour ce tour."},
    {"ai.turn_effort.help",
     "Per-turn reasoning effort. Applied as the OpenAI `reasoning_effort` parameter; "
     "providers that don't understand the param ignore it.",
     u8"Effort de raisonnement pour ce tour. Appliqué comme paramètre OpenAI "
     u8"`reasoning_effort` ; les fournisseurs qui ne le comprennent pas l'ignorent."},

    // Bug-report screenshot redaction — (?) help body.
    {"bugreport.screenshot_redaction.help",
     "The screenshot is captured with all text rendered as blocks (██) — sharp, "
     "layout-preserving, no readable text. Icons + colour stay intact.",
     u8"La capture d'écran est prise avec tout le texte rendu en blocs (██) — nette, mise en "
     u8"page préservée, aucun texte lisible. Icônes et couleurs restent intactes."},

    // Update banner — shortened one-liner (no marker; only "standalone" dropped).
    {"update.banner.short", "A newer Smatchet release is available on GitHub.",
     u8"Une version plus récente de Smatchet est disponible sur GitHub."},

    {"views.active_view", "Active View", u8"Vue active"},
    {"views.view_name", "View Name", u8"Nom de la vue"},
    {"views.apply_sync", "Apply View & Sync", u8"Appliquer la vue et synchroniser"},
    {"views.create_new", "Create New View", u8"Créer une vue"},
    {"views.delete", "Delete View", u8"Supprimer la vue"},
    {"views.loading_fields", "Loading available fields...", u8"Chargement des champs disponibles..."},
    {"views.field_picker", "Field Picker", u8"Sélecteur de champs"},
    {"views.search_fields_hint", "Search by field id or name", u8"Rechercher par ID ou nom de champ"},
    {"views.selected_count", "Selected: %zu", u8"Sélectionnés : %zu"},
    {"views.visible_count", "Visible: %zu", u8"Visibles : %zu"},
    {"views.select_all_visible", "Select All Visible", u8"Tout sélectionner visible"},
    {"views.clear_visible", "Clear Visible", u8"Effacer visible"},
    {"views.no_catalog", "No field catalog loaded yet.", u8"Aucun catalogue de champs chargé."},
    {"views.no_fields_match", "No fields match current search.", u8"Aucun champ ne correspond à la recherche."},
    {"views.basic_fields", "Basic Fields", u8"Champs de base"},
    {"views.system_fields", "System Fields", u8"Champs système"},
    {"views.custom_fields", "Custom Fields", u8"Champs personnalisés"},
    {"views.id_always_selected", "ID (id, always selected)", u8"ID (id, toujours sélectionné)"},
    {"views.column_order", "Column Order", u8"Ordre des colonnes"},
    {"views.remove_column_hint", "(right-click a column to remove it from the view)",
     u8"(clic droit sur une colonne pour la retirer de la vue)"},
    {"views.remove_column", "Remove column from view", u8"Retirer la colonne de la vue"},
    {"views.move_up", "Move Up", u8"Monter"},
    {"views.move_down", "Move Down", u8"Descendre"},
    {"views.no_views", "No views available.", u8"Aucune vue disponible."},
    {"views.prev_query_tip", "Run the previous query from navigation history.",
     u8"Exécuter la requête précédente de l'historique de navigation."},
    {"views.next_query_tip", "Run the next query from navigation history.",
     u8"Exécuter la requête suivante de l'historique de navigation."},
    {"views.query", "Query", u8"Requête"},
    {"views.jql", "JQL", u8"JQL"},
    {"views.plane_filter", "Plane filter", u8"Filtre Plane"},
    {"views.apply_query", "Apply Query", u8"Appliquer la requête"},
    {"views.apply_jql", "Apply JQL", u8"Appliquer le JQL"},
    {"views.clear_query", "Clear query", u8"Effacer la requête"},
    {"views.open_query_tip", "Open this query in tracker.", u8"Ouvrir cette requête dans le suivi."},
    {"views.save_query_tip", "Save this query on the active view and sync issues.",
     u8"Enregistrer cette requête dans la vue active et synchroniser les tickets."},

    {"bulk.source", "Source:", u8"Source :"},
    {"bulk.load_file", "Load file", u8"Charger un fichier"},
    {"bulk.paste_clipboard", "Paste clipboard", u8"Coller le presse-papiers"},
    {"bulk.parse_preview", "Parse preview", u8"Analyser l'aperçu"},
    {"bulk.run_import", "Run import", u8"Lancer l'import"},
    {"bulk.source_help", "Paste / edit source text here (headers = field ids or display names):",
     u8"Collez / modifiez le texte source ici (en-têtes = IDs ou noms de champs) :"},
    {"bulk.destination", "Destination path (for Save):", u8"Chemin de destination (pour Enregistrer) :"},
    {"bulk.copy_clipboard", "Copy to clipboard", u8"Copier dans le presse-papiers"},
    {"bulk.save_file", "Save to file", u8"Enregistrer dans un fichier"},
    {"bulk.headers_note", "Headers are field ids; import can read them back.",
     u8"Les en-têtes sont des IDs de champs ; l'import peut les relire."},

    {"audit.file", "Audit file: %s", u8"Fichier d'audit : %s"},
    {"audit.action", "Action", u8"Action"},
    {"audit.result", "Result", u8"Résultat"},
    {"audit.all", "All", u8"Tout"},
    {"audit.success", "Success", u8"Succès"},
    {"audit.failure", "Failure", u8"Échec"},
    {"audit.newest_first", "Newest first", u8"Plus récents d'abord"},
    {"audit.rows_page", "Rows/page", u8"Lignes/page"},
    {"audit.prev_page", "Prev##auditPage", u8"Préc.##auditPage"},
    {"audit.next_page", "Next##auditPage", u8"Suiv.##auditPage"},
    {"audit.page_count", "Page %d/%d (%zu rows)", u8"Page %d/%d (%zu lignes)"},
    {"audit.copy_filtered", "Copy filtered table (TSV)", u8"Copier le tableau filtré (TSV)"},
    {"audit.copy_row_json", "Copy audit row JSON", u8"Copier la ligne d'audit JSON"},

    {"ai.select_ticket", "Select a ticket to see AI insights.", u8"Sélectionnez un ticket pour voir les analyses IA."},
    {"ai.analyzing", "Analyzing: %s", u8"Analyse : %s"},
    {"ai.selected_fields", "Selected Fields", u8"Champs sélectionnés"},
    {"ai.model", "Model: %s", u8"Modèle : %s"},
    {"ai.endpoint", "Endpoint: %s", u8"Point de terminaison : %s"},
    {"ai.generate_action_plan", "Generate Action Plan", u8"Générer un plan d'action"},
    {"ai.clear_context", "Clear Context", u8"Effacer le contexte"},
    {"ai.thinking", "AI is thinking...", u8"L'IA réfléchit..."},
    {"ai.paste_truncated.title", "Paste truncated", u8"Collage tronqué"},
    {"ai.paste_truncated.body", "%d KB dropped (input limit %d KB)", u8"%d Ko supprimés (limite de saisie %d Ko)"},

    {"log.min_level", "Min log level", u8"Niveau minimal du journal"},
    {"log.tracker_bodies", "Log Tracker HTTP bodies (truncated)", u8"Journaliser les corps HTTP du suivi (tronqués)"},
    {"log.p4_stdout", "Log Perforce p4 stdout (truncated, Trace level)",
     u8"Journaliser stdout de Perforce p4 (tronqué, niveau Trace)"},
    {"log.clear", "Clear Log", u8"Effacer le journal"},
    {"log.copy_log", "Copy log", u8"Copier le journal"},
    {"log.copy_log_tip", "Copy the full application log to the clipboard.",
     u8"Copier l'intégralité du journal de l'application dans le presse-papiers."},
    {"log.auto_scroll", "Auto-scroll", u8"Défilement automatique"},
    {"log.application_log", "(application log)", u8"(journal de l'application)"},

    {"attachment.count", "Attachments: %d", u8"Pièces jointes : %d"},
    {"attachment.preview_error", "Preview error", u8"Erreur d'aperçu"},
    {"attachment.loading", "Loading...", u8"Chargement..."},
    {"attachment.metadata", "Metadata", u8"Métadonnées"},
    {"attachment.image", "Image", u8"Image"},
    {"attachment.file", "File", u8"Fichier"},
    {"attachment.preview_failed", "preview failed", u8"échec de l'aperçu"},

    {"new_issue.add", "+ New issue", u8"+ Nouveau ticket"},
    {"new_issue.new", "[new]", u8"[nouveau]"},
    {"new_issue.create_failed_queue", "Create failed. Queue instead?", u8"Création échouée. Mettre en file ?"},
    {"new_issue.queue_offline", "Queue offline", u8"Mettre en file hors ligne"},
    {"new_issue.add_attachment", "Add attachment...", u8"Ajouter une pièce jointe..."},
    {"new_issue.clear_all", "Clear all##clratt", u8"Tout effacer##clratt"},
    {"new_issue.choose", "(choose)", u8"(choisir)"},

    {"tracker.issue", "Issue:", u8"Ticket :"},
    {"tracker.no_watchers", "No watchers.", u8"Aucun observateur."},
    {"tracker.no_votes", "No votes.", u8"Aucun vote."},
    {"tracker.no_voters", "No voters to list.", u8"Aucun votant à afficher."},
    {"tracker.loading_watchers", "Loading watchers...", u8"Chargement des observateurs..."},
    {"tracker.loading_votes", "Loading votes...", u8"Chargement des votes..."},

    // issue-comments PR-A — backend-agnostic comments read/post modal + grid cell.
    {"comments.title", "Comments", u8"Commentaires"},
    {"comments.loading", "Loading comments...", u8"Chargement des commentaires..."},
    {"comments.none", "No comments yet.", u8"Aucun commentaire pour l'instant."},
    {"comments.fetch_failed", "Failed to load comments.", u8"Impossible de charger les commentaires."},
    {"comments.post_placeholder", "Write a comment (plain text)...", u8"Écrire un commentaire (texte brut)..."},
    {"comments.posting", "Posting comment...", u8"Publication du commentaire..."},
    {"comments.post_failed", "Failed to post comment.", u8"Impossible de publier le commentaire."},
    {"comments.disabled_readonly", "(disabled while offline/read-only)", u8"(désactivé hors ligne / en lecture seule)"},
    {"comments.cell_tooltip", "View / post comments", u8"Voir / publier des commentaires"},
    {"comments.queued_title", "Comment Queued", u8"Commentaire en file"},
    {"comments.posted_body", "Comment added.", u8"Commentaire ajouté."},
    {"comments.post_button", "Post Comment", u8"Publier le commentaire"},
    {"comments.unknown_author", "(unknown)", u8"(inconnu)"},

    {"field.clear", "<clear>", u8"<effacer>"},
    {"field.clear_all", "<clear all>", u8"<tout effacer>"},
    {"field.parent_only", "<parent only>", u8"<parent uniquement>"},
    {"field.no_labels", "(no labels discovered yet)", u8"(aucune étiquette découverte)"},
    {"field.no_matching_labels", "(no matching labels)", u8"(aucune étiquette correspondante)"},
    {"field.required_tooltip", "Required field", u8"Champ obligatoire"},
    {"field.required_inline_hint", "(required)", u8"(obligatoire)"},
    {"worklog.none", "No work logged yet. Click to log work.", u8"Aucun travail journalisé. Cliquez pour le saisir."},
    {"worklog.total", "Total Time Spent: %s\nClick to log work / edit estimates.",
     u8"Temps total passé : %s\nCliquez pour saisir le travail / modifier les estimations."},
    {"worklog.title", "Time tracking: %s", u8"Suivi du temps : %s"},
    {"worklog.original_estimate", "The original estimate for this work item was %s.",
     u8"L'estimation initiale de cet élément était %s."},
    {"worklog.time_spent", "Time spent *", u8"Temps passé *"},
    {"worklog.format_help", "Use the format: 2w 4d 6h 45m (w=weeks, d=days, h=hours, m=minutes)",
     u8"Utilisez le format : 2w 4d 6h 45m (w=semaines, d=jours, h=heures, m=minutes)"},
    {"worklog.time_remaining", "Time remaining", u8"Temps restant"},
    {"worklog.date_started", "Date started *", u8"Date de début *"},
    {"worklog.description", "Work description", u8"Description du travail"},
    {"worklog.templates", "Templates ▼", u8"Modèles ▼"},
    {"worklog.no_templates", "No templates configured in Preferences.",
     u8"Aucun modèle configuré dans les Préférences."},

    {"date.edit_raw", "Edit raw ISO string:", u8"Modifier la chaîne ISO brute :"},
    {"date.apply_parsed", "Apply parsed value", u8"Appliquer la valeur analysée"},
    {"date.time_utc", "Time (UTC wall)", u8"Heure (UTC)"},
    {"date.raw_iso", "Raw ISO…", u8"ISO brut…"},

    {"mcp.select_copy", "Select text below and use Ctrl+C to copy.",
     u8"Sélectionnez le texte ci-dessous et utilisez Ctrl+C pour copier."},
    {"mcp.recent_actions", "Recent actions", u8"Actions récentes"},
    {"mcp.edit_prefs", "Edit MCP fields in Preferences → Integrations.",
     u8"Modifiez les champs MCP dans Préférences → Intégrations."},

    {"lua.clear", "Clear", u8"Effacer"},
    {"lua.copy_clipboard", "Copy to Clipboard", u8"Copier dans le presse-papiers"},
    {"lua.auto_scroll", "Auto-scroll", u8"Défilement automatique"},
    {"lua.output", "Smatchet Automation Output", u8"Sortie de l'automatisation Smatchet"},
    {"lua.save_switch", "Save changes to the current script before switching?",
     u8"Enregistrer les changements du script courant avant de changer ?"},
    {"lua.no_actions", "No global actions registered.", u8"Aucune action globale enregistrée."},

    {"annotate.click_cl", "Click to open this changelist in p4vc.",
     u8"Cliquez pour ouvrir cette changelist dans p4vc."},
    {"annotate.loading_cl", "Loading CL info…", u8"Chargement des infos CL…"},
    {"annotate.no_describe", "(no describe details)", u8"(aucun détail describe)"},
    {"annotate.max_frames", "Max frames", u8"Nombre max de frames"},
    {"annotate.p4_executable", "P4 executable", u8"Exécutable P4"},
    {"annotate.p4vc_executable", "p4vc executable", u8"Exécutable p4vc"},
    {"annotate.timelapse_cmd", "Timelapse cmd (optional)", u8"Commande Timelapse (facultative)"},
    {"annotate.changelist_cmd", "Changelist cmd (optional)", u8"Commande changelist (facultative)"},
    {"annotate.ai_chat_url", "AI chat URL (optional)", u8"URL du chat IA (facultative)"},
    {"annotate.callstack_jira", "Callstack from Jira", u8"Pile d'appels depuis Jira"},
    {"annotate.callstack_field", "Callstack field id (optional)", u8"ID du champ de pile d'appels (facultatif)"},
    {"annotate.path_remap_from", "Path remap from", u8"Remapper le chemin depuis"},
    {"annotate.path_remap_to", "Path remap to", u8"Remapper le chemin vers"},
    {"annotate.save_settings", "Save settings", u8"Enregistrer les paramètres"},
    {"annotate.reload_settings", "Reload settings", u8"Recharger les paramètres"},
    {"annotate.ask_ai", "Ask AI", u8"Demander à l'IA"},
    {"annotate.export_json", "Export JSON", u8"Exporter JSON"},
    {"annotate.export_csv", "Export CSV", u8"Exporter CSV"},
    {"annotate.callstack", "Callstack", u8"Pile d'appels"},
    {"annotate.show_table", "Show Table", u8"Afficher le tableau"},
    {"annotate.show_raw", "Show Raw Text", u8"Afficher le texte brut"},
    {"annotate.at_changelist", "Before changelist (optional)", u8"Avant la changelist (facultatif)"},
    {"annotate.process", "Process", u8"Traiter"},
    {"annotate.assign_user", "Assign issue to user", u8"Assigner le ticket à l'utilisateur"},
    {"annotate.add_context_comment", "Add Annotate context comment",
     u8"Ajouter un commentaire de contexte d'annotation"},
    {"annotate.quick_templates", "Quick comment templates", u8"Modèles de commentaires rapides"},
    {"annotate.assign_and_comment", "Assign and add Annotate context",
     u8"Assigner et ajouter le contexte d'annotation"},
    {"annotate.groups", "Groups (best effort):", u8"Groupes (au mieux) :"},
    {"annotate.none_permitted", "(none or not permitted)", u8"(aucun ou non autorisé)"},

    // Project picker (draft + bulk import modal).
    {"draft.project", "Project", u8"Projet"},
    {"draft.project.placeholder", "(pick one)", u8"(choisir)"},
    {"draft.project.section.recent", "Recently used", u8"Récemment utilisés"},
    {"draft.project.section.all", "All projects", u8"Tous les projets"},
    {"draft.project.search", "Search...", u8"Rechercher..."},
    {"draft.project.loading", "Loading...", u8"Chargement..."},
    {"draft.project.submit_disabled_tooltip", "Pick a project", u8"Choisir un projet"},

    // multi-grid-tabs Slice 2: dockable grid panes.
    {"pane.add.tooltip",
     "New grid pane (duplicates this pane; dock it as a tab or drag its tab to an edge for a side-by-side split)",
     u8"Nouveau panneau de grille (duplique ce panneau ; ancrez-le comme onglet ou faites glisser son onglet vers un "
     u8"bord pour une vue côte à côte)"},

    // Active-view JQL bar project pill (Jira only).
    {"view.projectPill.single", "Project: %s", u8"Projet : %s"},
    {"view.projectPill.multi", "Project: multi", u8"Projet : multiple"},
    {"view.projectPill.tooltip.single", "Active view is scoped to a single project. Click to switch project.",
     u8"La vue active est limitée à un seul projet. Cliquer pour changer de projet."},
    {"view.projectPill.tooltip.multi",
     "Active view spans multiple projects (or has no project clause). Click to pick a single project.",
     u8"La vue active couvre plusieurs projets (ou n'a aucune clause de projet). Cliquer pour choisir un seul projet."},
    {"view.projectPill.empty", "(no cached projects)", u8"(aucun projet en cache)"},

    // Bulk import modal.
    {"bulkImport.chooseProject.title", "Choose target project for bulk import",
     u8"Choisir le projet cible pour l'import en masse"},
    {"bulkImport.chooseProject.cancel", "Cancel", u8"Annuler"},
    {"bulkImport.chooseProject.confirm", "Use this project", u8"Utiliser ce projet"},

    // Whisper dictation — first-run setup banner + model picker (Phase C).
    // See docs/plans/shipped/whisper-dictation.md § Setup banner spec.
    {"whisper.banner.title",
     "Enable voice dictation? Push-to-talk transcribes into any text field. Optional, off by default. "
     "No audio leaves your machine when the local model is used.",
     u8"Activer la dictée vocale ? Une touche transcrit dans n'importe quel champ texte. Option désactivée par défaut. "
     u8"Aucun audio ne quitte votre machine avec le modèle local."},
    {"whisper.banner.enable", "Enable", u8"Activer"},
    {"whisper.banner.later", "Decide later", u8"Décider plus tard"},
    {"whisper.banner.disable", "No thanks", u8"Non merci"},
    {"whisper.banner.downloading", "Downloading speech model...", u8"Téléchargement du modèle vocal..."},
    {"whisper.banner.verifying", "Verifying download...", u8"Vérification du téléchargement..."},
    {"whisper.banner.pickModel", "Choose speech model:", u8"Choisir un modèle vocal :"},
    {"whisper.modelPicker.smaller", "Smaller, faster (40 MB)", u8"Plus petit, plus rapide (40 Mo)"},
    {"whisper.modelPicker.recommended", "Recommended (150 MB)", u8"Recommandé (150 Mo)"},
    {"whisper.modelPicker.higher", "Higher accuracy (500 MB)", u8"Précision supérieure (500 Mo)"},
    {"whisper.modelPicker.download", "Download + enable", u8"Télécharger et activer"},
    {"whisper.modelPicker.cancel", "Cancel", u8"Annuler"},

    {"agent.prefs.tabTitle", "Agentic", u8"Agentic"},
    {"agent.prefs.enableToggle", "Enable scheduled agentic triage", u8"Activer le tri agentique planifié"},
    {"agent.prefs.intervalLabel", "Interval:", u8"Intervalle :"},
    {"agent.prefs.intervalUnit", "seconds (60..3600)", u8"secondes (60..3600)"},
    {"agent.prefs.sourceLabel", "Source:", u8"Source :"},
    {"agent.prefs.queryLabel", "Query:", u8"Requête :"},
    {"agent.prefs.queryHint.github", "For github: OWNER/REPO of the repository to poll",
     u8"Pour github : OWNER/REPO du dépôt à interroger"},
    {"agent.prefs.githubPatLabel", "GitHub PAT:", u8"PAT GitHub :"},
    {"agent.prefs.githubPatHint", "Bearer token - needs `repo` + `issues` scope",
     u8"Jeton Bearer - portée `repo` + `issues` requise"},
    {"agent.prefs.lastPoll", "Last poll:", u8"Dernier sondage :"},
    {"agent.prefs.lastPollNever", "never", u8"jamais"},
    {"agent.prefs.nextPoll", "Next poll: ~in {0}", u8"Prochain sondage : ~dans {0}"},
    {"agent.prefs.runNow", "Run triage now", u8"Lancer le tri maintenant"},
    // Triage-feedback Preferences UI — manual-trigger in-flight indicator + last-run summary.
    {"agent.prefs.runNowInFlight", "Triage running", u8"Tri en cours"},
    {"agent.prefs.runNowDone", "Last run:", u8"Dernière exécution :"},
    {"agent.prefs.runNowFailed", "Last run failed:", u8"Dernière exécution échouée :"},
    // Context-doc section in the Agentic Preferences tab — Save / Generate buttons + heading.
    {"agent.prefs.contextDocSection", "Agentic triage context document",
     u8"Document de contexte pour le tri agentique"},
    {"agent.prefs.contextDocHint",
     "Prepended to every triage LLM request. Stored at %LOCALAPPDATA%/Smatchet/agentic-context.md.",
     u8"Ajouté en préambule à chaque requête LLM de tri. Stocké dans "
     u8"%LOCALAPPDATA%/Smatchet/agentic-context.md."},
    {"agent.prefs.contextDocSave", "Save", u8"Enregistrer"},
    {"agent.prefs.contextDocGenerate", "Generate from current project", u8"Générer à partir du projet actuel"},
    {"whisper.preferences.tabTitle", "Whisper", u8"Whisper"},
    {"whisper.preferences.enableToggle", "Enable voice dictation", u8"Activer la dictée vocale"},
    {"whisper.preferences.modeAuto", "Auto (local if present, cloud fallback)",
     u8"Auto (local si présent, repli vers le cloud)"},
    {"whisper.preferences.modeLocal", "Local only (no network)", u8"Local uniquement (hors réseau)"},
    {"whisper.preferences.modeCloud", "Cloud only (OpenAI)", u8"Cloud uniquement (OpenAI)"},
    {"whisper.preferences.model", "Speech model", u8"Modèle vocal"},
    {"whisper.preferences.modelPresent", "(installed)", u8"(installé)"},
    {"whisper.preferences.downloadModel", "Download model", u8"Télécharger le modèle"},
    {"whisper.preferences.cancelDownload", "Cancel download", u8"Annuler le téléchargement"},
    {"whisper.preferences.hotkey", "Push-to-talk hotkey", u8"Touche de dictée"},
    {"whisper.preferences.hotkeyReadonly", "Hotkey rebinding UI lands in Phase E. The active hotkey is:",
     u8"L'interface de redéfinition arrive en Phase E. La touche active est :"},
    {"whisper.preferences.hotkeyRebindButton", "Click to rebind", u8"Cliquez pour redéfinir"},
    {"whisper.preferences.hotkeyCapturing", "Press a key combo... (Esc to cancel)",
     u8"Appuyez sur une combinaison... (Échap pour annuler)"},
    {"whisper.preferences.hotkeyErrorModifiersOnly", "Hotkey must include a non-modifier key",
     u8"La combinaison doit inclure une touche autre que modificateur"},
    {"whisper.preferences.hotkeyErrorReserved", "That combo is reserved by the operating system",
     u8"Cette combinaison est réservée par le système d'exploitation"},
    {"whisper.preferences.hotkeyErrorParse", "Could not parse the captured key combo",
     u8"Impossible d'analyser la combinaison capturée"},
    {"whisper.statusBar.recording", "\xE2\x97\x8F REC", u8"\xE2\x97\x8F REC"},
    {"whisper.statusBar.recordingTooltip",
     "Recording for dictation — release hotkey to transcribe; press Esc to cancel",
     u8"Enregistrement de dictée — relâchez la touche pour transcrire ; Échap pour annuler"},
    {"whisper.overlay.recordingLabel", "\xE2\x97\x8F REC", u8"\xE2\x97\x8F REC"},
    {"whisper.overlay.holdHint", "hold hotkey", u8"maintenez la touche"},
    {"whisper.overlay.cancelButton", "Cancel (Esc)", u8"Annuler (Échap)"},
    {"whisper.preferences.apiKey", "OpenAI API key (cloud mode)", u8"Clé API OpenAI (mode cloud)"},
    {"whisper.preferences.apiKeyFallback", "(uses AI Assistant OpenAI key when empty)",
     u8"(utilise la clé OpenAI de l'assistant IA si vide)"},
    {"whisper.preferences.privacyHeading", "Privacy disclosure", u8"Information sur la confidentialité"},
    {"whisper.preferences.privacyLocal", "Local mode: audio stays on your machine; no network call is made.",
     u8"Mode local : l'audio reste sur votre machine ; aucun appel réseau."},
    {"whisper.preferences.privacyCloud", "Cloud mode: audio is uploaded to OpenAI for transcription.",
     u8"Mode cloud : l'audio est envoyé à OpenAI pour transcription."},
    {"whisper.preferences.privacyDisabled", "Disabled: no microphone access, no network call, no model download.",
     u8"Désactivé : aucun accès micro, aucun appel réseau, aucun téléchargement."},
    // Phase F — polished Preferences tab keys (hot rebind error / test connection /
    // language / trim / max-clip / auto-send / re-run setup banner).
    {"whisper.preferences.hotkeyErrorRebind", "Hotkey rebind failed: ", u8"Échec de la redéfinition de la touche : "},
    {"whisper.preferences.apiKey.testButton", "Test connection", u8"Tester la connexion"},
    {"whisper.preferences.testConnection.success", "Connected", u8"Connecté"},
    {"whisper.preferences.testConnection.failure", "Failed: ", u8"Échec : "},
    {"whisper.preferences.language.label", "Language:", u8"Langue :"},
    {"whisper.preferences.language.autoHint", "(or \"auto\" for autodetect)",
     u8"(ou « auto » pour détection automatique)"},
    {"whisper.preferences.trim.label", "Trim leading/trailing silence", u8"Couper les silences au début et à la fin"},
    {"whisper.preferences.maxClipSec.label", "Max clip length:", u8"Durée maximale du clip :"},
    {"whisper.preferences.maxClipSec.unit", "seconds", u8"secondes"},
    {"whisper.preferences.maxClipSec.hint", "(0 = unlimited; max 600)", u8"(0 = illimité ; max 600)"},
    {"whisper.preferences.autoSend.label", "Auto-send AI chat on punctuation (\".\", \"!\", \"?\")",
     u8"Envoi automatique du chat IA sur ponctuation (« . », « ! », « ? »)"},
    {"whisper.preferences.rerunSetup.button", "Re-run setup banner", u8"Relancer la bannière de configuration"},
    {"whisper.preferences.rerunSetup.tooltip", "Forces WhisperSetupCompleted=false; banner appears next launch",
     u8"Force WhisperSetupCompleted=false ; la bannière réapparaît au prochain lancement"},
};

std::mutex& LocalizationMutex() {
    static std::mutex m;
    return m;
}

std::string& CurrentLanguageRef() {
    static std::string lang = "en-US";
    return lang;
}

std::unordered_map<std::string, std::string>& OverridesRef() {
    static std::unordered_map<std::string, std::string> overrides;
    return overrides;
}

std::unordered_set<std::string>& MissingKeysRef() {
    static std::unordered_set<std::string> missing;
    return missing;
}

const std::unordered_map<std::string, const TranslationEntry*>& EntriesByKey() {
    static std::unordered_map<std::string, const TranslationEntry*> map;
    if (map.empty()) {
        for (const TranslationEntry& entry : kEntries) {
            map[entry.Key] = &entry;
        }
    }
    return map;
}

const std::unordered_map<std::string, const TranslationEntry*>& EntriesByEnglish() {
    static std::unordered_map<std::string, const TranslationEntry*> map;
    if (map.empty()) {
        for (const TranslationEntry& entry : kEntries) {
            if (!entry.English || !*entry.English) {
                continue;
            }
            // First entry wins so common aliases like "Save" stay stable.
            map.emplace(entry.English, &entry);
        }
    }
    return map;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

const char* StoreTempString(std::string value) {
    static thread_local std::vector<std::string> ring(512);
    static thread_local std::size_t index = 0;
    ring[index] = std::move(value);
    const char* result = ring[index].c_str();
    index = (index + 1) % ring.size();
    return result;
}

// CPP_CODE_AUDIT.md #25: return override strings through StoreTempString (a copy into a
// thread-local ring) instead of `overrideIt->second.c_str()` — a pointer straight into
// OverridesRef()'s map. The caller (T() / TranslateSource()) reads the returned pointer
// AFTER releasing LocalizationMutex(); SetLanguage() -> LoadOverridesLocked() ->
// OverridesRef().clear() destroys the strings the map owned, so a raw pointer into the
// map dangles the moment a language switch races the caller's read.
const char* TranslateEntryLocked(const TranslationEntry& entry, const char* fallback) {
    const auto& overrides = OverridesRef();
    auto overrideIt = overrides.find(entry.Key);
    if (overrideIt != overrides.end()) {
        return StoreTempString(overrideIt->second);
    }
    if (entry.English) {
        overrideIt = overrides.find(entry.English);
        if (overrideIt != overrides.end()) {
            return StoreTempString(overrideIt->second);
        }
    }
    if (CurrentLanguageRef() == "fr-FR" && entry.French && entry.French[0] != '\0') {
        return entry.French;
    }
    return fallback ? fallback : (entry.English ? entry.English : "");
}

void LoadOverridesLocked(const std::string& language) {
    OverridesRef().clear();
    MissingKeysRef().clear();

    const std::string path = ConfigManager::GetRuntimeAssetDirectory() + "Locales/" + language + ".json";
    const nlohmann::json root = ConfigManager::LoadJsonFile(path);
    if (!root.is_object()) {
        return;
    }
    const std::string declaredLocale = SmatchetLocalization::NormalizeLanguageCode(root.value("locale", language));
    if (declaredLocale != language) {
        LOG_WARN("Localization: ignoring override file '%s' with locale '%s' while language is '%s'.", path.c_str(),
                 declaredLocale.c_str(), language.c_str());
        return;
    }
    if (!root.contains("strings") || !root["strings"].is_object()) {
        return;
    }
    for (auto it = root["strings"].begin(); it != root["strings"].end(); ++it) {
        if (it.value().is_string()) {
            OverridesRef()[it.key()] = it.value().get<std::string>();
        }
    }
    LOG_INFO("Localization: loaded %zu override strings from '%s'.", OverridesRef().size(), path.c_str());
}

std::string JoinWithStableId(const char* translated, const char* stableId, const char* separator) {
    if (!translated) {
        translated = "";
    }
    if (!stableId || stableId[0] == '\0') {
        return translated;
    }
    std::string out(translated);
    out += separator;
    out += stableId;
    return out;
}

const char* BuildLabelFromSource(const char* label, bool windowTitle) {
    if (!label) {
        return "";
    }

    const std::string original(label);
    const std::size_t triple = original.find("###");
    const std::size_t doubleHash = (triple == std::string::npos) ? original.find("##") : std::string::npos;
    const std::size_t split = triple != std::string::npos ? triple : doubleHash;
    const std::string visible = split == std::string::npos ? original : original.substr(0, split);
    const std::string suffix = split == std::string::npos ? std::string() : original.substr(split);

    if (visible.empty()) {
        return label;
    }

    const char* translated = SmatchetLocalization::TranslateSource(visible.c_str());
    if (std::strcmp(translated, visible.c_str()) == 0) {
        return label;
    }

    if (!suffix.empty()) {
        return StoreTempString(std::string(translated) + suffix);
    }
    return StoreTempString(JoinWithStableId(translated, visible.c_str(), windowTitle ? "###" : "##"));
}

} // namespace

namespace SmatchetLocalization {

const std::vector<LanguageInfo>& AvailableLanguages() {
    static const std::vector<LanguageInfo> languages = {{"en-US", "English"}, {"fr-FR", u8"Français"}};
    return languages;
}

std::string NormalizeLanguageCode(const std::string& code) {
    const std::string normalized = ToLowerAscii(TrimAscii(code));
    if (normalized == "fr" || normalized == "fr-fr" || normalized == "fr_fr") {
        return "fr-FR";
    }
    return "en-US";
}

void SetLanguage(const std::string& code) {
    const std::string normalized = NormalizeLanguageCode(code);
    std::lock_guard<std::mutex> lock(LocalizationMutex());
    CurrentLanguageRef() = normalized;
    LoadOverridesLocked(normalized);
}

const std::string& GetLanguage() { return CurrentLanguageRef(); }

const char* T(const char* key, const char* englishFallback) {
    if (!key || key[0] == '\0') {
        return englishFallback ? englishFallback : "";
    }
    std::lock_guard<std::mutex> lock(LocalizationMutex());
    const auto& overrides = OverridesRef();
    auto overrideIt = overrides.find(key);
    if (overrideIt != overrides.end()) {
        // CPP_CODE_AUDIT.md #25: copy via StoreTempString — see TranslateEntryLocked's
        // doc comment for why a raw pointer into `overrides` can't outlive this lock.
        return StoreTempString(overrideIt->second);
    }
    const auto& byKey = EntriesByKey();
    auto entryIt = byKey.find(key);
    if (entryIt != byKey.end()) {
        return TranslateEntryLocked(*entryIt->second, englishFallback);
    }
    if (CurrentLanguageRef() != "en-US" && MissingKeysRef().insert(key).second) {
        LOG_WARN("Localization: missing translation key '%s'.", key);
    }
    return englishFallback ? englishFallback : "";
}

const char* TranslateSource(const char* englishSource) {
    if (!englishSource) {
        return "";
    }
    std::lock_guard<std::mutex> lock(LocalizationMutex());
    const auto& overrides = OverridesRef();
    auto overrideIt = overrides.find(englishSource);
    if (overrideIt != overrides.end()) {
        // CPP_CODE_AUDIT.md #25: copy via StoreTempString — see TranslateEntryLocked's
        // doc comment for why a raw pointer into `overrides` can't outlive this lock.
        return StoreTempString(overrideIt->second);
    }
    const auto& byEnglish = EntriesByEnglish();
    auto entryIt = byEnglish.find(englishSource);
    if (entryIt != byEnglish.end()) {
        return TranslateEntryLocked(*entryIt->second, englishSource);
    }
    return englishSource;
}

const char* Label(const char* key, const char* englishFallback, const char* stableId) {
    return StoreTempString(JoinWithStableId(T(key, englishFallback), stableId, "##"));
}

const char* WindowTitle(const char* key, const char* englishFallback, const char* stableId) {
    return StoreTempString(JoinWithStableId(T(key, englishFallback), stableId, "###"));
}

const char* LabelFromSource(const char* label) { return BuildLabelFromSource(label, false); }

const char* WindowTitleFromSource(const char* title) { return BuildLabelFromSource(title, true); }

// Extract the ordered list of printf conversion-specifier tokens from a format
// string (each token is the run from '%' through its conversion char; `%%` is a
// literal and produces no token). Used to compare a translated override against
// the trusted English literal before either is handed to vsnprintf.
static std::vector<std::string> ConversionSpecifiers(const char* fmt) {
    std::vector<std::string> specs;
    if (fmt == nullptr) {
        return specs;
    }
    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            continue;
        }
        const char* start = p++;
        if (*p == '%') { // "%%" — literal percent, not a conversion
            continue;
        }
        // flags, width/precision (incl. '*'), length modifiers — consume up to the
        // conversion char (the first alphabetic that terminates the specifier).
        while (*p != '\0' && std::strchr("-+ #0123456789.*hljztLqI", *p) != nullptr) {
            ++p;
        }
        if (*p == '\0') { // truncated specifier — treat the whole tail as one token
            specs.emplace_back(start);
            break;
        }
        specs.emplace_back(start, static_cast<std::size_t>(p - start) + 1); // include conversion char
    }
    return specs;
}

// True iff `translated` carries exactly the same conversion-specifier sequence as
// the trusted `englishLiteral`. A mismatch means the override added/changed/removed
// a specifier, so feeding it to vsnprintf would consume varargs that were never
// supplied (or a `%n` write) — Pillar 3 / arbitrary-write guard.
static bool FormatSpecifiersMatch(const char* translated, const char* englishLiteral) {
    return ConversionSpecifiers(translated) == ConversionSpecifiers(englishLiteral);
}

const char* TranslateSourceAsFormat(const char* englishSource) {
    if (!englishSource) {
        return "";
    }
    const char* translated = TranslateSource(englishSource);
    // SECURITY: same guard as Format() (CPP_CODE_AUDIT.md #7) — TranslateSource's override
    // is attacker-influenceable and this call's result is about to be handed to a printf-
    // family sink (SmatchetLocalizedImGui's Text*/SetTooltip/SliderInt wrappers). Only use it
    // as the format string when its conversion-specifier sequence is identical to the trusted
    // englishSource; otherwise fall back to englishSource itself, whose specifiers the caller's
    // varargs always match.
    if (translated == englishSource || FormatSpecifiersMatch(translated, englishSource)) {
        return translated;
    }
    return englishSource;
}

const char* Format(const char* key, const char* englishFallbackFmt, ...) {
    const char* translated = T(key, englishFallbackFmt);
    // SECURITY: a locale override (Locales/<lang>.json) is attacker-influenceable.
    // Only use it as the vsnprintf format when its conversion-specifier sequence is
    // identical to the trusted English literal the caller passed; otherwise a crafted
    // override (e.g. "%s %s %n") would read/write varargs that don't exist. On
    // mismatch, fall back to the caller's literal — the varargs always match it.
    const char* fmt = FormatSpecifiersMatch(translated, englishFallbackFmt) ? translated : englishFallbackFmt;

    va_list args;
    va_start(args, englishFallbackFmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, argsCopy);
    va_end(argsCopy);
    if (needed < 0) {
        char fallback[4096];
        std::vsnprintf(fallback, sizeof(fallback), fmt, args);
        va_end(args);
        return StoreTempString(fallback);
    }
    std::vector<char> buf(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);
    return StoreTempString(std::string(buf.data()));
}

} // namespace SmatchetLocalization
