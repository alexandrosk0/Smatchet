#pragma once

// Quick-create issue popup — a lightweight create flow (project / type / summary /
// description) whose description arrives prefilled with the host-engine context
// snapshot (EngineHostContext.h) filtered through the Preferences toggles
// (EngineContextFormat.h). Opened by the `issue.quick_create.open` command
// (default hotkey Ctrl+Shift+T); submits through the backend-agnostic
// AppController::CreateIssueAsync with an offline-queue fallback.
//
// docs/plans/quick-create-issue-unreal-context.md.

class AppController;
struct UiDrawSession;

/// Draw the popup for this frame (early-out when hidden). Also polls the
/// in-flight create future so a submit finishes (toast) even if the window
/// was closed meanwhile. UI-thread only.
void SmatchetQuickCreateIssueUi_Draw(AppController& app, UiDrawSession& d);
