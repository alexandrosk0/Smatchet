#!/usr/bin/env bash
# test-ui-left-view-history.sh — bucket-E driver for the window listing issues that left a view.
# Thin wrapper over test-ui-jira-deterministic-backend.sh: same exe, isolated user data and JSON
# parsing, with the left-view fixture and the LeftViewHistory test group. Exit codes are the wrapped
# driver's (0 pass, 1 fail, 2 binary missing / UI tests not built).
# ui-test-home: opt-out — launches no exe itself; the exec'd jira driver runs the shared preamble and isolates the profile.
set -euo pipefail
export UI_TEST_FILTER="${UI_TEST_FILTER:-LeftViewHistory}"
export SMATCHET_TEST_JIRA_BACKEND_FIXTURE="${SMATCHET_LEFT_VIEW_FIXTURE:-tests/fixtures/jira_backend/left-view-history.json}"
exec bash "$(dirname "$0")/test-ui-jira-deterministic-backend.sh"
