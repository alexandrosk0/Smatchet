#include "SmatchetFieldIconPrunePure.h"

#include "FileIo.h"
#include "SmatchetFieldIconRender_detail.h"

#include <algorithm>

namespace SmatchetFieldIconDiskCache {

namespace {

// "<icon name>.part-<digits>-<digits>": the exact temp form of a current icon name.
bool IsIconTempName(const std::string& name) {
    const std::string::size_type marker = name.find(".bin.part-");
    if (marker == std::string::npos) {
        return false;
    }
    const std::string base = name.substr(0, marker + 4); // keep ".bin"
    return SmatchetFieldIconRender::detail::IsIconCacheFileName(base) && smatchet::fileio::IsTempSiblingOf(name, base);
}

} // namespace

std::vector<std::string> SelectIconPruneVictims(const std::vector<IconCacheFile>& files, std::uint64_t capBytes,
                                                std::int64_t nowEpochSec, std::int64_t tempGraceSeconds) {
    using SmatchetFieldIconRender::detail::IsIconCacheFileName;
    using SmatchetFieldIconRender::detail::IsLegacyIconCacheFileName;

    std::vector<std::string> victims;
    std::vector<IconCacheFile> current;
    std::uint64_t currentBytes = 0;
    for (const IconCacheFile& f : files) {
        if (IsLegacyIconCacheFileName(f.Name)) {
            victims.push_back(f.Name);
        } else if (IsIconTempName(f.Name)) {
            if (IsStale(f.ModifiedEpochSec, nowEpochSec, tempGraceSeconds)) {
                victims.push_back(f.Name);
            }
        } else if (IsIconCacheFileName(f.Name)) {
            current.push_back(f);
            currentBytes += f.SizeBytes;
        }
    }
    std::sort(victims.begin(), victims.end());
    std::sort(current.begin(), current.end(), [](const IconCacheFile& a, const IconCacheFile& b) {
        return a.ModifiedEpochSec != b.ModifiedEpochSec ? a.ModifiedEpochSec < b.ModifiedEpochSec : a.Name < b.Name;
    });
    // A zero cap means "drop them all", including empty files that would never push the total over it.
    for (std::size_t i = 0; i < current.size() && (capBytes == 0 || currentBytes > capBytes); ++i) {
        victims.push_back(current[i].Name);
        currentBytes -= current[i].SizeBytes;
    }
    return victims;
}

} // namespace SmatchetFieldIconDiskCache
