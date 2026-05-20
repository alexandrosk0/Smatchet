// Bucket-A doctest for MarkdownPreviewRender's BuildPlan + HashContent API.
//
// Verifies the parse-side seam introduced by the perf Opt 1 + Opt B
// refactor: BuildPlan must be pure (no ImGui calls), idempotent for the
// same input, produce a non-empty plan for non-empty markdown, and
// HashContent must be deterministic + non-trivial (zero hash on non-empty
// input would defeat the cache key).
//
// Render-side coverage (the per-word draw via ImDrawList::AddText, the
// SelectableText overlay, the link hit-test) is bucket E because all of
// it requires an ImGui context. This test stays pure-logic.

#include "MarkdownPreviewRender.h"
#include "SmatchetImGuiFonts.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

// Test-only stubs — MarkdownPreviewRender.cpp's RenderPlan path references
// these production symbols, but the pure-logic tests never invoke RenderPlan
// (no ImGui context). Provide trivial stubs so the test exe links without
// pulling SmatchetImGuiFonts.cpp + CppSyntaxHighlight.cpp + their full
// transitive ImGui font-atlas / preview-font dependency stack.
const SmatchetPreviewFonts& SmatchetGetPreviewFonts() {
    static SmatchetPreviewFonts s_stub;
    return s_stub;
}

void DrawColoredCppText(const char*) {
    // No-op — only called from RenderPlan which we don't exercise here.
}

namespace {

using namespace MarkdownPreviewRender;

} // namespace

TEST_CASE("HashContent — empty input yields the FNV-1a offset basis") {
    // FNV-1a 64-bit offset basis (0xcbf29ce484222325). Stable across runs;
    // any caller relying on the empty-string hash will see this constant.
    CHECK(HashContent("", 0) == 0xcbf29ce484222325ULL);
    CHECK(HashContent(std::string()) == 0xcbf29ce484222325ULL);
}

TEST_CASE("HashContent — non-empty input is non-trivial") {
    const std::uint64_t h1 = HashContent("hello");
    const std::uint64_t h2 = HashContent("world");
    CHECK(h1 != 0);
    CHECK(h2 != 0);
    CHECK(h1 != h2);
    // The empty-string hash is the FNV offset basis; non-empty input must
    // diverge from it.
    CHECK(h1 != 0xcbf29ce484222325ULL);
}

TEST_CASE("HashContent — same bytes produce same hash (cache-key contract)") {
    const std::string s1 = "# Hello\n\nA paragraph.";
    const std::string s2 = s1;
    CHECK(HashContent(s1) == HashContent(s2));
    // Mutating a single byte must change the hash (FNV-1a avalanche).
    std::string s3 = s1;
    s3[0] = 'x';
    CHECK(HashContent(s3) != HashContent(s1));
}

TEST_CASE("BuildPlan — empty markdown yields an empty plan") {
    PreviewPlanPtr planPtr = MakePlan();
    REQUIRE(planPtr != nullptr);
    BuildPlan("", *planPtr);
    CHECK(planPtr->blocks.empty());
}

TEST_CASE("BuildPlan — single paragraph emits one kPara block") {
    PreviewPlanPtr planPtr = MakePlan();
    BuildPlan("Hello world.", *planPtr);
    REQUIRE_FALSE(planPtr->blocks.empty());
    // First (and only) block must be kPara — single-paragraph markdown has
    // no headings / lists / code / tables.
    CHECK(planPtr->blocks[0].kind == PreviewPlan::Block::kPara);
    CHECK_FALSE(planPtr->blocks[0].runs.empty());
}

TEST_CASE("BuildPlan — heading emits kHeading with the correct level") {
    PreviewPlanPtr planPtr = MakePlan();
    BuildPlan("## A subsection", *planPtr);
    REQUIRE_FALSE(planPtr->blocks.empty());
    CHECK(planPtr->blocks[0].kind == PreviewPlan::Block::kHeading);
    CHECK(planPtr->blocks[0].headingLevel == 2);
}

TEST_CASE("BuildPlan — fenced code block captured as kCode with lang tag") {
    const std::string md = "```cpp\nint x = 42;\n```";
    PreviewPlanPtr planPtr = MakePlan();
    BuildPlan(md, *planPtr);
    // Expect exactly one kCode block somewhere in the plan.
    int codeCount = 0;
    PreviewPlan::Block::Kind firstCodeKind = PreviewPlan::Block::kPara;
    for (std::size_t i = 0; i < planPtr->blocks.size(); ++i) {
        if (planPtr->blocks[i].kind == PreviewPlan::Block::kCode) {
            ++codeCount;
            if (codeCount == 1) {
                firstCodeKind = planPtr->blocks[i].kind;
                CHECK(planPtr->blocks[i].codeLang == "cpp");
                CHECK(planPtr->blocks[i].codeBuffer.find("int x = 42;") != std::string::npos);
            }
        }
    }
    CHECK(codeCount == 1);
    CHECK(firstCodeKind == PreviewPlan::Block::kCode);
}

TEST_CASE("BuildPlan — idempotent on repeat calls (cache-rebuild contract)") {
    const std::string md = "# Heading\n\nBody.\n";
    PreviewPlanPtr p1 = MakePlan();
    PreviewPlanPtr p2 = MakePlan();
    BuildPlan(md, *p1);
    BuildPlan(md, *p2);
    REQUIRE(p1->blocks.size() == p2->blocks.size());
    for (std::size_t i = 0; i < p1->blocks.size(); ++i) {
        CHECK(p1->blocks[i].kind == p2->blocks[i].kind);
        CHECK(p1->blocks[i].headingLevel == p2->blocks[i].headingLevel);
        CHECK(p1->blocks[i].runs.size() == p2->blocks[i].runs.size());
    }
}

TEST_CASE("BuildPlan — re-using a plan clears prior state") {
    PreviewPlanPtr planPtr = MakePlan();
    BuildPlan("# First", *planPtr);
    const std::size_t firstCount = planPtr->blocks.size();
    BuildPlan("A paragraph.", *planPtr);
    // After the second BuildPlan, the plan must reflect ONLY the new content
    // — the first heading block must NOT survive.
    CHECK(planPtr->blocks.size() >= 1);
    bool sawHeading = false;
    for (std::size_t i = 0; i < planPtr->blocks.size(); ++i) {
        if (planPtr->blocks[i].kind == PreviewPlan::Block::kHeading) {
            sawHeading = true;
        }
    }
    CHECK_FALSE(sawHeading);
    (void)firstCount;
}

TEST_CASE("BuildPlan — horizontal rule emits kHr") {
    PreviewPlanPtr planPtr = MakePlan();
    BuildPlan("Above.\n\n---\n\nBelow.\n", *planPtr);
    int hrCount = 0;
    for (std::size_t i = 0; i < planPtr->blocks.size(); ++i) {
        if (planPtr->blocks[i].kind == PreviewPlan::Block::kHr) {
            ++hrCount;
        }
    }
    CHECK(hrCount == 1);
}
