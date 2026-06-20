#pragma once

// AttachmentAppUpdateService — owns the attachment-open + GitHub app-update logic extracted verbatim
// from AppController per the AppController god-object decomposition plan (Phase 4), mirroring the
// OfflineQueueService / TicketSyncService / EditMetaCacheService / FieldEditPipelineService /
// ConnectivityMonitorService template. The service holds an IAttachmentAppUpdateDeps& (typically
// backed by GridContextDepsAdapter) for the AppController-side state it reaches: the optional host
// callbacks, the URL-open fallback, and the app-quit request. AppController's public attachment +
// app-update surface keeps the same eight signatures but its bodies become thin delegators that
// forward into this service.
// One combined service rather than two: the attachment cluster and the app-update cluster share the
// same tiny dependency surface (the host callbacks + OpenUrl + RequestAppQuit) and neither owns a
// long-lived mutex, so a single service keeps one interface, one adapter base, one fake, and one
// test file — splitting would double the wiring for no behavioural or testability gain.
// Concurrency: the service holds no mutex and no member state. Every method is a pure function of its
// arguments plus the host callbacks read live through the deps interface. The cpr HTTP downloads run
// synchronously on the calling thread (callers already dispatch the blocking installer download via
// LaunchBackgroundTask). The file-local download / temp-path / semantic-version helpers move with the
// methods into the service .cpp (they were used only by these eight methods).
// Lifetime contract mirrors the sibling services: AppController owns the service via std::unique_ptr
// and outlives it; the IAttachmentAppUpdateDeps& (GridContextDepsAdapter) outlives this service
// (declared after it, destroyed after it).

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Types/AppUpdateTypes.h"   // AppUpdateInfo
#include "Types/AttachmentTypes.h"  // AttachmentDescriptor

class IAttachmentAppUpdateDeps;

class AttachmentAppUpdateService {
  public:
    explicit AttachmentAppUpdateService(IAttachmentAppUpdateDeps& deps);

    /// Dispatch a multi-attachment collection to the host collection handler when present; otherwise
    /// open the first attachment via OpenAttachment. No-op for an empty collection.
    void ShowAttachmentCollection(const std::vector<AttachmentDescriptor>& attachments);

    /// Open one attachment without leaking Basic-Auth headers to a browser: when a viewer/preview
    /// handler exists, download to a temp file then dispatch to the viewer (or in-app image preview);
    /// otherwise fall back to OpenUrl. Download failure also falls back to OpenUrl.
    void OpenAttachment(const std::string& url, const std::string& filename, const std::string& mimeType);

    /// Download to a temp file then open it in the OS default app (matches the Unreal attachment
    /// viewer). Download failure falls back to OpenUrl.
    void OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename,
                                      const std::string& mimeType);

    /// Download a supported image attachment to a temp file and hand it to the in-app preview handler.
    /// Returns false (with an optional reason via outError) when no preview handler is set, the mime
    /// is unsupported, the download fails, or the handler rejects the file.
    bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename,
                                      const std::string& mimeType, std::string* outError = nullptr);

    /// Compile-time app version (SMATCHET_APP_VERSION) or "0.0.0" when undefined.
    std::string GetAppVersion() const;

    /// Compile-time default GitHub release repo (SMATCHET_GITHUB_RELEASE_REPO) or the hard-coded
    /// fallback when undefined.
    std::string GetGitHubReleaseRepo() const;

    /// Query the GitHub releases API for the newest non-draft release and compare against the current
    /// version. Blocking HTTP; the result reports whether an update is available + the Windows
    /// installer asset. includePrerelease admits prerelease tags into the comparison.
    AppUpdateInfo CheckForAppUpdate(bool includePrerelease = false) const;

    /// Download the installer to the temp dir then launch it via ShellExecute and request app quit.
    /// Blocking — callers dispatch this on a worker thread. The optional cancelFlag is polled inside
    /// the cpr write callback; when set the download aborts cleanly, the partial file is removed, and
    /// the method returns false with outError == "Download cancelled." Windows-only; other platforms
    /// return false with an unsupported-platform message.
    bool DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                          std::string& outError,
                                          std::shared_ptr<std::atomic<bool>> cancelFlag = {}) const;

  private:
    IAttachmentAppUpdateDeps& deps_;
};
