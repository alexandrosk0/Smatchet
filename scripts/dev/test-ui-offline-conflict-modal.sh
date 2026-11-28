#!/usr/bin/env bash
# test-ui-offline-conflict-modal.sh — bucket-E driver for the offline-conflict resolver modal
# (tests/ui/offline_conflict_modal_panes.test.cpp, group OfflineConflictModalPanes).
# Thin wrapper over test-ui-jira-deterministic-backend.sh: the basic-grid fixture (the host pane
# needs SMAT-1 loaded) plus the throwaway in-memory local cache (SMATCHET_UITEST_WITH_LOCAL_CACHE=1),
# without which every case SKIPs before it can queue an offline edit. Exit codes are the wrapped
# driver's (0 pass, 1 fail, 2 binary missing / UI tests not built).
# ui-test-home: opt-out — launches no exe itself; the exec'd jira driver runs the shared preamble and isolates the profile.
set -euo pipefail
export UI_TEST_FILTER="${UI_TEST_FILTER:-OfflineConflictModalPanes}"
export SMATCHET_TEST_JIRA_BACKEND_FIXTURE="${SMATCHET_OFFLINE_CONFLICT_FIXTURE:-tests/fixtures/jira_backend/basic-grid.json}"
export SMATCHET_UITEST_WITH_LOCAL_CACHE=1
# The wrapped driver passes no --timeout, so --spawn would default to 40 s for a 600-frame run; the
# group's ~20 cases can run past that under software GL in CI.
export SMATCHET_SPAWN_TIMEOUT_MS="${SMATCHET_SPAWN_TIMEOUT_MS:-120000}"
exec bash "$(dirname "$0")/test-ui-jira-deterministic-backend.sh"
