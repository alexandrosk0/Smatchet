// DR6 regression: field-catalog lock discipline (threading/lifetime race).
//
// Worker threads replace GridContextFieldCatalog::AvailableFields / AvailableIssueTypeMeta
// (the field-catalog fetch, the async snapshot restore, a backend switch) while UI-thread
// readers iterate them (AppController::GetAvailableFields callers, GetRequiredFieldSet,
// CreateIssueAsync). GridContextFieldCatalog::availableFieldsMutex_ guards every member per
// its GridLiveContext.h contract.
//
// AvailableFields is copy-on-write: a published generation is immutable, writers swap the
// shared_ptr under the mutex, and GetAvailableFields hands out that pointer, so a reader
// iterates with no lock held. Before that, GetAvailableFields returned a bare reference into
// the vector a worker could move-assign over mid-iteration (UAF / torn read).
//
// AppController.cpp is not part of the doctest link rig (it drags the UI/Lua/MCP chain), so
// this TU pins the invariant at the GridContextFieldCatalog seam the production readers and
// writers share; AppControllerFieldCatalog.posix.test.cpp drives the real controller. The
// stress loops double as the TSan/ASan tripwire (this TU is in the SmatchetTsanTests subset).

#include "GridLiveContext.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using FieldsSnapshot = std::shared_ptr<const std::vector<TrackerField>>;

// Build a homogeneous catalog generation: every field's Id starts with `marker`, and the
// generation size is tied to the marker, so a reader can tell a whole generation from a
// half-swapped or freed one. Field 0 is read-only, as SetFieldCatalog's read-only sweep marks
// the timetracking ids, so a torn read would also surface as a mismatched flag.
std::vector<TrackerField> MakeGeneration(char marker, std::size_t count) {
    std::vector<TrackerField> fields;
    fields.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        TrackerField f;
        f.Id = std::string(1, marker) + "field_" + std::to_string(i);
        f.Name = std::string("gen-") + marker;
        f.ReadOnly = (i == 0);
        fields.push_back(std::move(f));
    }
    return fields;
}

std::size_t GenerationSize(char marker) { return marker == 'a' ? 4 : 9; }

// The production writer shape (ApplyFieldCatalogInto / FinishCatalogError / the startup
// restore): the fixups run on the unpublished vector, then one swap under the mutex.
void Publish(GridContextFieldCatalog& cat, char marker) {
    FieldsSnapshot next = std::make_shared<std::vector<TrackerField>>(MakeGeneration(marker, GenerationSize(marker)));
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.AvailableFields = std::move(next);
}

// The explicit clear's write, as a tracker switch starts with: a fresh empty generation, swapped
// in under the mutex.
void ClearCatalog(GridContextFieldCatalog& cat) {
    FieldsSnapshot empty = std::make_shared<std::vector<TrackerField>>();
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    cat.AvailableFields = std::move(empty);
}

// The production reader shape (AppController::GetAvailableFields): copy the pointer under the
// mutex, nothing else.
FieldsSnapshot Snapshot(const GridContextFieldCatalog& cat) {
    std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
    return cat.AvailableFields;
}

// True when `fields` is exactly one whole generation (or the empty initial catalog).
bool IsOneGeneration(const std::vector<TrackerField>& fields) {
    if (fields.empty()) {
        return true;
    }
    const char marker = fields.front().Id.empty() ? '?' : fields.front().Id.front();
    if (fields.size() != GenerationSize(marker)) {
        return false;
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const TrackerField& field = fields[i];
        if (field.Id.empty() || field.Id.front() != marker || field.Name != std::string("gen-") + marker ||
            field.ReadOnly != (i == 0)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("AvailableFields is never null, even on a fresh catalog") {
    GridContextFieldCatalog cat;
    const FieldsSnapshot fields = Snapshot(cat);
    REQUIRE(fields.get() != nullptr);
    CHECK(fields->empty());
}

TEST_CASE("A held AvailableFields snapshot survives the catalog being replaced and cleared") {
    // The UI holds a snapshot across a frame (the grid's catalog index points into it) while a
    // worker lands a new catalog and a tracker switch then clears it. The held generation must
    // stay intact and alive: the writers swap the pointer, never the vector it points to.
    GridContextFieldCatalog cat;
    Publish(cat, 'a');
    const FieldsSnapshot held = Snapshot(cat);
    REQUIRE(held->size() == GenerationSize('a'));
    const TrackerField* const firstField = &held->front();

    Publish(cat, 'b');
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        cat.AvailableFields = std::make_shared<std::vector<TrackerField>>(); // the explicit clear's write
    }

    CHECK(Snapshot(cat)->empty());
    CHECK(held.use_count() == 1); // the reader is the last owner; the catalog moved on
    CHECK(&held->front() == firstField);
    CHECK(IsOneGeneration(*held));
    CHECK(held->front().Id == "afield_0");
}

TEST_CASE("FindFieldSnapshot is null on a fresh catalog and for an unknown id, and finds a published field") {
    GridContextFieldCatalog cat;
    CHECK(cat.FindFieldSnapshot("afield_0").get() == nullptr);

    Publish(cat, 'a');
    CHECK(cat.FindFieldSnapshot("zfield_0").get() == nullptr);
    CHECK(cat.FindFieldSnapshot(std::string()).get() == nullptr);

    const std::shared_ptr<const TrackerField> first = cat.FindFieldSnapshot("afield_0");
    const std::shared_ptr<const TrackerField> last = cat.FindFieldSnapshot("afield_3");
    REQUIRE(first.get() != nullptr);
    REQUIRE(last.get() != nullptr);
    CHECK(first->Id == "afield_0");
    CHECK(first->ReadOnly);
    CHECK(last->Id == "afield_3");
    CHECK_FALSE(last->ReadOnly);
}

TEST_CASE("A FindFieldSnapshot handle keeps its generation alive while the catalog is replaced and cleared") {
    // A command holds the field it looked up across many tracker round-trips while a worker lands a new
    // catalog and a tracker switch clears it. The handle owns a share of the generation the field lives in,
    // so the field stays readable, and it is the published field itself, never a copy.
    GridContextFieldCatalog cat;
    Publish(cat, 'a');
    std::weak_ptr<const std::vector<TrackerField>> generation;
    const TrackerField* publishedField = nullptr;
    {
        const FieldsSnapshot published = Snapshot(cat);
        generation = published;
        publishedField = &published->back();
    }
    std::shared_ptr<const TrackerField> handle = cat.FindFieldSnapshot("afield_3");
    REQUIRE(handle.get() != nullptr);
    CHECK(handle.get() == publishedField);

    Publish(cat, 'b');
    ClearCatalog(cat);

    REQUIRE_FALSE(generation.expired());
    CHECK(handle->Id == "afield_3");
    CHECK(handle->Name == "gen-a");
    CHECK_FALSE(handle->ReadOnly);
    CHECK(cat.FindFieldSnapshot("afield_3").get() == nullptr); // a new lookup reads the cleared catalog

    handle.reset();
    CHECK(generation.expired()); // the handle was the generation's last owner
}

TEST_CASE("DR6: lock-free iteration of an AvailableFields snapshot races catalog swaps safely") {
    GridContextFieldCatalog cat;
    std::atomic<bool> stop{false};
    std::atomic<bool> sawInconsistent{false};
    std::atomic<std::uint64_t> readerIterations{0};
    std::atomic<std::uint64_t> writerSwaps{0};

    // Writer: the catalog fetch / snapshot restore / backend switch, swapping generations (and an
    // occasional clear, as a tracker switch starts with) as fast as it can.
    std::thread writer([&] {
        char marker = 'a';
        std::uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (++n % 16 == 0) {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                cat.AvailableFields = std::make_shared<std::vector<TrackerField>>();
            } else {
                Publish(cat, marker);
                marker = (marker == 'a') ? 'b' : 'a';
            }
            writerSwaps.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Readers: the UI thread's GetAvailableFields callers. Take the snapshot, then walk it twice
    // with NO lock held and a yield in between, so swaps land while the reader is mid-iteration.
    auto readerBody = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const FieldsSnapshot fields = Snapshot(cat);
            const bool whole = fields && IsOneGeneration(*fields);
            std::this_thread::yield();
            if (!whole || !IsOneGeneration(*fields)) {
                sawInconsistent.store(true, std::memory_order_relaxed);
            }
            readerIterations.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread readerA(readerBody);
    std::thread readerB(readerBody);

    while (readerIterations.load(std::memory_order_relaxed) < 2000 ||
           writerSwaps.load(std::memory_order_relaxed) < 2000) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    readerA.join();
    readerB.join();

    CHECK_FALSE(sawInconsistent.load());
}

TEST_CASE("FindFieldSnapshot handles stay readable while a writer swaps and clears the catalog") {
    // Two readers take a field handle and read it with no lock held, yielding in between so swaps land while
    // the handle is live. A handle that did not own its generation would read freed memory here.
    GridContextFieldCatalog cat;
    const std::uint64_t kIterations = 2000;
    std::atomic<bool> stop{false};
    std::atomic<bool> sawInconsistent{false};
    std::atomic<std::uint64_t> readerAIterations{0};
    std::atomic<std::uint64_t> readerBIterations{0};
    std::atomic<std::uint64_t> writerSwaps{0};

    std::thread writer([&] {
        for (std::uint64_t n = 1; !stop.load(std::memory_order_relaxed); ++n) {
            if (n % 16 == 0) {
                ClearCatalog(cat);
            } else {
                Publish(cat, n % 2 == 0 ? 'a' : 'b');
            }
            writerSwaps.store(n, std::memory_order_relaxed);
        }
    });

    // The last field of each generation: neither is the read-only first field.
    const auto readerBody = [&](std::atomic<std::uint64_t>& iterations) {
        std::uint64_t n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const std::string id = (++n % 2 == 0) ? "afield_3" : "bfield_8";
            const std::shared_ptr<const TrackerField> handle = cat.FindFieldSnapshot(id);
            std::this_thread::yield();
            if (handle && (handle->Id != id || handle->Name != std::string("gen-") + id.front() || handle->ReadOnly)) {
                sawInconsistent.store(true, std::memory_order_relaxed);
            }
            iterations.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread readerA([&] { readerBody(readerAIterations); });
    std::thread readerB([&] { readerBody(readerBIterations); });

    // No REQUIRE while the threads run: it throws, and unwinding past a joinable thread would terminate.
    while (readerAIterations.load(std::memory_order_relaxed) < kIterations ||
           readerBIterations.load(std::memory_order_relaxed) < kIterations ||
           writerSwaps.load(std::memory_order_relaxed) < kIterations) {
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
