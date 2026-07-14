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

#include "Types/AppUpdateTypes.h"  // AppUpdateInfo
#include "Types/AttachmentTypes.h" // AttachmentDescriptor

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
    /// viewer). Download failure falls back to OpenUrl (reported via *outFellBackToUrl so the UI
    /// can say so — P2-H8). Returns true only when the local viewer launch succeeded; failure
    /// detail lands in *outError when provided.
    bool OpenAttachmentInSystemViewer(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError = nullptr, bool* outFellBackToUrl = nullptr);

    /// Download a supported image attachment to a temp file and hand it to the in-app preview handler.
    /// Returns false (with an optional reason via outError) when no preview handler is set, the mime
    /// is unsupported, the download fails, or the handler rejects the file.
    bool DownloadAttachmentForPreview(const std::string& url, const std::string& filename, const std::string& mimeType,
                                      std::string* outError = nullptr);

    /// Compile-time app version (SMATCHET_APP_VERSION) or "0.0.0" when undefined.
    std::string GetAppVersion() const;

    /// Compile-time default GitHub release repo (SMATCHET_GITHUB_RELEASE_REPO) or the hard-coded
    /// fallback when undefined.
    std::string GetGitHubReleaseRepo() const;

    /// Query the GitHub releases API for the newest non-draft release and compare against the current
    /// version. Blocking HTTP; the result reports whether an update is available + the Windows
    /// installer asset. includePrerelease admits prerelease tags into the comparison.
    AppUpdateInfo CheckForAppUpdate(bool includePrerelease = false) const;

    /// Download the installer to the temp dir, verify its Authenticode signature (WinVerifyTrust —
    /// an unverified installer is deleted and refused; SMATCHET_UPDATE_ALLOW_UNSIGNED=1 forgives
    /// everything except a bad digest, see smatchet::app_update::ShouldLaunchDownloadedInstaller),
    /// then launch it via ShellExecute and request app quit. Blocking — callers dispatch this on a
    /// worker thread. The optional cancelFlag is polled inside the cpr write callback; when set the
    /// download aborts cleanly, the partial file is removed, and the method returns false with
    /// outError == "Download cancelled." Windows-only; other platforms return false with an
    /// unsupported-platform message.
    bool DownloadAndLaunchInstallerUpdate(const std::string& downloadUrl, const std::string& assetName,
                                          std::string& outError,
                                          std::shared_ptr<std::atomic<bool>> cancelFlag = {}) const;

  private:
    IAttachmentAppUpdateDeps& deps_;
};

namespace smatchet {
namespace app_update {

// Reduce an untrusted GitHub release-asset name to a bare, separator-free filename that is safe to
// concatenate directly onto the %TEMP% directory before the installer is written and ShellExecute-
// launched. Strips any directory prefix (both '/' and '\\'), neutralizes Windows path/reserved
// metacharacters and control bytes, and rejects pure-traversal tokens ("." / ".."), substituting a
// fixed fallback when nothing safe remains. This blocks a crafted asset such as
// "..\\..\\...\\Startup\\x-windows-setup.exe" (which still satisfies the "-windows-setup.exe"
// substring filter) from escaping %TEMP%. Pure string logic: lives inline in the header so the
// doctest rig exercises it without linking the cpr-coupled service TU.
inline std::string SanitizeInstallerFilename(const std::string& rawName) {
    std::string base = rawName;
    const std::string::size_type lastSep = base.find_last_of("/\\");
    if (lastSep != std::string::npos) {
        base = base.substr(lastSep + 1);
    }
    for (char& ch : base) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        const bool reserved = ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
                              ch == '<' || ch == '>' || ch == '|';
        if (reserved || uch < 0x20) {
            ch = '_';
        }
    }
    if (base.empty() || base == "." || base == "..") {
        return std::string("SmatchetUpdateSetup.exe");
    }
    return base;
}

// Authenticode trust policy for the downloaded installer. The Win32 side (WinVerifyTrust in
// AttachmentAppUpdateService.cpp) reduces signature verification to a single status code; the pure
// functions below map that code to the launch decision plus a user-facing reason, so the policy is
// unit-testable off-Windows. The constants mirror the wintrust/wincrypt HRESULT values (the macros
// themselves are Windows-only headers).
const long kInstallerTrustOk = 0L;                                            // ERROR_SUCCESS
const long kInstallerTrustNoSignature = static_cast<long>(0x800B0100UL);      // TRUST_E_NOSIGNATURE
const long kInstallerTrustExpiredCert = static_cast<long>(0x800B0101UL);      // CERT_E_EXPIRED
const long kInstallerTrustUntrustedRoot = static_cast<long>(0x800B0109UL);    // CERT_E_UNTRUSTEDROOT
const long kInstallerTrustExplicitDistrust = static_cast<long>(0x800B0111UL); // TRUST_E_EXPLICIT_DISTRUST
const long kInstallerTrustBadDigest = static_cast<long>(0x80096010UL);        // TRUST_E_BAD_DIGEST
const long kInstallerTrustSecurityPolicy = static_cast<long>(0x80092026UL);   // CRYPT_E_SECURITY_SETTINGS

inline std::string DescribeInstallerTrustStatus(long status) {
    if (status == kInstallerTrustNoSignature) {
        return std::string("the installer is not code-signed");
    }
    if (status == kInstallerTrustExpiredCert) {
        return std::string("a certificate in the installer's signing chain has expired");
    }
    if (status == kInstallerTrustUntrustedRoot) {
        return std::string("the installer's signing certificate does not chain to a trusted root");
    }
    if (status == kInstallerTrustExplicitDistrust) {
        return std::string("the installer's signing certificate is explicitly distrusted");
    }
    if (status == kInstallerTrustBadDigest) {
        return std::string("the installer's contents do not match its signature (corrupt or tampered file)");
    }
    if (status == kInstallerTrustSecurityPolicy) {
        return std::string("signature verification was blocked by local security policy");
    }
    return std::string("signature verification failed (status ") + std::to_string(status) + ")";
}

// Decide whether the downloaded installer may be launched given its Authenticode verification
// status. GitHub HTTPS authenticates the transport, not the artifact, so anything other than a
// signature chaining to a trusted root fails closed. allowUnsignedOverride (the
// SMATCHET_UPDATE_ALLOW_UNSIGNED env hatch for local release-pipeline validation with self-signed
// certificates) forgives every failure EXCEPT a bad digest: a signature that is present but does
// not match the file bytes is unambiguous tampering and never launches. On refusal, outError
// carries the user-facing reason.
inline bool ShouldLaunchDownloadedInstaller(long trustStatus, bool allowUnsignedOverride, std::string& outError) {
    outError.clear();
    if (trustStatus == kInstallerTrustOk) {
        return true;
    }
    if (trustStatus != kInstallerTrustBadDigest && allowUnsignedOverride) {
        return true;
    }
    outError = "Update was not applied: " + DescribeInstallerTrustStatus(trustStatus) +
               ". The downloaded installer was discarded.";
    return false;
}

} // namespace app_update
} // namespace smatchet
