// AppController_McpActivity.cpp — MCP client-activity plumbing extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/active/appcontroller-clusters-followup.md). Method DECLARATIONS stay in
// AppController.h; only the definitions moved, so linkage and behavior are identical.
// The whole cluster is guarded by SMATCHET_WITH_MCP (empty TU when MCP is off). Includes
// mirror the AppController_Init.cpp companion-TU idiom (full subsystem superset — the
// pImpl inline ctor forces complete types for every owned-service unique_ptr member).
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining AppController MCP client-activity methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on
#include "AppControllerImpl.h"
#include "GridContextDepsAdapter.h"
#include "LocalCacheManager.h" // direct: AppController.h fwd-decls LocalCacheManager (fan-in Phase 1); this TU calls Cache-> methods.

#include <nlohmann/json.hpp> // direct: AppController.h dropped json.hpp for json_fwd (fan-in Phase 1); this TU constructs nlohmann::json.

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=a companion TU of the AppController god-class necessarily shares its subsystem include set (ConfigManager / backends / owned services); no shared header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when AppController.h fan-in is narrowed per ADR-0020 / debt.md)
// clang-format on
#include "ConfigManager.h"
#include "ConfigSaveWorker.h" // not AI-gated — config saves happen regardless of feature flags

#include "Commands/BuiltinCommands.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include "FieldCatalogCache.h"
#include "JqlProjectScope.h"

#include "DefaultTrackerBackendFactory.h"
#include "GitHubFixtureBackend.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerIssueMutations.h"
#include "LinearFixtureBackend.h"
#include "PlaneFixtureBackend.h"

#include "LuaAutomationHost.h"
#include "OfflineQueueService.h"
#include "EditMetaCacheService.h"
#include "FieldEditPipelineService.h"
#include "ConnectivityMonitorService.h"
#include "AttachmentAppUpdateService.h"
#include "TicketSyncService.h"

#include "Logger.h"
#include "StringUtil.h"
#include "UiThreadAffinity.h"
#include "Views.h"

#include "SmatchetUI.h"
#include "SmatchetToast.h"
#include "SmatchetMergeWatchNotifyServer.h"
#include "Ui/SmatchetFieldRender.h" // RunLegacyStartupSweeps calls the free fn SetCallstackFieldIdHint declared here

#include <ghc/filesystem.hpp>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

#include "AiTypes.h"
#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiAssistantUiStateAdapter.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetUiSession.h"
#endif

#if defined(SMATCHET_WITH_MCP)

namespace {

std::string PrefixMcpActivityLine(const std::string& msg) {

    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tmBuf{};

#if defined(_WIN32)

    if (localtime_s(&tmBuf, &t) != 0) {

        return msg;
    }

#else

    if (localtime_r(&t, &tmBuf) == nullptr) {

        return msg;
    }

#endif

    char timeBuf[32];

    if (std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tmBuf) == 0) {

        return msg;
    }

    return std::string(timeBuf) + " " + msg;
}

} // namespace

void AppController::AppendMcpActivity(const std::string& line) {

    std::lock_guard<std::mutex> lock(impl_->mcpActivityMutex_);

    impl_->mcpActivityLog_.push_back(PrefixMcpActivityLine(line));

    while (impl_->mcpActivityLog_.size() > impl_->kMcpActivityLogMax) {

        impl_->mcpActivityLog_.pop_front();
    }
}

std::vector<std::string> AppController::CopyMcpActivityLog() const {

    std::lock_guard<std::mutex> lock(impl_->mcpActivityMutex_);

    return std::vector<std::string>(impl_->mcpActivityLog_.begin(), impl_->mcpActivityLog_.end());
}

void AppController::NotifyMcpClientHttpActivity() {

    mcpHttpTrafficEpoch_.fetch_add(1, std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

    mcpLastClientHttpActivityNs_.store(static_cast<std::uint64_t>(ns), std::memory_order_release);
}

std::uint64_t AppController::GetMcpHttpTrafficEpoch() const {

    return mcpHttpTrafficEpoch_.load(std::memory_order_acquire);
}

bool AppController::TryGetMcpLastClientHttpActivity(std::chrono::steady_clock::time_point* out) const {

    const std::uint64_t raw = mcpLastClientHttpActivityNs_.load(std::memory_order_acquire);

    if (raw == 0 || out == nullptr) {

        return false;
    }

    *out = std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(raw)));

    return true;
}

#endif
