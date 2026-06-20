#ifndef SMATCHET_TYPES_HOST_CALLBACKS_H
#define SMATCHET_TYPES_HOST_CALLBACKS_H

// Types/HostCallbacks.h — optional host-provided callbacks, grouped out of AppController.h
// (Phase 0 of the AppController god-object decomposition, docs/plans/shipped/
// appcontroller-god-object-decomposition.md). Rank-0 leaf (non-AppController basename,
// non-layer-prefix dir): pulls only <functional>/<string> + the attachment handler aliases.
// Pure member-grouping: the seven public setters on AppController (SetOpenUrlHandler, ...,
// SetOpenFilePathsHandler) are unchanged and now back onto this struct. Publish-once
// discipline is unchanged — the host assigns each callback once at setup (UI thread,
// pre-worker) and consumers read on the UI thread.

#include <functional>
#include <string>

#include "Types/AttachmentTypes.h"

struct HostCallbacks {
    std::function<void(const std::string&)> OpenUrl;
    std::function<void()> CloseEmbeddedUi;
    std::function<void()> RequestAppQuit;
    AttachmentViewerHandler AttachmentViewer;
    AttachmentPreviewHandler AttachmentPreview;
    AttachmentCollectionHandler AttachmentCollection;
    OpenFilePathsHandler OpenFilePaths;
};

#endif // SMATCHET_TYPES_HOST_CALLBACKS_H
