// UpdateInstallerSignaturePolicy — the auto-updater must not launch a downloaded installer whose
// Authenticode verification failed (GitHub HTTPS authenticates the transport, not the artifact).
// ShouldLaunchDownloadedInstaller() maps the raw WinVerifyTrust status to the launch decision and
// the user-facing refusal reason; the SMATCHET_UPDATE_ALLOW_UNSIGNED override forgives everything
// except a bad digest (a present-but-mismatched signature is unambiguous tampering). Pure logic,
// so this exercises the header-inline policy directly with no Win32 / wintrust surface.

#include "AttachmentAppUpdateService.h"

#include <doctest/doctest.h>

#include <string>

using namespace smatchet::app_update;

TEST_CASE("ShouldLaunchDownloadedInstaller launches a trusted installer with no error") {
    std::string error = "stale";
    CHECK(ShouldLaunchDownloadedInstaller(kInstallerTrustOk, false, error));
    CHECK(error.empty());
}

TEST_CASE("ShouldLaunchDownloadedInstaller refuses an unsigned installer") {
    std::string error;
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustNoSignature, false, error));
    CHECK(error.find("not code-signed") != std::string::npos);
    CHECK(error.find("not applied") != std::string::npos);
}

TEST_CASE("ShouldLaunchDownloadedInstaller refuses an untrusted-root signature") {
    std::string error;
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustUntrustedRoot, false, error));
    CHECK(error.find("trusted root") != std::string::npos);
}

TEST_CASE("ShouldLaunchDownloadedInstaller refuses expired / distrusted / policy-blocked chains") {
    std::string error;
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustExpiredCert, false, error));
    CHECK(error.find("expired") != std::string::npos);
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustExplicitDistrust, false, error));
    CHECK(error.find("distrusted") != std::string::npos);
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustSecurityPolicy, false, error));
    CHECK(error.find("security policy") != std::string::npos);
}

TEST_CASE("ShouldLaunchDownloadedInstaller refuses an unknown status and names it") {
    std::string error;
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(12345L, false, error));
    CHECK(error.find("12345") != std::string::npos);
}

TEST_CASE("Unsigned override forgives a missing signature and an untrusted root") {
    std::string error;
    CHECK(ShouldLaunchDownloadedInstaller(kInstallerTrustNoSignature, true, error));
    CHECK(error.empty());
    CHECK(ShouldLaunchDownloadedInstaller(kInstallerTrustUntrustedRoot, true, error));
    CHECK(error.empty());
}

TEST_CASE("Unsigned override never forgives a bad digest (tampered file)") {
    std::string error;
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustBadDigest, true, error));
    CHECK(error.find("tampered") != std::string::npos);
    // And without the override it is refused all the same.
    CHECK_FALSE(ShouldLaunchDownloadedInstaller(kInstallerTrustBadDigest, false, error));
}

TEST_CASE("DescribeInstallerTrustStatus maps every mirrored wintrust status to a distinct reason") {
    const std::string noSig = DescribeInstallerTrustStatus(kInstallerTrustNoSignature);
    const std::string expired = DescribeInstallerTrustStatus(kInstallerTrustExpiredCert);
    const std::string untrusted = DescribeInstallerTrustStatus(kInstallerTrustUntrustedRoot);
    const std::string distrust = DescribeInstallerTrustStatus(kInstallerTrustExplicitDistrust);
    const std::string digest = DescribeInstallerTrustStatus(kInstallerTrustBadDigest);
    const std::string policy = DescribeInstallerTrustStatus(kInstallerTrustSecurityPolicy);
    CHECK(noSig != expired);
    CHECK(expired != untrusted);
    CHECK(untrusted != distrust);
    CHECK(distrust != digest);
    CHECK(digest != policy);
    CHECK(policy != noSig);
}
