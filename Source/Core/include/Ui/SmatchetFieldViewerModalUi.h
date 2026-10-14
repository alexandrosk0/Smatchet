#pragma once

#include <memory>
#include <string>

class AppController;
struct CachedTicket;

/// Generic "field viewer" modal: one top-level popup shell that owns the popup lifecycle (deferred
/// OpenPopup at top-level depth, centered 0.5x0.6 window, Close / Esc / title-bar X, an optional
/// discard-confirm) while a per-field content provider draws the body. The grid cell calls
/// `OpenFieldViewerForField`; the once-per-frame top-level `RenderFieldViewerModal` renders it, so the
/// modal survives the originating cell scrolling out of view. Comments and History use it.
/// Pillar 2 — a content provider must not do network/file IO from `DrawBody`.
class IFieldViewerContent {
  public:
    virtual ~IFieldViewerContent() = default;
    /// Window header text (without the issue id), e.g. "Comments", "History".
    virtual std::string Title() const = 0;
    /// Fills the region above the Close row. Must not call `OpenFieldViewer` / `OpenFieldViewerForField`
    /// / `OpenCommentsModal`: replacing the viewer destroys the content while it is still drawing.
    virtual void DrawBody(AppController& app, bool readOnlyMode) = 0;
    /// True while closing would lose input the user typed: the shell asks before closing.
    virtual bool HasUnsavedInput() const { return false; }
    virtual const char* DiscardConfirmTitle() const { return ""; }
    virtual const char* DiscardConfirmText() const { return ""; }
    virtual const char* DiscardButtonLabel() const { return ""; }
    virtual const char* KeepButtonLabel() const { return ""; }
    /// Called by the shell when the viewer closes AND when another viewer replaces this one. Cleanup
    /// belongs here, never in a destructor: the shell's file-static owner is destroyed at static
    /// destruction time, possibly after the state a destructor would touch (another TU).
    virtual void OnClose() {}
};

/// Makes `content` the open viewer for `issueId` / `fieldId` (calling the previous content's
/// `OnClose()` first — without the discard confirm, so a replaced Comments draft is dropped; the open
/// modal blocks grid input, so only code can replace it). Safe to call from inside a table cell: the
/// popup opens on the next `RenderFieldViewerModal`.
void OpenFieldViewer(std::unique_ptr<IFieldViewerContent> content, const std::string& issueId,
                     const std::string& fieldId);

/// Opens the viewer registered for `fieldId` on `ticket`; false when the field has none.
bool OpenFieldViewerForField(AppController& app, const CachedTicket& ticket, const std::string& fieldId);

/// Renders the open viewer once per frame from a stable top-level location (focused pane only; the
/// popup stack is global). `readOnlyMode` is passed through to the content.
void RenderFieldViewerModal(AppController& app, bool readOnlyMode);

/// Test hook: a copy of the shell's state. UI thread only.
struct FieldViewerSnapshot {
    bool Active = false;
    std::string FieldId;
    std::string IssueId;
    std::string Title;
};
FieldViewerSnapshot GetFieldViewerSnapshotForTests();
