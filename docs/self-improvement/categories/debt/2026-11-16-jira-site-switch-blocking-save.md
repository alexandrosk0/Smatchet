- 2026-11-16 · code-review on #2444 · [debt] · P3 — the Jira site switch saves the config on the UI thread

Details:
`SmatchetUI::switchActiveJiraSite` (`Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`) calls the
blocking `ConfigManager::Save` (io mutex + read-modify-write of the config JSON) on the UI thread before
it re-syncs, because `JiraClient` paths re-read the `ConfigManager::Load()` cache and must see the new
site first (persist-before-sync). It was the Views - Jira Domain combo's behaviour; since #2444 the
grid header "+ ▾" Jira site rows call it too. One-shot per click, so not a steady-state hitch, but it
is the synchronous-file-I/O class `Source/Core/src/Ui/AGENTS.md` § Pillar 2 lists.

Concrete next action:
- Route the save through `smatchet::config_save::EnqueueTrackerConfig` (as #2026 did for the
  cross-backend `TrackerType` re-point) and make the following `SyncWithBackend` wait on a flush of
  that write (or hand the sync the in-memory config so the Load cache is not consulted). Cover it with
  the bucket-E `GridPanes/TwoJiraSitesRowOpensPaneOnSite` case, which exercises the switch.

Status: open
Last-reviewed: 2026-11-16
