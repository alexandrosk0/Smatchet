#ifndef SMATCHET_TESTS_FAKE_ATTACHMENT_APP_UPDATE_DEPS_H
#define SMATCHET_TESTS_FAKE_ATTACHMENT_APP_UPDATE_DEPS_H

// FakeAttachmentAppUpdateDeps — header-only in-memory implementation of IAttachmentAppUpdateDeps for
// the doctest rig. Holds a plain HostCallbacks struct tests populate to script the
// viewer/preview/collection dispatch, and records OpenUrl + RequestAppQuit invocations so tests
// assert the fallback + post-install side effects. SQLite/ImGui/cpr-free by construction: it pulls
// only the narrow interface header + the HostCallbacks struct.
// This fixture is the test-side counterpart of GridContextDepsAdapter's IAttachmentAppUpdateDeps
// surface. Any new method added to IAttachmentAppUpdateDeps MUST be implemented here too — the
// override list mirrors the production adapter.

#include "IAttachmentAppUpdateDeps.h"
#include "Types/HostCallbacks.h"

#include <string>
#include <vector>

namespace smatchet_tests {

class FakeAttachmentAppUpdateDeps : public IAttachmentAppUpdateDeps {
  public:
    /// Host callbacks tests populate (e.g. assign HostImpl.AttachmentCollection to capture the
    /// dispatched descriptors). Read live by the service exactly as the production hostCallbacks_.
    HostCallbacks HostImpl;

    /// Recorded side effects.
    std::vector<std::string> OpenUrlCalls;
    int RequestAppQuitCalls = 0;

    // --- IAttachmentAppUpdateDeps overrides ---------------------------------------------------

    const HostCallbacks& Host() const override { return HostImpl; }

    void OpenUrl(const std::string& url) const override {
        // const interface method, mutable recorder — mirror the production adapter's const forwards.
        const_cast<FakeAttachmentAppUpdateDeps*>(this)->OpenUrlCalls.push_back(url);
    }

    void RequestAppQuit() const override {
        const_cast<FakeAttachmentAppUpdateDeps*>(this)->RequestAppQuitCalls++;
    }
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_ATTACHMENT_APP_UPDATE_DEPS_H
