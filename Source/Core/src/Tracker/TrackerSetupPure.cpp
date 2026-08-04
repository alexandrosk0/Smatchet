#include "TrackerSetupPure.h"

#include <cstdint>

namespace TrackerSetupPure {
namespace {

// FNV-1a 64. Chosen over std::hash because the digest must be stable across runs and
// across the two call sites (probe completion vs Save & Sync) — libstdc++/MSVC are free
// to salt std::hash per process, which would make every comparison a mismatch.
const std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
const std::uint64_t kFnvPrime = 1099511628211ULL;

void HashField(std::uint64_t& h, const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(s[i]));
        h *= kFnvPrime;
    }
    // Length-delimit so ("ab","c") and ("a","bc") cannot collide.
    h ^= 0xFFULL;
    h *= kFnvPrime;
}

std::string ToHex(std::uint64_t h) {
    static const char* kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[h & 0xFULL];
        h >>= 4;
    }
    return out;
}

} // namespace

bool CredentialFieldsComplete(const TrackerConfig& cfg) {
    return ConfigManager::BackendCredentialsPresent(cfg, ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType));
}

bool NeedsSetup(const TrackerConfig& cfg) { return !cfg.BackendHasBeenReachable || !CredentialFieldsComplete(cfg); }

std::string CredentialFingerprint(const TrackerConfig& cfg) {
    const std::string backend = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    std::uint64_t h = kFnvOffsetBasis;
    HashField(h, backend);
    // Only the ACTIVE backend's fields participate: editing a Plane key while Jira is
    // selected must not invalidate a green Jira probe.
    if (backend == "Plane") {
        HashField(h, cfg.PlaneUrl);
        HashField(h, cfg.PlaneWorkspaceSlug);
        HashField(h, cfg.PlaneApiKey);
    } else if (backend == "GitHub") {
        HashField(h, cfg.GitHubBaseUrl);
        HashField(h, cfg.GitHubPat);
        HashField(h, cfg.GitHubOwner);
        HashField(h, cfg.GitHubRepo);
    } else if (backend == "Linear") {
        HashField(h, cfg.LinearBaseUrl);
        HashField(h, cfg.LinearApiKey);
        HashField(h, cfg.LinearTeamId);
        HashField(h, cfg.LinearTeamKey);
    } else {
        HashField(h, cfg.Domain);
        HashField(h, cfg.Email);
        HashField(h, cfg.ApiToken);
    }
    return ToHex(h);
}

bool ApplyVerifiedSaveUnlock(TrackerConfig& cfg, const std::string& verifiedFingerprint) {
    if (!cfg.ReadOnlyMode || verifiedFingerprint.empty() || CredentialFingerprint(cfg) != verifiedFingerprint) {
        return false;
    }
    cfg.ReadOnlyMode = false;
    // Same edge, both flags. The pin is only ever set on an AuthenticatedReachable probe —
    // the identical verdict the connectivity monitor latches on (SmatchetUI.cpp, first-launch
    // gate) — so this is that same latch reached one tick earlier, not a weaker gate.
    cfg.BackendHasBeenReachable = true;
    return true;
}

} // namespace TrackerSetupPure
