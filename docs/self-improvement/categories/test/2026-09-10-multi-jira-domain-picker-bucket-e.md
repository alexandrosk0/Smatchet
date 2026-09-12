- 2026-09-10 · orchestrator · [test] · P3 — Views - Jira domain picker has no bucket-E case (Linux cloud agent cannot launch Smatchet.exe)

  Multiple Jira backend domains landed with bucket-A coverage (host normalize, inherit, cache
  key, Save/Load extras, same-kind host recreate). The domain combo and extra-sites editor are
  standard ImGui in `SmatchetViewsDashboardUi.cpp` / `SmatchetPreferencesUi.cpp`. This Linux
  container cannot run the MSVC UI-test rig, so the ship-loop visual-validation exception
  (`docs/agent-rules/ship-loops.md` § exception 5) stays a named manual residue.

  Concrete next action: add a `preferences_tracker_switch`-style bucket-E scenario that adds a
  second Jira domain in Preferences → Tracker, opens Views - Jira, switches the Domain combo,
  and asserts the live origin + cache namespace follow. Gate it on the Windows UI-test preset.

  Status: open
  Last-reviewed: 2026-09-10
