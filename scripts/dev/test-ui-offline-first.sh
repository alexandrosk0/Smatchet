#!/usr/bin/env bash
# test-ui-offline-first.sh — bucket-E driver for the Quality Pillar 6 (offline-first) UI tests.
# Thin wrapper over test-ui-jira-deterministic-backend.sh: same exe, isolated user data and JSON
# parsing, with the offline-first fixture and the OfflineFirst test group. Exit codes are the wrapped
# driver's (0 pass, 1 fail, 2 binary missing / UI tests not built).
set -euo pipefail
export UI_TEST_FILTER="${UI_TEST_FILTER:-OfflineFirst}"
export SMATCHET_TEST_JIRA_BACKEND_FIXTURE="${SMATCHET_OFFLINE_FIRST_FIXTURE:-tests/fixtures/jira_backend/offline-first.json}"
exec bash "$(dirname "$0")/test-ui-jira-deterministic-backend.sh"
