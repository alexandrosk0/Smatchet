#pragma once

// IAttachmentAppUpdateDeps — narrow interface bundle exposing every AppController-side dependency
// that AttachmentAppUpdateService reaches into while opening attachments and running the GitHub
// app-update check. Mirrors the ITicketSyncDeps / IEditMetaDeps / IFieldEditDeps / IConnectivityDeps
// template: AppController constructs GridContextDepsAdapter (which implements this interface
// alongside the five existing ones) and hands it to AttachmentAppUpdateService. The service holds an
// IAttachmentAppUpdateDeps& reference and never touches AppController internals directly.
// The surface is deliberately tiny. The attachment + app-update bodies only need three host-side
// capabilities: read the optional host attachment callbacks (the HostCallbacks struct grouped in
// Phase 0), invoke the URL-open fallback, and request app quit after a successful installer launch.
// HostCallbacks is surfaced by const reference so the service reads AttachmentViewer /
// AttachmentPreview / AttachmentCollection exactly as the prior in-class bodies read hostCallbacks_,
// preserving the publish-once UI-thread discipline (the host assigns each callback once at setup and
// the service reads them on the UI thread).
// Lifetime contract mirrors the sibling deps interfaces: the implementer (GridContextDepsAdapter) is
// owned by AppController and outlives the service. No cross-thread handles are surfaced here; the cpr
// HTTP downloads run synchronously inside the service .cpp on whichever thread the caller dispatched
// the method onto (callers already wrap the blocking installer download in LaunchBackgroundTask).
// Test fixtures implement this interface directly (see tests/support/FakeAttachmentAppUpdateDeps.h)
// so unit tests can exercise the dispatch + handler-selection logic without constructing an
// AppController. Any new method added here MUST be implemented in that fake too.

#include <string>

struct HostCallbacks;

class IAttachmentAppUpdateDeps {
  public:
    virtual ~IAttachmentAppUpdateDeps() = default;

    /// Read-only access to the optional host-provided callbacks (attachment viewer / preview /
    /// collection). The service selects the open path off these exactly as the prior in-class bodies
    /// read AppController::hostCallbacks_. UI-thread publish-once discipline preserved.
    virtual const HostCallbacks& Host() const = 0;

    /// URL-open fallback (scheme-allowlisted in the AppController body the adapter forwards to). Used
    /// when no attachment handler path applies, or as the failure fallback after a failed download.
    virtual void OpenUrl(const std::string& url) const = 0;

    /// Request the host quit the app — invoked after a downloaded installer launches successfully so
    /// the running instance exits before the installer replaces it. No-op when the host set no quit
    /// handler.
    virtual void RequestAppQuit() const = 0;
};
