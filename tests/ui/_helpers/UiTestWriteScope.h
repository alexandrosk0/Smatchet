// UiTestWriteScope.h — shared bucket-E RAII helper that flips ConfigManager's
// fresh-install read-only default OFF for the duration of a write-driving test.
//
// PROBLEM THIS SOLVES
// -------------------
// Bucket-E tests run inside the live host process against a FRESH profile in the
// isolated SMATCHET_USER_DATA dir. When ConfigManager::Load() finds no on-disk
// smatchet_config.json it defaults `ReadOnlyMode=true` (first-launch protection
// for real users). Any bucket-E test that performs a queue/field-edit WRITE
// (QueueFieldEditOffline / QueueCreateOffline / etc.) therefore has its write
// SILENTLY rejected — the assertion path is unreachable and the test goes
// vacuously green. Unlike the doctest-side TestEnvGuard, a bucket-E test cannot
// redirect the user-data dir (it shares the already-booted process config), so
// it must flip the live config: Load -> set ReadOnlyMode=false -> Save ->
// InvalidateCache, then restore the prior value on teardown.
//
// HARD-FAIL, NOT SKIP
// -------------------
// This helper only clears the read-only gate. The write itself MUST still be
// asserted (hard-fail) by the test once its other env gates are met — a silent
// skip there would re-hide the very regression class this guard exists to expose.
// See agents/core/test-rig.md (bucket-E write rule).
//
// USAGE
// -----
//     {
//         BucketE::UiTestWriteScope writeScope; // ReadOnlyMode flipped off here
//         const std::int64_t editId = app->QueueFieldEditOffline(...);
//         IM_CHECK_NO_RET(editId > 0);          // MUST land — do not skip
//         ...
//     }                                          // prior ReadOnlyMode restored
//
// Header-only; the dir is already on the include path for SmatchetStandalone and
// SmatchetCore_DX12 via tests/ui/CMakeLists.txt. C++14-compliant — no deps beyond
// ConfigManager.h.

#ifndef SMATCHET_TESTS_UI_HELPERS_UI_TEST_WRITE_SCOPE_H
#define SMATCHET_TESTS_UI_HELPERS_UI_TEST_WRITE_SCOPE_H

#include "Config/ConfigManager.h"

namespace BucketE {

class UiTestWriteScope {
  public:
    UiTestWriteScope() : prevReadOnly_(ConfigManager::Load().ReadOnlyMode) {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.ReadOnlyMode = false;
        ConfigManager::Save(cfg);
        ConfigManager::InvalidateCache();
    }

    ~UiTestWriteScope() {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.ReadOnlyMode = prevReadOnly_;
        ConfigManager::Save(cfg);
        ConfigManager::InvalidateCache();
    }

    UiTestWriteScope(const UiTestWriteScope&) = delete;
    UiTestWriteScope& operator=(const UiTestWriteScope&) = delete;

    bool PreviousReadOnly() const { return prevReadOnly_; }

  private:
    bool prevReadOnly_;
};

} // namespace BucketE

#endif // SMATCHET_TESTS_UI_HELPERS_UI_TEST_WRITE_SCOPE_H
