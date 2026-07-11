// UpdateInstallerFilenameSanitize — DR10(a) regression: the auto-update installer path must be
// pinned to %TEMP%. SanitizeInstallerFilename() reduces an attacker-influenced GitHub release-asset
// name to a bare, separator-free basename before it is concatenated onto the temp dir and
// ShellExecute-launched. Pure string logic, so this exercises the header-inline helper directly with
// no cpr / HTTP / Win32 surface.

#include "AttachmentAppUpdateService.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::app_update::SanitizeInstallerFilename;

TEST_CASE("SanitizeInstallerFilename keeps a plain installer name unchanged") {
    CHECK(SanitizeInstallerFilename("Smatchet-1.2.3-windows-setup.exe") ==
          "Smatchet-1.2.3-windows-setup.exe");
}

TEST_CASE("SanitizeInstallerFilename strips a Windows path-traversal prefix to the basename") {
    // The malicious name still passes the "-windows-setup.exe" substring filter upstream, but must
    // not be allowed to escape %TEMP% into e.g. the Startup folder.
    const std::string sanitized =
        SanitizeInstallerFilename("..\\..\\..\\Users\\victim\\AppData\\Roaming\\Microsoft\\Windows"
                                  "\\Start Menu\\Programs\\Startup\\x-windows-setup.exe");
    CHECK(sanitized == "x-windows-setup.exe");
    CHECK(sanitized.find('\\') == std::string::npos);
    CHECK(sanitized.find('/') == std::string::npos);
    CHECK(sanitized.find("..") == std::string::npos);
}

TEST_CASE("SanitizeInstallerFilename strips a POSIX path-traversal prefix to the basename") {
    CHECK(SanitizeInstallerFilename("../../etc/evil-windows-setup.exe") == "evil-windows-setup.exe");
}

TEST_CASE("SanitizeInstallerFilename neutralizes reserved metacharacters and control bytes") {
    const std::string sanitized = SanitizeInstallerFilename(std::string("a:b*c?\x01-windows-setup.exe"));
    CHECK(sanitized == "a_b_c__-windows-setup.exe");
    CHECK(sanitized.find(':') == std::string::npos);
    CHECK(sanitized.find('*') == std::string::npos);
}

TEST_CASE("SanitizeInstallerFilename substitutes a fallback for empty / pure-traversal names") {
    CHECK(SanitizeInstallerFilename("") == "SmatchetUpdateSetup.exe");
    CHECK(SanitizeInstallerFilename("..") == "SmatchetUpdateSetup.exe");
    CHECK(SanitizeInstallerFilename(".") == "SmatchetUpdateSetup.exe");
    // A trailing-separator name reduces to an empty basename and must also take the fallback.
    CHECK(SanitizeInstallerFilename("evil\\") == "SmatchetUpdateSetup.exe");
}
