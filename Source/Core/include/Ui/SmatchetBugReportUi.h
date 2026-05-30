#ifndef SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H
#define SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H

// "Log a Bug" modal — description + optional screenshot + egress preview, files
// to the fixed dev GitHub repo via diagnostics::SubmitBugReport on a worker
// thread. Core/Ui, dual-target (no GL/GLFW; screenshot toggle hidden on DX12).
// docs/plans/active/log-a-bug-github.md Slice 4.

class AppController;
struct UiDrawSession;

void SmatchetBugReportUi_Draw(AppController& app, UiDrawSession& d);

#endif // SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H
