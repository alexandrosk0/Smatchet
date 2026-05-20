#ifndef SMATCHET_PLAN_DOC_VIEWER_UI_H
#define SMATCHET_PLAN_DOC_VIEWER_UI_H

// Plan-doc viewer — read-only TextEditor over docs/design/*.md and
// docs/adr/*.md. Markdown coloring driven by
// TextEditor::LanguageDefinition::Markdown().
//
// Surface contract: one window keyed off `UiDrawSession::showPlanDocViewer`.
// File picker (combo, sorted alpha) selects the active doc; the viewer reads
// the file once on combo change (single-buffer reuse).
//
// This is a viewer only — no editing, no favourites, no per-file recent list,
// no in-doc TOC. Keep it that way per the docs/design/<slug>.md plan §
// Risks § R6 ("scope creep").

struct UiDrawSession;

namespace smatchet {

// Render the plan-doc viewer window when `d.showPlanDocViewer` is true.
// No-op otherwise. Safe to call every frame.
void DrawPlanDocViewer(UiDrawSession& d);

} // namespace smatchet

#endif // SMATCHET_PLAN_DOC_VIEWER_UI_H
