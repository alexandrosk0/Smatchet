#include "Commands/Scenarios/IScenario.h"

#include "Commands/Command.h"
#include "Commands/PathConfinement.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#else
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

namespace smatchet {
namespace cmd {

void ScenarioRunner::RegisterFactory(const std::string& name, Factory f) { factories_[name] = std::move(f); }

std::size_t ScenarioRunner::UnregisterFactory(const std::string& name) { return factories_.erase(name); }

CommandResult ScenarioRunner::Start(const std::string& name, const nlohmann::json& args, const CommandContext& ctx) {
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        std::vector<std::string> available;
        for (const auto& kv : factories_)
            available.push_back(kv.first);
        CommandResult r = CommandResult::Failure(ErrorCode::NotFound, "Scenario '" + name + "' not registered.",
                                                 "Available: " + (available.empty() ? "(none)" : available.front()));
        r.Error.Suggestions = std::move(available);
        return r;
    }

    // Dry-run: return what the scenario would do without running it.
    if (ctx.DryRun) {
        return CommandResult::Success({{"wouldDo", {{"scenario", name}, {"args", args}}}});
    }

    // Build output path for results.
    std::string outPath = args.value("outPath", std::string());
    if (outPath.empty()) {
        const std::string& userDataDir = ConfigManager::GetUserDataDirectory();
        std::error_code ec;
        fs::create_directories(fs::path(userDataDir + "perf"), ec);
        char ts[64] = {};
        std::time_t t = std::time(nullptr);
        if (const std::tm* lt = std::localtime(&t)) // null-check: localtime can fail; strftime(nullptr) is UB
            std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", lt);
        outPath = userDataDir + "perf/" + name + "-" + ts + ".json";
    } else {
        // SECURITY: a caller-supplied outPath is untrusted (MCP/CLI/Lua can invoke
        // scenario.run). Confine it under the dedicated <userData>/perf/ subdir (matching
        // the default branch above) — not the user-data root, which holds
        // smatchet_config.json — so it cannot write or clobber an arbitrary file.
        std::string resolved, confineErr;
        if (!ConfinePathUnderSubdir(ConfigManager::GetUserDataDirectory(), "perf", outPath, resolved, confineErr)) {
            return CommandResult::Failure(ErrorCode::ValidationError, "scenario.run outPath rejected: " + confineErr);
        }
        outPath = resolved;
    }
    outPath_ = std::move(outPath);
    frame_ = 0;
    warmupResetDone_ = false;

    // Stale-file footgun fix: --spawn callers' WaitForFile poller treats any
    // non-empty file at outPath_ as "result ready". A stale file from the prior
    // run produces phantom-same numbers on three consecutive runs (perf-detective
    // hit this while debugging). Unlink before scenario starts so the poller
    // can only ever see the result of this run.
    std::remove(outPath_.c_str());

    std::unique_ptr<IScenario> scenario = it->second();

    // DR7: a scenario is already running when this Start arrives. Tear the
    // outgoing one down through the normal Cancel path FIRST. Cancel drives its
    // OnCancel hook, which for the AI streaming scenarios signals their cancel
    // token, joins the owned worker std::thread, and clears the process-wide
    // AiClientFactory test override. This must happen before the replacement's
    // OnStart installs its own override, and before the move-assign to active_
    // below. Move-assigning over a live scenario would instead destroy it in
    // place: running ~std::thread on a still-joinable worker calls
    // std::terminate, and the stale factory override would dangle into the
    // freed scenario's state.
    if (active_) {
        Cancel();
    }

    std::string startErr;
    // Capture the context's scenario host before the call (handler may be invoked from any source).
    IAppScenarioHost* hostPtr = ctx.ScenarioHost;
    if (hostPtr) {
        scenario->OnStart(*hostPtr, args, startErr);
    } else {
        startErr = "AppController not available";
    }
    if (!startErr.empty()) {
        return CommandResult::Failure(ErrorCode::HandlerError, "Scenario '" + name + "' failed to start: " + startErr);
    }

    app_ = hostPtr;
    active_ = std::move(scenario);
    LOG_INFO("ScenarioRunner: started '%s' → %s", name.c_str(), outPath_.c_str());
    return CommandResult::Success({{"running", true}, {"outPath", outPath_}});
}

void ScenarioRunner::Tick(IAppScenarioHost& app, bool& outScrollActive, int& outScrollTarget) {
    outScrollActive = false;
    outScrollTarget = -1;
    if (!active_)
        return;

    // Uniform warmup-frame exclusion (tooling.md `p99-gate-warmup-frame-exclusion`).
    // The perf scopes for frames 0..frame_-1 have already recorded into their
    // sample rings by the time this Tick runs (Scenarios().Tick is the last call
    // in SmatchetUI::Draw, after every measured scope). Once the scenario's
    // declared warmup count has been driven, Reset() once — clearing the rings
    // so the snapshot-time ComputeP99 that feeds the absolute p99 ceiling sees
    // only steady-state samples, not the one-time cold-start spikes (font-atlas
    // build, first-frame layout, initial catalog sync) that otherwise dominate
    // p99 (observed: SmatchetUI::Draw p99 ~12 ms, drawEnsureCatalogAndInitialSync
    // ~9 ms, both pure warmup). WarmupFrames() defaults to 0, so scenarios that
    // do not opt in — including the ten that Reset() themselves in OnStart — are
    // never touched here.
    if (!warmupResetDone_) {
        const int warmup = active_->WarmupFrames();
        if (warmup > 0 && frame_ >= warmup) {
            UiPerfMonitor::Instance().Reset();
            warmupResetDone_ = true;
        }
    }

    active_->OnFrame(app, frame_);

    const int scrollY = active_->CurrentScrollY();
    if (scrollY >= 0) {
        outScrollActive = true;
        outScrollTarget = scrollY;
    }

    ++frame_;

    if (active_->IsDone(frame_)) {
        nlohmann::json result = active_->OnFinish(app);
        outScrollActive = false;
        outScrollTarget = -1;

        // Write result to disk.
        if (!outPath_.empty()) {
            std::FILE* f = std::fopen(outPath_.c_str(), "wb");
            if (f) {
                const std::string s = result.dump(2);
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
                LOG_INFO("ScenarioRunner: wrote result → %s (%zu frames)", outPath_.c_str(), (size_t)frame_);
            } else {
                LOG_WARN("ScenarioRunner: could not write result to %s", outPath_.c_str());
            }
        }
        active_.reset();
        app_ = nullptr;
        frame_ = 0;
        warmupResetDone_ = false;
    }
}

void ScenarioRunner::Cancel() {
    if (active_) {
        // Drive OnCancel so scenarios can unwind transient state (e.g. unregister scenario-
        // installed Lua providers). OnFinish is NOT called on cancel — cleanup needs its own
        // hook. The app_ pointer was stashed in Start.
        if (app_) {
            try {
                active_->OnCancel(*app_);
            } catch (const std::exception& ex) {
                LOG_WARN("ScenarioRunner::Cancel: OnCancel threw: %s", ex.what());
            } catch (...) {
                LOG_WARN("ScenarioRunner::Cancel: OnCancel threw unknown exception");
            }
        }
        LOG_INFO("ScenarioRunner: cancelled '%s' at frame %d", active_->Name().c_str(), frame_);
        active_.reset();
        app_ = nullptr;
        frame_ = 0;
        warmupResetDone_ = false;
    }
}

std::vector<std::string> ScenarioRunner::ListNames() const {
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& kv : factories_)
        out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace cmd
} // namespace smatchet
