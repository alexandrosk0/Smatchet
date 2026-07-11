#ifndef SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H
#define SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H

// "Log a Bug" modal — description + optional screenshot + egress preview, files
// to the fixed dev GitHub repo via diagnostics::SubmitBugReport on a worker
// thread. Core/Ui, dual-target (no GL/GLFW; screenshot toggle hidden on DX12).
// docs/plans/shipped/log-a-bug-github.md Slice 4.

class IAppMeta;      // narrow AppController facets — the modal needs the worker/post idiom
class IAppThreading; // plus the app version the service stamps into the report
struct UiDrawSession;

void SmatchetBugReportUi_Draw(IAppThreading& app, IAppMeta& meta, UiDrawSession& d);

#endif // SMATCHET_UI_SMATCHET_BUG_REPORT_UI_H
