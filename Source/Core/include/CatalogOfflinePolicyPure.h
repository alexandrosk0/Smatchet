#pragma once

// CatalogOfflinePolicyPure — Quality Pillar 6 (offline-first) decisions for a failed field-catalog
// refresh. Pure (no I/O, no Logger.h) so the doctest rig links it bare.
//
// Invariant: a failed refresh never discards a catalog the user already has, whether in memory or in
// the local snapshot. Only the banner differs: a retryable failure (Transport / RateLimited /
// ServerError) shows a Warning; a non-retryable one (Auth / InvalidRequest / Parse / Unknown) shows an
// Error because the user has to act.

#include <cstring>
#include <string>

namespace smatchet {
namespace catalogoffline {

enum class CatalogFailureBanner : unsigned char {
    WarningUsingCached,       ///< retryable; the in-memory catalog is kept
    WarningRestoredSnapshot,  ///< retryable; memory was empty and the disk snapshot was restored
    WarningSessionHadCatalog, ///< retryable; nothing to restore but this session had a catalog
    ErrorKeepCatalog,         ///< non-retryable; the catalog (memory or snapshot) is kept
    ErrorNoCatalog,           ///< nothing available at all
};

inline CatalogFailureBanner DecideCatalogFailureBanner(bool errorTransient, bool hasFieldsNow, bool snapshotLoaded,
                                                       bool everLoaded) {
    if (!errorTransient) {
        return (hasFieldsNow || snapshotLoaded) ? CatalogFailureBanner::ErrorKeepCatalog
                                                : CatalogFailureBanner::ErrorNoCatalog;
    }
    if (hasFieldsNow) {
        return CatalogFailureBanner::WarningUsingCached;
    }
    if (snapshotLoaded) {
        return CatalogFailureBanner::WarningRestoredSnapshot;
    }
    if (everLoaded) {
        return CatalogFailureBanner::WarningSessionHadCatalog;
    }
    return CatalogFailureBanner::ErrorNoCatalog;
}

/// Every catalog warning AppController builds embeds the raw fetch error after this marker.
constexpr const char* kLastFetchFailedMarker = " Last fetch failed: ";
/// Prefix of the warning set when the catalog was restored from the snapshot at startup.
constexpr const char* kWorkingOfflinePrefix = "Working offline:";

inline bool IsStartupSnapshotWarning(const std::string& warning) {
    const std::size_t n = std::strlen(kWorkingOfflinePrefix);
    return warning.size() >= n && warning.compare(0, n, kWorkingOfflinePrefix) == 0;
}

/// The raw fetch error embedded in a catalog warning, or "" when the warning carries none.
inline std::string ExtractLastFetchFailedDetail(const std::string& warning) {
    const std::size_t pos = warning.find(kLastFetchFailedMarker);
    if (pos == std::string::npos) {
        return std::string();
    }
    return warning.substr(pos + std::strlen(kLastFetchFailedMarker));
}

} // namespace catalogoffline
} // namespace smatchet
