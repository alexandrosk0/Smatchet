#ifndef SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H
#define SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H

// Slice 2 of docs/plans/active/deterministic-jira-test-backend.md — factory wrapper
// that returns fresh fixture-configured FakeTrackerClient("Jira") instances on Create().
// Injected via AppController::SetBackendFactory before Initialize in UI-test builds.
//
// Non-Jira tracker types get a plain FakeTrackerClient(trackerType) so the app can
// still initialise without crashing if a different backend is configured.

#include "FakeTrackerClient.h"
#include "ITrackerBackendFactory.h"
#include "JiraFakeTrackerFixture.h"

#include <memory>
#include <string>

namespace smatchet_tests {

class ScriptedTrackerBackendFactory : public ITrackerBackendFactory {
  public:
    explicit ScriptedTrackerBackendFactory(const JiraFakeTrackerFixture* fixture) : fixture_(fixture) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType) override {
        if (fixture_ && (trackerType == "Jira" || trackerType == "jira")) {
            return fixture_->CreateClient();
        }
        return std::unique_ptr<ITrackerBackend>(new FakeTrackerClient(trackerType));
    }

  private:
    const JiraFakeTrackerFixture* fixture_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H
