// LocalCacheManager AI-chat persistence unit tests (Phase 3 of
// ai-chat-claude-desktop-parity). In-memory SQLite round-trips for the
// Append + Load + UpdatePin + Trim + ClearAll surface, plus the
// "pinned exempt from trim" invariant called out in the design doc.

#if defined(SMATCHET_WITH_AI)

#include "../support/SqliteMemFixture.h"

#include "AiTypes.h"
#include "LocalCacheManager.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using smatchet_tests::SqliteMemFixture;

namespace {

AiMessage MakeMsg(const std::string& role, const std::string& content, std::int64_t createdAt, bool pinned = false) {
    AiMessage m;
    m.Role = role;
    m.Content = content;
    m.CreatedAtUnixMs = createdAt;
    m.Pinned = pinned;
    return m;
}

} // namespace

TEST_CASE("LocalCacheManager AI chat: Append assigns monotonically increasing row ids and Load round-trips") {
    SqliteMemFixture fix;
    const std::int64_t id1 = fix.Ref().AppendChatMessage(MakeMsg("user", "hello", 1000));
    const std::int64_t id2 = fix.Ref().AppendChatMessage(MakeMsg("assistant", "hi back", 1100));
    const std::int64_t id3 = fix.Ref().AppendChatMessage(MakeMsg("user", "how are you?", 1200));
    REQUIRE(id1 > 0);
    REQUIRE(id2 > id1);
    REQUIRE(id3 > id2);

    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(100, out, ids);
    REQUIRE(out.size() == 3u);
    REQUIRE(ids.size() == 3u);
    // Ascending-by-id order — chronological for the UI.
    CHECK(out[0].Role == "user");
    CHECK(out[0].Content == "hello");
    CHECK(out[0].CreatedAtUnixMs == 1000);
    CHECK(out[0].Pinned == false);
    CHECK(ids[0] == id1);
    CHECK(out[1].Role == "assistant");
    CHECK(ids[1] == id2);
    CHECK(out[2].Role == "user");
    CHECK(ids[2] == id3);
}

TEST_CASE("LocalCacheManager AI chat: Load with cap=0 returns empty") {
    SqliteMemFixture fix;
    fix.Ref().AppendChatMessage(MakeMsg("user", "x", 1));
    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(0, out, ids);
    CHECK(out.empty());
    CHECK(ids.empty());
}

TEST_CASE("LocalCacheManager AI chat: Load cap returns the most-recent N in chronological order") {
    SqliteMemFixture fix;
    for (int i = 0; i < 10; ++i) {
        fix.Ref().AppendChatMessage(MakeMsg("user", std::string("msg-") + std::to_string(i), 1000 + i));
    }
    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(3, out, ids);
    REQUIRE(out.size() == 3u);
    CHECK(out[0].Content == "msg-7");
    CHECK(out[1].Content == "msg-8");
    CHECK(out[2].Content == "msg-9");
}

TEST_CASE("LocalCacheManager AI chat: UpdatePin flips the pinned flag and round-trips through Load") {
    SqliteMemFixture fix;
    const std::int64_t id = fix.Ref().AppendChatMessage(MakeMsg("user", "pin me", 1000));
    fix.Ref().UpdateChatMessagePin(id, true);

    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(10, out, ids);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].Pinned == true);

    fix.Ref().UpdateChatMessagePin(id, false);
    fix.Ref().LoadChatMessages(10, out, ids);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].Pinned == false);
}

TEST_CASE("LocalCacheManager AI chat: Trim keeps the most-recent N non-pinned and ALWAYS preserves pinned rows" *
          doctest::test_suite("[high-risk]")) {
    SqliteMemFixture fix;
    // 5 rows total. ids 1, 3 pinned at insert; 2, 4, 5 unpinned.
    fix.Ref().AppendChatMessage(MakeMsg("user", "1-old-pinned", 1000, /*pinned=*/true));
    fix.Ref().AppendChatMessage(MakeMsg("user", "2-old-unpinned", 1100));
    fix.Ref().AppendChatMessage(MakeMsg("user", "3-mid-pinned", 1200, /*pinned=*/true));
    fix.Ref().AppendChatMessage(MakeMsg("user", "4-mid-unpinned", 1300));
    fix.Ref().AppendChatMessage(MakeMsg("user", "5-newest-unpinned", 1400));

    // Trim cap = 1 non-pinned. Expected survivors: rows 1 + 3 (pinned, exempt) + 5
    // (newest non-pinned). Rows 2 + 4 deleted.
    fix.Ref().TrimChatMessages(1);

    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(100, out, ids);
    REQUIRE(out.size() == 3u);
    CHECK(out[0].Content == "1-old-pinned");
    CHECK(out[0].Pinned == true);
    CHECK(out[1].Content == "3-mid-pinned");
    CHECK(out[1].Pinned == true);
    CHECK(out[2].Content == "5-newest-unpinned");
    CHECK(out[2].Pinned == false);
}

TEST_CASE("LocalCacheManager AI chat: Trim cap=0 removes every non-pinned row while preserving pinned rows") {
    SqliteMemFixture fix;
    fix.Ref().AppendChatMessage(MakeMsg("user", "drop1", 1));
    fix.Ref().AppendChatMessage(MakeMsg("user", "drop2", 2));
    fix.Ref().AppendChatMessage(MakeMsg("user", "keep", 3, /*pinned=*/true));
    fix.Ref().TrimChatMessages(0);

    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(100, out, ids);
    REQUIRE(out.size() == 1u);
    CHECK(out[0].Content == "keep");
    CHECK(out[0].Pinned == true);
}

TEST_CASE("LocalCacheManager AI chat: ClearChatMessages removes EVERY row including pinned") {
    SqliteMemFixture fix;
    fix.Ref().AppendChatMessage(MakeMsg("user", "a", 1));
    fix.Ref().AppendChatMessage(MakeMsg("user", "b", 2, /*pinned=*/true));
    fix.Ref().ClearChatMessages();

    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(100, out, ids);
    CHECK(out.empty());
    CHECK(ids.empty());
}

TEST_CASE("LocalCacheManager AI chat: hydration latency microbench (Phase 3.10 Pillar 2 evidence)") {
    // Phase 3.10 of ai-chat-claude-desktop-parity. Drives the dominant cost of
    // the first-frame hydration path (`LocalCacheManager::LoadChatMessages` against
    // 200 pre-existing rows — the inline-vs-async cutoff in
    // `HydrateFromConfigOnce`). Time per iteration is printed via MESSAGE so it
    // shows up in `ctest --output-on-failure` even when the case passes; the
    // measured number is recorded next to the production call site in
    // `SmatchetAiAssistantUi.cpp::HydrateFromConfigOnce` as the Pillar 2 budget
    // annotation. NOT a CI regression gate — the SmatchetTests rig has no
    // wall-clock budget. Run on the dev box to refresh the annotation when the
    // hydration path changes.
    SqliteMemFixture fix;
    constexpr int kSeedRows = 200;
    for (int i = 0; i < kSeedRows; ++i) {
        AiMessage m = MakeMsg(i % 2 == 0 ? "user" : "assistant",
                              std::string("seeded message body #") + std::to_string(i) +
                                  " — a sentence of representative length so the SQLite TEXT column "
                                  "moves a non-trivial number of bytes per row",
                              1700000000000LL + i);
        fix.Ref().AppendChatMessage(m);
    }

    constexpr int kIterations = 20;
    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    const auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < kIterations; ++it) {
        fix.Ref().LoadChatMessages(static_cast<std::size_t>(kSeedRows), out, ids);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double perIterUs = totalUs / static_cast<double>(kIterations);
    MESSAGE("LoadChatMessages(200) average: " << perIterUs << " us / iter (in-memory SQLite, " << kIterations
                                              << " iters)");
    CHECK(out.size() == static_cast<std::size_t>(kSeedRows));
    // Generous ceiling — 6940 us is the 6.94 ms UI-thread budget. Any value above
    // this is a hard Pillar 2 violation and signals the inline-hydration cutoff
    // needs lowering. In-memory SQLite measures sub-millisecond on modern dev
    // boxes; on-disk WAL is typically 2-5x slower but still well under budget.
#if defined(__SANITIZE_ADDRESS__)
    CHECK(perIterUs < 69400.0);  // ASAN ~3-10x wall-clock overhead; budget loosened (#1215 pattern)
#else
    CHECK(perIterUs < 6940.0);
#endif
}

TEST_CASE("LocalCacheManager AI chat: UpdatePin on missing id is graceful (no throw)") {
    SqliteMemFixture fix;
    // No append; just attempt to flip a non-existent row.
    fix.Ref().UpdateChatMessagePin(9999, true); // expects LOG_WARN, no exception
    std::vector<AiMessage> out;
    std::vector<std::int64_t> ids;
    fix.Ref().LoadChatMessages(10, out, ids);
    CHECK(out.empty());
}

#endif // SMATCHET_WITH_AI
