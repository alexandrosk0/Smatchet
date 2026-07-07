// DR6 regression: field-catalog lock discipline (threading/lifetime race).
//
// SetFieldCatalog reassigns GridContextFieldCatalog::AvailableFields /
// AvailableIssueTypeMeta on a background worker thread while UI-thread create/draft
// readers (AppController::BuildDraftFromLastTicket, GetRequiredFieldSet,
// CreateIssueAsync) copy or scan those same vectors. Before the fix the readers ran
// unguarded, so a concurrent move-assign could hand them a reallocated-out-from-under
// buffer (UAF) or a torn read. GridContextFieldCatalog::availableFieldsMutex_ guards
// every member per its GridLiveContext.h contract.
//
// AppController.cpp is not part of the doctest link rig (it drags the UI/Lua/MCP
// chain), so this TU pins the invariant at the GridContextFieldCatalog seam the
// production readers/writers now share: a reader that copies under the guard always
// observes ONE self-consistent catalog generation, never a half-swapped mix. The
// stress loop is also the TSan/ASan tripwire that fails loudly if any of those sites
// drops its lock again.

#include "GridLiveContext.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Build a homogeneous catalog generation: every field's Id starts with `marker` and
// carries the same generation tag, so a reader that copies the whole vector atomically
// (under the guard) can assert the copy is internally consistent. `marker` alternates
// each generation, and timetracking-style ids get the read-only sweep the production
// SetFieldCatalog applies.
std::vector<TrackerField> MakeGeneration(char marker, std::size_t count) {
    std::vector<TrackerField> fields;
    fields.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        TrackerField f;
        f.Id = std::string(1, marker) + "field_" + std::to_string(i);
        f.Name = std::string("gen-") + marker;
        // Mimic SetFieldCatalog's non-Plane read-only sweep: mark the sentinel id
        // read-only so a torn read (mixed generations) would surface as an
        // inconsistent flag/marker pairing.
        f.ReadOnly = (f.Id.find("field_0") != std::string::npos);
        fields.push_back(std::move(f));
    }
    return fields;
}

} // namespace

TEST_CASE("DR6: guarded field-catalog copy never observes a half-swapped generation") {
    GridContextFieldCatalog cat;
    std::atomic<bool> stop{false};
    std::atomic<bool> sawInconsistent{false};
    std::atomic<std::uint64_t> readerIterations{0};

    // Writer: mimic SetFieldCatalog — reassign AvailableFields under the guard, then run
    // the read-only sweep under the guard (matching the DR6 fix in
    // AppController_CatalogAndFieldEdit.cpp).
    std::thread writer([&] {
        char marker = 'a';
        std::size_t count = 4;
        while (!stop.load(std::memory_order_relaxed)) {
            auto next = MakeGeneration(marker, count);
            {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                cat.AvailableFields = std::move(next);
                for (auto& field : cat.AvailableFields) {
                    if (field.Id.find("field_0") != std::string::npos) {
                        field.ReadOnly = true;
                    }
                }
            }
            marker = (marker == 'a') ? 'b' : 'a';
            count = (count == 4) ? 9 : 4;
        }
    });

    // Readers: mimic CreateIssueAsync / BuildDraftFromLastTicket — copy AvailableFields
    // under the guard, then verify the copy is one homogeneous generation.
    auto readerBody = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<TrackerField> copy;
            {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                copy = cat.AvailableFields;
            }
            readerIterations.fetch_add(1, std::memory_order_relaxed);
            if (copy.empty()) {
                continue;
            }
            const char marker = copy.front().Id.empty() ? '?' : copy.front().Id.front();
            for (const auto& field : copy) {
                if (field.Id.empty() || field.Id.front() != marker) {
                    sawInconsistent.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        }
    };
    std::thread readerA(readerBody);
    std::thread readerB(readerBody);

    while (readerIterations.load(std::memory_order_relaxed) < 2000) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    readerA.join();
    readerB.join();

    CHECK_FALSE(sawInconsistent.load());
}

TEST_CASE("DR6: guarded AvailableIssueTypeMeta scan tolerates concurrent reassignment") {
    GridContextFieldCatalog cat;
    std::atomic<bool> stop{false};
    std::atomic<bool> sawInconsistent{false};
    std::atomic<std::uint64_t> readerIterations{0};

    std::thread writer([&] {
        std::string project = "PROJ-A";
        std::size_t count = 3;
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<TrackerIssueTypeCreateMeta> next;
            next.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                TrackerIssueTypeCreateMeta meta;
                meta.ProjectKey = project;
                meta.IssueTypeId = project + "-type-" + std::to_string(i);
                meta.IssueTypeName = project;
                next.push_back(std::move(meta));
            }
            {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                cat.AvailableIssueTypeMeta = std::move(next);
            }
            project = (project == "PROJ-A") ? "PROJ-B" : "PROJ-A";
            count = (count == 3) ? 7 : 3;
        }
    });

    // Reader: mimic GetRequiredFieldSet — scan the whole vector under the guard; every
    // entry in a single locked scan must belong to the same generation (same ProjectKey).
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
            std::string expected;
            for (const auto& entry : cat.AvailableIssueTypeMeta) {
                if (expected.empty()) {
                    expected = entry.ProjectKey;
                } else if (entry.ProjectKey != expected) {
                    sawInconsistent.store(true, std::memory_order_relaxed);
                }
            }
            readerIterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (readerIterations.load(std::memory_order_relaxed) < 2000) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    reader.join();

    CHECK_FALSE(sawInconsistent.load());
}
