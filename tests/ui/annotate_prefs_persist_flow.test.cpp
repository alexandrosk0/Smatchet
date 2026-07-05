// annotate_prefs_persist_flow.test.cpp — bucket-E coverage for the Annotate
// Preferences tab's edit → persist → reload round-trip (follow-up "item 5"
// from the Annotate prefs cleanup; the last manual-residue verification after
// the N14 async-save cutover).
//
// What the data layer ALREADY covers (tests/Core/AnnotateAnalysisConfig.test.cpp,
// doctest): clamp, show_raw_callstack, multi-rule path_remaps Save/Load
// round-trip; plus a [high-risk] concurrent-write valid-JSON gate. What is NOT
// covered there is the UI-interaction → persistence → reload path, and in
// particular the N14 timing change: a prefs edit routes the save
// through a DETACHED worker thread (value-snapshot → ConfigManager::
// SaveAnnotateAnalysis), so persistence is ASYNCHRONOUS — the test must
// bounded-wait for the write to land, never assume a synchronous save.
//
// Drift warning — IF YOU CHANGE any of these host surfaces, UPDATE THE REPLICA /
// HOST-COUPLED CHECKS below to match:
//   - Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:16-22
//       (PersistAnnotateCfg → ScheduleAnnotateConfigSaveDetached), and the
//       cl-cache commit site :181-190 (ChangelistCacheMaxEntries clamp).
//   - Source/Core/src/Ui/AnnotateAnalysisUi_Config.cpp:24-34
//       (ScheduleAnnotateConfigSaveDetached: value-snapshot + detached
//       std::thread → ConfigManager::SaveAnnotateAnalysis, catch-all swallow).
//   - Source/Core/src/Config/ConfigManager.cpp:463-532 (LoadAnnotateAnalysis,
//       incl. the [16,8192] cl_cache_max clamp) and :534+ (SaveAnnotateAnalysis).
//
// Why a TU-local replica of the detach (not a direct call into
// AnnotateInternal::ScheduleAnnotateConfigSaveDetached): that symbol lives
// behind AnnotateAnalysisUi_Internal.h, which `#define ImGui
// SmatchetLocalizedImGui` and pulls the full private AnnotateState type — not
// includable from a generic test TU without ODR / macro hazards. The replica
// here is a byte-faithful mirror of AnnotateAnalysisUi_Config.cpp:24-34: a
// value snapshot handed to a detached std::thread that calls the SAME static
// persistence seam (ConfigManager::SaveAnnotateAnalysis) the production path
// uses. The deterministic core therefore exercises the real Save/Load code +
// the real async timing; only the ~10-line thread-launch shim is mirrored, and
// the drift-warning above keeps it in lock-step.
//
// Host-coupling note (why NOT a temp-dir redirect): an earlier draft repointed
// ConfigManager::SetUserDataDirectory at a private temp dir for the test
// window. In the LIVE host process that races the UI thread's own config
// reads (the host briefly sees a config file missing all its keys) and made
// the engine frame loop flake (~1-in-4). The process-wide ConfigManager
// singleton must not be repointed mid-run. Instead we snapshot the host's
// real config-file BYTES, run the real Save/Load round-trip against the actual
// config path (single file — the host only ever sees a valid config with the
// `annotate_analysis.cl_cache_max` value transiently changed, which is benign
// and self-healing), then restore the original bytes verbatim. No dir swap, no
// missing-keys window.
//
// Two variants (both deterministic, same persistence seam):
//   1. EditPersistsAndReloads (PRIMARY) — drive a replica Annotate cl-cache
//      InputText through the engine to an in-range value, fire the N14 detached
//      save on commit, bounded-wait for the file write, then InvalidateCache +
//      LoadAnnotateAnalysis and assert the reloaded value.
//   2. EditClampsThenPersists (SECONDARY) — same path with an over-max edit;
//      asserts the UI-layer clamp to 8192 round-trips (load also re-clamps, so
//      the value is stable).
// Both skip-with-log if the host config path can't be resolved/snapshotted
// (host-coupling guard).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "ConfigManager.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

// --- Replica pref-form state ---------------------------------------------
//
// Mirrors the slice of AnnotateAnalysisUi AnnotateState relevant to the
// cl-cache InputInt commit path (AnnotateAnalysisUi_Preferences.cpp:181-190):
// an edit buffer + a save-fire counter. The persisted value lives in the real
// config file via the production Save/Load seam.

struct PersistState {
    char clCacheBuf[16] = {0}; // text edit buffer for the InputInt-equivalent field
    int saveCalls = 0;         // number of detached saves fired this run
    // Host config-file byte snapshot, captured at test start and restored at
    // end so the live host's real config is left byte-identical.
    std::string configPath;
    std::string savedBytes;
    bool savedHadFile = false;
    bool snapshotOk = false; // false → host-coupling skip
};

PersistState g_persistState;

bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteWholeFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    if (!bytes.empty()) {
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(f);
}

// Snapshot the host config file bytes so the round-trip can be undone exactly.
bool BeginConfigSnapshot(PersistState& s) {
    s.configPath = ConfigManager::GetConfigPath();
    if (s.configPath.empty()) {
        return false;
    }
    s.savedBytes.clear();
    s.savedHadFile = ReadWholeFile(s.configPath, s.savedBytes);
    return true;
}

// Restore the host config file to its exact pre-test bytes (or remove it if it
// did not exist before) and drop the cache so the host re-reads the original.
void EndConfigSnapshot(PersistState& s) {
    if (s.savedHadFile) {
        WriteWholeFile(s.configPath, s.savedBytes);
    } else {
        std::remove(s.configPath.c_str());
    }
    ConfigManager::InvalidateCache();
}

// Byte-faithful mirror of AnnotateAnalysisUi_Config.cpp:24-34
// ScheduleAnnotateConfigSaveDetached — value-snapshot handed to a detached
// std::thread calling the SAME static persistence seam. Returns immediately;
// the caller bounded-waits on the file write.
void FireDetachedSaveReplica(const AnnotateAnalysisConfig& cfg) {
    AnnotateAnalysisConfig snapshot = cfg;
    std::thread([snapshot]() {
        try {
            ConfigManager::SaveAnnotateAnalysis(snapshot);
        } catch (...) { // catch-all-ok: mirrors the detached-worker swallow so a
                        // worker exit can't terminate the process.
        }
    }).detach();
}

// Bounded poll: re-hydrate from disk until the persisted cl-cache value equals
// `expected`, or the deadline elapses. Returns true if it landed. Mirrors the
// N14 reality that the save is asynchronous — never assumes it's done on
// return from the commit.
bool WaitForPersistedClCache(ImGuiTestContext* ctx, int expected, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ConfigManager::InvalidateCache();
        if (ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries == expected) {
            return true;
        }
        ctx->Yield(); // keep the engine frame loop alive while the worker writes
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Replica of the Annotate cl-cache InputInt commit (Preferences.cpp:181-190):
// edit-commit clamps into [16,8192], assigns the cfg field, and fires the
// detached save. Driven via a text buffer so the engine's KeyChars works
// uniformly; parses to int on commit like InputInt does. Reads the current
// config first (LoadAnnotateAnalysis) so only the cl-cache key changes.
void DrawPersistReplica(PersistState& s) {
    ImGui::SetNextWindowSize(ImVec2(360, 100), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::AnnotatePrefsPersist", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::InputText("Changelist cache size", s.clCacheBuf, sizeof(s.clCacheBuf), ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            int v = std::atoi(s.clCacheBuf);
            v = (std::max)(16, (std::min)(8192, v)); // mirrors the production clamp
            AnnotateAnalysisConfig cfg = ConfigManager::LoadAnnotateAnalysis();
            cfg.ChangelistCacheMaxEntries = v;
            FireDetachedSaveReplica(cfg);
            ++s.saveCalls;
        }
    }
    ImGui::End();
}

// Shared body for both variants: drive a commit to `typed`, then bounded-wait
// for the persisted value to equal `expectPersisted`.
void RunEditPersistReload(ImGuiTestContext* ctx, const char* typed, int expectPersisted) {
    auto* s = static_cast<PersistState*>(ctx->Test->UserData);
    std::memset(s->clCacheBuf, 0, sizeof(s->clCacheBuf));
    s->saveCalls = 0;

    s->snapshotOk = BeginConfigSnapshot(*s);
    if (!s->snapshotOk) {
        ctx->LogInfo("skip: could not resolve/snapshot host config path (host-coupling)");
        return;
    }

    // Seed an on-disk pre-value DISTINCT from expectPersisted so the wait below can only succeed if
    // THIS edit's write actually lands — guards against a false positive where the value already
    // happened to be on disk from a prior run (CR: false-positive persistence check).
    {
        const int preValue = (expectPersisted != 64) ? 64 : 128; // in-range [16,8192], != expectPersisted
        AnnotateAnalysisConfig pre = ConfigManager::LoadAnnotateAnalysis();
        pre.ChangelistCacheMaxEntries = preValue;
        ConfigManager::SaveAnnotateAnalysis(pre);
        ConfigManager::InvalidateCache();
        IM_CHECK_EQ(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries, preValue);
    }

    ctx->SetRef("SmatchetTest::AnnotatePrefsPersist");
    ctx->Yield();
    ctx->Yield();

    ctx->ItemClick("Changelist cache size");
    ctx->KeyChars(typed);
    ctx->KeyPress(ImGuiKey_Enter); // commit → IsItemDeactivatedAfterEdit fires the detached save
    ctx->Yield();
    IM_CHECK_EQ(s->saveCalls, 1);

    // N14: the save is on a detached worker — bounded-wait for it, never assume
    // it's already on disk.
    const bool landed = WaitForPersistedClCache(ctx, expectPersisted, /*timeoutMs=*/3000);
    IM_CHECK(landed);
    IM_CHECK_EQ(ConfigManager::LoadAnnotateAnalysis().ChangelistCacheMaxEntries, expectPersisted);

    EndConfigSnapshot(*s);
}

void RegisterEditPersistsAndReloadsVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AnnotatePrefs", "EditPersistsAndReloads");
    t->UserData = &g_persistState;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawPersistReplica(*static_cast<PersistState*>(ctx->Test->UserData)); };
    t->TestFunc = [](ImGuiTestContext* ctx) { RunEditPersistReload(ctx, "777", 777); };
}

void RegisterRealFormClampVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AnnotatePrefs", "EditClampsThenPersists");
    t->UserData = &g_persistState;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawPersistReplica(*static_cast<PersistState*>(ctx->Test->UserData)); };
    t->TestFunc = [](ImGuiTestContext* ctx) { RunEditPersistReload(ctx, "100000", 8192); };
}

} // namespace

extern "C" void SmatchetRegisterAnnotatePrefsPersistFlowTests(ImGuiTestEngine* engine) {
    RegisterEditPersistsAndReloadsVariant(engine);
    RegisterRealFormClampVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
