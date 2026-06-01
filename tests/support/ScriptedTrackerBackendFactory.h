#ifndef SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H
#define SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H

// Slice 2 of docs/plans/active/deterministic-jira-test-backend.md — factory wrapper
// that returns fresh fixture-configured FakeTrackerClient("Jira") instances on Create().
// Injected via AppController::SetBackendFactory before Initialize in UI-test builds.
//
// The factory owns the JiraFakeTrackerFixture by value so there is no lifetime hazard
// when AppController holds the factory via unique_ptr. Non-Jira tracker types get a
// plain FakeTrackerClient(trackerType).

#include "FakeTrackerClient.h"
#include "ITrackerBackendFactory.h"
#include "JiraFakeTrackerFixture.h"

#include <memory>
#include <string>
#include <utility>

namespace smatchet_tests {

class ScriptedTrackerBackendFactory : public ITrackerBackendFactory {
  public:
    explicit ScriptedTrackerBackendFactory(JiraFakeTrackerFixture fixture) : fixture_(std::move(fixture)) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType) override {
        if (trackerType == "Jira" || trackerType == "jira") {
            return fixture_.CreateClient();
        }
        return std::unique_ptr<ITrackerBackend>(new FakeTrackerClient(trackerType));
    }

  private:
    JiraFakeTrackerFixture fixture_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_SCRIPTED_TRACKER_BACKEND_FACTORY_H
