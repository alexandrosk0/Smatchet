#ifndef SMATCHET_INTERFACES_IAPP_ATTACHMENTS_H
#define SMATCHET_INTERFACES_IAPP_ATTACHMENTS_H

// Narrow facet for attachment open / preview-download — AppController fan-in Phase 5
// (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController implements it; the
// attach.* command TU depends on this instead of the full AppController.h. No per-frame
// method. Rank-0 leaf (Interfaces/); only string params/returns.

#include <string>

class IAppAttachments {
  public:
    virtual ~IAppAttachments() = default;

    virtual void OpenAttachment(const std::string& url, const std::string& filename,
                                const std::string& mimeType) = 0;
    virtual bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                              const std::string& mimeType, std::string* outError = nullptr) = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_ATTACHMENTS_H
