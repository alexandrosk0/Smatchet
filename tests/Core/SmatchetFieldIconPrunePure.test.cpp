// The pure prune/revalidation rules of the field-icon cache directory.

#include "SmatchetFieldIconPrunePure.h"

#include "SmatchetFieldIconRender_detail.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace SmatchetFieldIconDiskCache;

namespace {

std::string IconName(int n) {
    return SmatchetFieldIconRender::detail::UrlToCacheFileName("https://x/" + std::to_string(n));
}

IconCacheFile File(const std::string& name, std::uint64_t size, std::int64_t modified) {
    IconCacheFile f;
    f.Name = name;
    f.SizeBytes = size;
    f.ModifiedEpochSec = modified;
    return f;
}

} // namespace

TEST_SUITE("SmatchetFieldIconPrunePure") {

    TEST_CASE("IsStale: age boundary, and a future timestamp is never stale") {
        CHECK(IsStale(0, kRevalidateAfterSeconds, kRevalidateAfterSeconds));
        CHECK_FALSE(IsStale(1, kRevalidateAfterSeconds, kRevalidateAfterSeconds));
        CHECK_FALSE(IsStale(5000, 1000, 10)); // modified "in the future" (clock moved back)
        CHECK(kRevalidateAfterSeconds == 7 * 24 * 3600);
    }

    TEST_CASE("legacy names are always victims; foreign files are never touched") {
        const std::vector<IconCacheFile> files = {
            File("icon_1a2b3c4d5e6f7a8b.bin", 10, 100), // legacy 64-bit-hash name
            File("icon_0.bin", 10, 100),                // legacy
            File("notes.txt", 5000, 0),                 // not ours
            File("icon_zz.bin", 5000, 0),               // not hex: not ours
            File(IconName(1), 10, 100),                 // current
        };
        const std::vector<std::string> victims = SelectIconPruneVictims(files, 1u << 20, 1000, 600);
        CHECK(victims == std::vector<std::string>{"icon_0.bin", "icon_1a2b3c4d5e6f7a8b.bin"});
    }

    TEST_CASE("orphaned temps go once old; young temps and lookalikes stay") {
        const std::string base = IconName(2);
        const std::vector<IconCacheFile> files = {
            File(base, 10, 100),
            File(base + ".part-12-3", 10, 100),  // old temp: victim
            File(base + ".part-12-4", 10, 900),  // young temp (age 100 < 600): kept
            File(base + ".part-12-x", 10, 100),  // not the exact suffix: not ours
            File("icon_abc.bin.part-1-1", 1, 0), // legacy-shaped base: not a current icon temp
        };
        const std::vector<std::string> victims = SelectIconPruneVictims(files, 1u << 20, 1000, 600);
        CHECK(victims == std::vector<std::string>{base + ".part-12-3"});
    }

    TEST_CASE("over the cap, the oldest current icons go first until it fits; ties break by name") {
        std::vector<IconCacheFile> files;
        files.push_back(File(IconName(10), 400, 300));
        files.push_back(File(IconName(11), 400, 100)); // oldest
        files.push_back(File(IconName(12), 400, 200));
        files.push_back(File(IconName(13), 400, 200)); // ties with 12 on age
        // 1600 bytes against a 700 cap: evict until <= 700 -> three go (oldest first), one stays.
        const std::vector<std::string> victims = SelectIconPruneVictims(files, 700, 1000, 600);
        REQUIRE(victims.size() == 3);
        CHECK(victims[0] == IconName(11));
        const std::string a = IconName(12);
        const std::string b = IconName(13);
        CHECK(victims[1] == (a < b ? a : b));
        CHECK(victims[2] == (a < b ? b : a));
    }

    TEST_CASE("at or under the cap nothing is evicted, and legacy bytes do not count toward it") {
        std::vector<IconCacheFile> files;
        files.push_back(File(IconName(20), 500, 100));
        files.push_back(File(IconName(21), 500, 200));
        files.push_back(File("icon_ab.bin", 100000, 0)); // legacy: deleted, never counted
        const std::vector<std::string> victims = SelectIconPruneVictims(files, 1000, 1000, 600);
        CHECK(victims == std::vector<std::string>{"icon_ab.bin"});
    }

    TEST_CASE("a zero cap evicts every current icon; an empty directory selects nothing") {
        std::vector<IconCacheFile> files;
        files.push_back(File(IconName(30), 1, 100));
        files.push_back(File(IconName(31), 1, 200));
        CHECK(SelectIconPruneVictims(files, 0, 1000, 600).size() == 2);
        CHECK(SelectIconPruneVictims(std::vector<IconCacheFile>(), 0, 1000, 600).empty());
    }
}
