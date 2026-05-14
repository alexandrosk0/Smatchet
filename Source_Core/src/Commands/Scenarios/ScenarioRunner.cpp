#include "Commands/Scenarios/IScenario.h"

#include "Commands/Command.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#else
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

namespace smatchet {
namespace cmd {

void ScenarioRunner::RegisterFactory(const std::string& name, Factory f) {
    factories_[name] = std::move(f);
}

CommandResult ScenarioRunner::Start(const std::string& name, const nlohmann::json& args,
                                    const CommandContext& ctx) {
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        std::vector<std::string> available;
        for (const auto& kv : factories_) available.push_back(kv.first);
        CommandResult r = CommandResult::Failure(
            ErrorCode::NotFound,
            "Scenario '" + name + "' not registered.",
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
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
        outPath = userDataDir + "perf/" + name + "-" + ts + ".json";
    }
    outPath_ = std::move(outPath);
    frame_ = 0;

    std::unique_ptr<IScenario> scenario = it->second();
    std::string startErr;
    // Capture ctx.App before the call (handler may be invoked from any source).
    AppController* appPtr = ctx.App;
    if (appPtr) {
        scenario->OnStart(*appPtr, args, startErr);
    } else {
        startErr = "AppController not available";
    }
    if (!startErr.empty()) {
        return CommandResult::Failure(ErrorCode::HandlerError,
            "Scenario '" + name + "' failed to start: " + startErr);
    }

    app_    = appPtr;
    active_ = std::move(scenario);
    LOG_INFO("ScenarioRunner: started '%s' → %s", name.c_str(), outPath_.c_str());
    return CommandResult::Success({{"running", true}, {"outPath", outPath_}});
}

void ScenarioRunner::Tick(AppController& app, bool& outScrollActive, int& outScrollTarget) {
    outScrollActive = false;
    outScrollTarget = -1;
    if (!active_) return;

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
        app_   = nullptr;
        frame_ = 0;
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
        app_   = nullptr;
        frame_ = 0;
    }
}

std::vector<std::string> ScenarioRunner::ListNames() const {
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& kv : factories_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace cmd
}  // namespace smatchet
