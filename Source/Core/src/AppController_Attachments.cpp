#include "AppController.h"

#include "AttachmentAppUpdateService.h"

// AppController attachment surface — thin delegators forwarding into AttachmentAppUpdateService
// (AppController god-object decomposition Phase 4). The full implementation (download path,
// host-handler selection, temp-file helpers) moved verbatim into AttachmentAppUpdateService.cpp; the
// public signatures are preserved so the UI / command / grid call sites compile unchanged.

void AppController::ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments) {
    attachmentAppUpdate_->ShowAttachmentCollection(attachments);
}

void AppController::OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType) {
    attachmentAppUpdate_->OpenAttachment(url, filename, mimeType);
}

bool AppController::OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename,
                                                 const std::string& mimeType, std::string* outError,
                                                 bool* outFellBackToUrl) {
    return attachmentAppUpdate_->OpenAttachmentInSystemViewer(url, filename, mimeType, outError, outFellBackToUrl);
}

bool AppController::DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                                const std::string& mimeType, std::string* outError) {
    return attachmentAppUpdate_->DownloadAttachmentForPreview(url, filename, mimeType, outError);
}
