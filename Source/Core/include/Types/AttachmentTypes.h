#ifndef SMATCHET_TYPES_ATTACHMENT_TYPES_H
#define SMATCHET_TYPES_ATTACHMENT_TYPES_H

// Types/AttachmentTypes.h — attachment value type + host-callback aliases relocated out of
// AppController.h (fan-in Phase 3b, docs/plans/appcontroller-fan-in.md). Rank-0 leaf (non-AppController
// basename, non-layer-prefix dir). All five were nested in the AppController class; moved to the global
// namespace here and re-exported via in-class using-aliases so the ~115 includers keep naming
// AppController::AttachmentDescriptor / AppController::AttachmentViewerHandler etc. unchanged.
// AttachmentDescriptor MUST precede AttachmentCollectionHandler (which references it).

#include <functional>
#include <string>
#include <vector>

struct AttachmentDescriptor {
    std::string Filename;
    std::string Url;
    std::string MimeType;
};

using AttachmentViewerHandler =
    std::function<void(const std::string& localPath, const std::string& mimeType, const std::string& filename)>;

using AttachmentPreviewHandler = std::function<bool(const std::string& localPath, const std::string& mimeType,
                                                    const std::string& filename, const std::string& sourceUrl)>;

using AttachmentCollectionHandler = std::function<void(const std::vector<AttachmentDescriptor>& attachments)>;

using OpenFilePathsHandler =
    std::function<void(bool allowMultiple, const std::string& initialDirectoryUtf8,
                       std::function<void(std::vector<std::string> absolutePathsUtf8)> onComplete)>;

#endif // SMATCHET_TYPES_ATTACHMENT_TYPES_H
