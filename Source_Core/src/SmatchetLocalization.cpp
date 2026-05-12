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

    // PR 5 of docs/design/remove-global-project-key.md: badge for dead-letter rows whose draft
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
     u8"Les modifications de grille et les commentaires rapides restent désactivés tant que le suivi n'est pas joignable."},

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
    {"menu.source_blame", "Source Blame...", u8"Blame source..."},
    {"menu.sync_audit", "Sync Audit...", u8"Audit de synchronisation..."},
    {"menu.runtime_log", "Runtime Log", u8"Journal d'exécution"},
    {"menu.performance_monitor", "Performance Monitor...", u8"Moniteur de performances..."},
    {"menu.read_only_mode", "Read-only Mode", u8"Mode lecture seule"},
    {"menu.preferences", "Preferences...", u8"Préférences..."},
    {"menu.performance", "Performance...", u8"Performances..."},
    {"menu.windows", "Windows", u8"Fenêtres"},
    {"menu.open_views", "Open Views...", u8"Ouvrir les vues..."},
    {"menu.blame_analysis", "Blame Analysis...", u8"Analyse de blame..."},
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
    {"prefs.tab.blame_analysis", "Blame Analysis", u8"Analyse de blame"},
    {"prefs.tab.local_data", "Local data", u8"Données locales"},
    {"prefs.local_data.help",
     "Stored tickets, offline create queues, and pending field edits live in a local SQLite file. "
     "Recreating it clears that data only; tracker credentials and views are not removed. A full "
     "issue refresh runs afterward.",
     u8"Les tickets en cache, les files de création hors ligne et les modifications de champs en "
     u8"attente sont stockés dans un fichier SQLite local. Le recréer efface uniquement ces données "
     u8"— pas les identifiants du suivi ni les vues. Une actualisation complète des tickets suit."},
    {"prefs.local_data.file_label", "Cache file:", u8"Fichier de cache :"},
    {"prefs.local_data.recreate", "Recreate database...", u8"Recréer la base..."},
    {"prefs.local_data.recreate_tooltip",
     "Permanently delete the local cache file and start with an empty database.",
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
    {"prefs.project_key", "Project Key", u8"Clé de projet"},
    {"prefs.url", "URL", u8"URL"},
    {"prefs.workspace_slug", "Workspace Slug", u8"Slug de l'espace de travail"},
    {"prefs.project_id_uuid", "Project ID (UUID)", u8"ID du projet (UUID)"},
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
    {"prefs.blame_comments", "Blame Comments", u8"Commentaires de blame"},
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

    {"field.clear", "<clear>", u8"<effacer>"},
    {"field.clear_all", "<clear all>", u8"<tout effacer>"},
    {"field.parent_only", "<parent only>", u8"<parent uniquement>"},
    {"field.no_labels", "(no labels discovered yet)", u8"(aucune étiquette découverte)"},
    {"field.no_matching_labels", "(no matching labels)", u8"(aucune étiquette correspondante)"},
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

    {"blame.click_cl", "Click to open this changelist in p4vc.", u8"Cliquez pour ouvrir cette changelist dans p4vc."},
    {"blame.loading_cl", "Loading CL info…", u8"Chargement des infos CL…"},
    {"blame.no_describe", "(no describe details)", u8"(aucun détail describe)"},
    {"blame.max_frames", "Max frames", u8"Nombre max de frames"},
    {"blame.p4_executable", "P4 executable", u8"Exécutable P4"},
    {"blame.p4vc_executable", "p4vc executable", u8"Exécutable p4vc"},
    {"blame.timelapse_cmd", "Timelapse cmd (optional)", u8"Commande Timelapse (facultative)"},
    {"blame.changelist_cmd", "Changelist cmd (optional)", u8"Commande changelist (facultative)"},
    {"blame.ai_chat_url", "AI chat URL (optional)", u8"URL du chat IA (facultative)"},
    {"blame.callstack_jira", "Callstack from Jira", u8"Pile d'appels depuis Jira"},
    {"blame.callstack_field", "Callstack field id (optional)", u8"ID du champ de pile d'appels (facultatif)"},
    {"blame.path_remap_from", "Path remap from", u8"Remapper le chemin depuis"},
    {"blame.path_remap_to", "Path remap to", u8"Remapper le chemin vers"},
    {"blame.save_settings", "Save settings", u8"Enregistrer les paramètres"},
    {"blame.reload_settings", "Reload settings", u8"Recharger les paramètres"},
    {"blame.ask_ai", "Ask AI", u8"Demander à l'IA"},
    {"blame.export_json", "Export JSON", u8"Exporter JSON"},
    {"blame.export_csv", "Export CSV", u8"Exporter CSV"},
    {"blame.callstack", "Callstack", u8"Pile d'appels"},
    {"blame.show_table", "Show Table", u8"Afficher le tableau"},
    {"blame.show_raw", "Show Raw Text", u8"Afficher le texte brut"},
    {"blame.at_changelist", "Before changelist (optional)", u8"Avant la changelist (facultatif)"},
    {"blame.process", "Process", u8"Traiter"},
    {"blame.assign_user", "Assign issue to user", u8"Assigner le ticket à l'utilisateur"},
    {"blame.add_context_comment", "Add blame context comment", u8"Ajouter un commentaire de contexte blame"},
    {"blame.quick_templates", "Quick comment templates", u8"Modèles de commentaires rapides"},
    {"blame.assign_and_comment", "Assign and add blame context", u8"Assigner et ajouter le contexte blame"},
    {"blame.groups", "Groups (best effort):", u8"Groupes (au mieux) :"},
    {"blame.none_permitted", "(none or not permitted)", u8"(aucun ou non autorisé)"},

    // PR 4b: project picker (draft + bulk import modal).
    {"draft.project", "Project", u8"Projet"},
    {"draft.project.placeholder", "(pick one)", u8"(choisir)"},
    {"draft.project.section.recent", "Recently used", u8"Récemment utilisés"},
    {"draft.project.section.all", "All projects", u8"Tous les projets"},
    {"draft.project.search", "Search...", u8"Rechercher..."},
    {"draft.project.loading", "Loading...", u8"Chargement..."},
    {"draft.project.submit_disabled_tooltip", "Pick a project", u8"Choisir un projet"},

    // PR 4b: active-view JQL bar project pill (Jira only).
    {"view.projectPill.single", "Project: %s", u8"Projet : %s"},
    {"view.projectPill.multi", "Project: multi", u8"Projet : multiple"},
    {"view.projectPill.tooltip.single",
     "Active view is scoped to a single project. Click to switch project.",
     u8"La vue active est limitée à un seul projet. Cliquer pour changer de projet."},
    {"view.projectPill.tooltip.multi",
     "Active view spans multiple projects (or has no project clause). Click to pick a single project.",
     u8"La vue active couvre plusieurs projets (ou n'a aucune clause de projet). Cliquer pour choisir un seul projet."},
    {"view.projectPill.empty", "(no cached projects)", u8"(aucun projet en cache)"},

    // PR 4b: bulk import modal.
    {"bulkImport.chooseProject.title", "Choose target project for bulk import",
     u8"Choisir le projet cible pour l'import en masse"},
    {"bulkImport.chooseProject.cancel", "Cancel", u8"Annuler"},
    {"bulkImport.chooseProject.confirm", "Use this project", u8"Utiliser ce projet"},
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

const char* TranslateEntryLocked(const TranslationEntry& entry, const char* fallback) {
    const auto& overrides = OverridesRef();
    auto overrideIt = overrides.find(entry.Key);
    if (overrideIt != overrides.end()) {
        return overrideIt->second.c_str();
    }
    if (entry.English) {
        overrideIt = overrides.find(entry.English);
        if (overrideIt != overrides.end()) {
            return overrideIt->second.c_str();
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
        return overrideIt->second.c_str();
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
        return overrideIt->second.c_str();
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

const char* Format(const char* key, const char* englishFallbackFmt, ...) {
    const char* fmt = T(key, englishFallbackFmt);

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
