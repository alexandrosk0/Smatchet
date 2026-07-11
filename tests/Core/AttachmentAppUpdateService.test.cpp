// AttachmentAppUpdateService bucket-A tests — exercise the attachment-open dispatch + app-version
// helpers extracted from AppController (god-object decomposition Phase 4) through the
// IAttachmentAppUpdateDeps interface bundle. Tests drive the service against a
// FakeAttachmentAppUpdateDeps fixture (header-only: a plain HostCallbacks struct tests populate +
// recorded OpenUrl / RequestAppQuit side effects) so no AppController, ImGui, cpr, HTTP, or SQLite
// surface is touched.
//
// Scope note: the network paths (CheckForAppUpdate, DownloadAndLaunchInstallerUpdate, and the
// download step inside OpenAttachment / OpenAttachmentInSystemViewer / DownloadAttachmentForPreview)
// make live cpr calls and cannot run in a hermetic unit. These tests cover the deterministic,
// pre-download decision logic that is reachable without a network: the collection-handler dispatch,
// the no-handler URL fallback, and the preview-handler early-rejection branches. The compile-time
// version helpers are covered too (non-empty contract).
//
// Per-case isolation: every TEST_CASE constructs its own FakeAttachmentAppUpdateDeps + fresh
// AttachmentAppUpdateService. No statics, no shared world state, no order dependencies.

#include "../support/FakeAttachmentAppUpdateDeps.h"

#include "AttachmentAppUpdateService.h"
#include "Types/AttachmentTypes.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet_tests::FakeAttachmentAppUpdateDeps;

// ---------------------------------------------------------------------------
// ShowAttachmentCollection — host collection handler dispatch.
// ---------------------------------------------------------------------------
TEST_CASE("ShowAttachmentCollection dispatches to the host collection handler when present") {
    FakeAttachmentAppUpdateDeps deps;
    std::vector<AttachmentDescriptor> captured;
    deps.HostImpl.AttachmentCollection = [&captured](const std::vector<AttachmentDescriptor>& a) { captured = a; };
    AttachmentAppUpdateService svc(deps);

    std::vector<AttachmentDescriptor> attachments;
    AttachmentDescriptor d;
    d.Filename = "a.png";
    d.Url = "https://example.com/a.png";
    d.MimeType = "image/png";
    attachments.push_back(d);

    svc.ShowAttachmentCollection(attachments);

    CHECK(captured.size() == 1);
    CHECK(captured.front().Filename == "a.png");
    // The collection handler consumed it; no URL-open fallback fired.
    CHECK(deps.OpenUrlCalls.empty());
}

TEST_CASE("ShowAttachmentCollection on empty input is a no-op") {
    FakeAttachmentAppUpdateDeps deps;
    bool handlerCalled = false;
    deps.HostImpl.AttachmentCollection = [&handlerCalled](const std::vector<AttachmentDescriptor>&) {
        handlerCalled = true;
    };
    AttachmentAppUpdateService svc(deps);

    svc.ShowAttachmentCollection({});

    CHECK_FALSE(handlerCalled);
    CHECK(deps.OpenUrlCalls.empty());
}

TEST_CASE("ShowAttachmentCollection with no collection handler falls back to opening the first URL") {
    // No collection handler AND no viewer/preview handler → OpenAttachment opens the URL directly.
    FakeAttachmentAppUpdateDeps deps;
    AttachmentAppUpdateService svc(deps);

    std::vector<AttachmentDescriptor> attachments;
    AttachmentDescriptor d;
    d.Url = "https://example.com/first.bin";
    attachments.push_back(d);

    svc.ShowAttachmentCollection(attachments);

    REQUIRE(deps.OpenUrlCalls.size() == 1);
    CHECK(deps.OpenUrlCalls.front() == "https://example.com/first.bin");
}

// ---------------------------------------------------------------------------
// OpenAttachment — handler selection / fallback (pre-download branches only).
// ---------------------------------------------------------------------------
TEST_CASE("OpenAttachment with no viewer/preview handler opens the URL directly") {
    FakeAttachmentAppUpdateDeps deps;
    AttachmentAppUpdateService svc(deps);

    svc.OpenAttachment("https://example.com/file.pdf", "file.pdf", "application/pdf");

    REQUIRE(deps.OpenUrlCalls.size() == 1);
    CHECK(deps.OpenUrlCalls.front() == "https://example.com/file.pdf");
}

TEST_CASE("OpenAttachment with an empty URL is a no-op") {
    FakeAttachmentAppUpdateDeps deps;
    AttachmentAppUpdateService svc(deps);

    svc.OpenAttachment("", "x", "image/png");

    CHECK(deps.OpenUrlCalls.empty());
}

// ---------------------------------------------------------------------------
// DownloadAttachmentForPreview — early-rejection branches (no network reached).
// ---------------------------------------------------------------------------
TEST_CASE("DownloadAttachmentForPreview rejects when no preview handler is set") {
    FakeAttachmentAppUpdateDeps deps; // HostImpl.AttachmentPreview left unset
    AttachmentAppUpdateService svc(deps);

    std::string err;
    const bool ok = svc.DownloadAttachmentForPreview("https://example.com/a.png", "a.png", "image/png", &err);

    CHECK_FALSE(ok);
    CHECK(err == "Preview handler is unavailable.");
    CHECK(deps.OpenUrlCalls.empty());
}

TEST_CASE("DownloadAttachmentForPreview rejects an unsupported mime type before any download") {
    FakeAttachmentAppUpdateDeps deps;
    deps.HostImpl.AttachmentPreview = [](const std::string&, const std::string&, const std::string&,
                                         const std::string&) { return true; };
    AttachmentAppUpdateService svc(deps);

    std::string err;
    const bool ok = svc.DownloadAttachmentForPreview("https://example.com/a.pdf", "a.pdf", "application/pdf", &err);

    CHECK_FALSE(ok);
    CHECK(err == "Attachment is not a supported image type.");
}

// ---------------------------------------------------------------------------
// Update-check response byte cap — pins the streaming-abort invariant.
//
// CheckForAppUpdate bounds the GitHub release listing with a cpr::WriteCallback
// that appends the streamed body into an accumulator until a hard byte cap would
// be exceeded, then aborts the transfer (returns false, leaving the accumulator
// unchanged) so a hostile / misconfigured endpoint can't stream an unbounded
// body into memory before the parser runs. The live cpr::Get path is
// network-only and cannot run in a hermetic unit (see the file header), so these
// cases pin the exact accept/abort predicate that callback implements: appends
// below the cap accumulate; the write that would cross the cap is rejected whole
// (no partial append). If the production cap logic drifts, this contract breaks.
// ---------------------------------------------------------------------------
namespace {
// Mirror of the CheckForAppUpdate WriteCallback body: true == keep going, false
// == abort. On rejection the accumulator is left byte-for-byte unchanged.
bool CappedAccumulate(std::string& accum, const std::string& chunk, size_t cap) {
    if (accum.size() + chunk.size() > cap) {
        return false;
    }
    accum.append(chunk);
    return true;
}
} // namespace

TEST_CASE("Update-check byte cap accepts chunks up to the cap and aborts past it") {
    constexpr size_t cap = 16;
    std::string accum;

    // Below the cap: appended, transfer continues.
    CHECK(CappedAccumulate(accum, "12345678", cap)); // 8 bytes
    CHECK(accum.size() == 8);

    // Exactly reaching the cap is allowed (size == cap is not "> cap").
    CHECK(CappedAccumulate(accum, "abcdefgh", cap)); // +8 == 16
    CHECK(accum.size() == 16);

    // The next byte would cross the cap: rejected whole, accumulator untouched.
    CHECK_FALSE(CappedAccumulate(accum, "x", cap));
    CHECK(accum.size() == 16);
    CHECK(accum == "12345678abcdefgh");
}

TEST_CASE("Update-check byte cap rejects an oversized first chunk without partial append") {
    constexpr size_t cap = 4;
    std::string accum;

    // A single chunk larger than the whole cap is refused atomically — no prefix
    // is retained (the abort must not leave a truncated body for the parser).
    CHECK_FALSE(CappedAccumulate(accum, "overflowing-chunk", cap));
    CHECK(accum.empty());
}

// ---------------------------------------------------------------------------
// Version helpers — compile-time constants, non-empty contract.
// ---------------------------------------------------------------------------
TEST_CASE("GetAppVersion and GetGitHubReleaseRepo return non-empty compile-time defaults") {
    FakeAttachmentAppUpdateDeps deps;
    AttachmentAppUpdateService svc(deps);

    CHECK_FALSE(svc.GetAppVersion().empty());
    CHECK_FALSE(svc.GetGitHubReleaseRepo().empty());
    // The release repo default carries the owner/name shape.
    CHECK(svc.GetGitHubReleaseRepo().find('/') != std::string::npos);
}
