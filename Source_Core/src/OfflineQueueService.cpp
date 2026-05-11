#include "OfflineQueueService.h"

#include "AppController.h"
#include "LocalCacheManager.h"
#include "Logger.h"

#include <utility>

OfflineQueueService::OfflineQueueService(AppController& app) : app_(app) {}

std::size_t OfflineQueueService::GetPendingCreateCount() const {
    if (!app_.Cache) {
        return 0;
    }
    try {
        return app_.Cache->LoadPendingCreates().size();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::size_t OfflineQueueService::GetDeadPendingCreateCount() const {
    if (!app_.Cache) {
        return 0;
    }
    try {
        return app_.Cache->GetDeadPendingCreateCount();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: %s", ex.what());
        return 0;
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreateCount failed: unknown exception");
        return 0;
    }
}

std::vector<PendingCreate> OfflineQueueService::GetPendingCreates() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetPendingCreates failed: unknown exception");
        return {};
    }
}

std::vector<DeadPendingCreate> OfflineQueueService::GetDeadPendingCreates() const {
    if (!app_.Cache) {
        return {};
    }
    try {
        return app_.Cache->LoadDeadPendingCreates();
    } catch (const std::exception& ex) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: %s", ex.what());
        return {};
    } catch (...) {
        LOG_ERROR("OfflineQueueService::GetDeadPendingCreates failed: unknown exception");
        return {};
    }
}

std::string OfflineQueueService::TakeLegacyPendingStartupBanner() {
    return std::move(legacyPendingStartupBanner_);
}
