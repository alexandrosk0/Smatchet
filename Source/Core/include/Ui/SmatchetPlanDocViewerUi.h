#ifndef SMATCHET_PLAN_DOC_VIEWER_UI_H
#define SMATCHET_PLAN_DOC_VIEWER_UI_H

// Plan-doc viewer — read-only MarkdownPreviewRender surface over
// docs/design/*.md + docs/adr/*.md, plus externally supplied markdown files
// (drag-and-drop onto the app window, or the in-window "Open..." dialog).
// Surface contract: one window keyed off `UiDrawSession::showPlanDocViewer`.
// File picker (combo, scanned docs sorted alpha, then session-dropped files)
// selects the active doc; reads run on a worker via AsyncLoadGatePure.
// This is a viewer only — no editing, no favourites, no per-file recent list,
// no in-doc TOC. Keep it that way per the docs/plans/active/<slug>.md plan §
// Risks § R6 ("scope creep").

#include <string>
#include <vector>

struct UiDrawSession;

namespace smatchet {

// Render the plan-doc viewer window when `d.showPlanDocViewer` is true.
// No-op otherwise. Safe to call every frame.
void DrawPlanDocViewer(UiDrawSession& d);

// Open an arbitrary markdown file (absolute path, UTF-8) in the Plan Docs
// viewer: adds it to the picker's session-dropped group (deduped by
// normalized path), selects it, and shows + focuses the window. No file I/O
// here (Pillar 2) — the viewer's per-frame async load reads it; an
// unreadable path surfaces the viewer's normal error body.
void PlanDocViewerOpenExternalFile(UiDrawSession& d, const std::string& absPathUtf8);

// Batch form shared by the drop callback and the "Open..." dialog: every path
// joins the picker, the FIRST one becomes the active doc. Empty entries are
// skipped; an all-empty batch changes nothing (window not opened).
void PlanDocViewerOpenExternalFiles(UiDrawSession& d, const std::vector<std::string>& absPathsUtf8);

} // namespace smatchet

#endif // SMATCHET_PLAN_DOC_VIEWER_UI_H
