#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
#include "SmatchetUiDensity.h"
#include "imgui.h"

#include <cmath>

namespace {

// Per-frame ImGui style lives in a global state owned by the active context. ApplyStyle()
// dereferences ImGui::GetStyle(), so every test_case must build a context, run the apply, then
// destroy. RAII fixture keeps the lifetime tight and re-entrant across SUBCASE.
struct ImGuiCtxFixture {
    ImGuiCtxFixture() : ctx_(ImGui::CreateContext()) {}
    ~ImGuiCtxFixture() {
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    ImGuiContext* ctx_;
};

// Channel-wise byte equality is overkill for floats stored as 0..1; doctest's default tolerance
// matches against floats but the palette literals in SmatchetTheme.cpp are also stored as floats,
// so equality is safe for hard-coded constants. Helper exists to keep CHECK lines readable.
bool ApproxEq(const float (&a)[4], const float (&b)[4]) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > 1e-5f) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes SmatchetDark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    const float keyword[4] = {0.78f, 0.50f, 1.00f, 1.0f};
    const float strLit[4] = {0.95f, 0.65f, 0.45f, 1.0f};
    const float comment[4] = {0.45f, 0.75f, 0.45f, 1.0f};
    const float number[4] = {0.65f, 0.85f, 1.00f, 1.0f};
    const float preproc[4] = {0.85f, 0.85f, 0.50f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes ModernDark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::ModernDark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // ModernDark shares the SmatchetDark syntax family (only chrome diverges) — encoded explicitly
    // so a future divergence between the two trips the assertion instead of silently passing.
    const float keyword[4] = {0.78f, 0.50f, 1.00f, 1.0f};
    CHECK(ApproxEq(s.Keyword, keyword));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes Vs2022Dark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // VS Code Dark+ canonical: keyword #569CD6, string #CE9178, comment #6A9955, number #B5CEA8,
    // preproc #9B9B9B. Source-of-truth lives in SmatchetTheme.cpp ApplyVs2022Dark.
    const float keyword[4] = {0.34f, 0.61f, 0.84f, 1.0f};
    const float strLit[4] = {0.81f, 0.57f, 0.47f, 1.0f};
    const float comment[4] = {0.42f, 0.60f, 0.33f, 1.0f};
    const float number[4] = {0.71f, 0.81f, 0.66f, 1.0f};
    const float preproc[4] = {0.61f, 0.61f, 0.61f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes Vs2022Light syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // VS Code Light+ canonical: keyword #0000FF (pure blue), string #A31515, comment #008000,
    // number #098658, preproc #808080. Chosen for legibility on white WindowBg.
    const float keyword[4] = {0.00f, 0.00f, 1.00f, 1.0f};
    const float strLit[4] = {0.64f, 0.08f, 0.08f, 1.0f};
    const float comment[4] = {0.00f, 0.50f, 0.00f, 1.0f};
    const float number[4] = {0.04f, 0.53f, 0.35f, 1.0f};
    const float preproc[4] = {0.50f, 0.50f, 0.50f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes HighContrast syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // Fully saturated primaries — keyword yellow, string magenta, comment green, number cyan,
    // preproc orange. Picked for low-vision / accessibility audits on pure-black background.
    const float keyword[4] = {1.00f, 1.00f, 0.00f, 1.0f};
    const float strLit[4] = {1.00f, 0.00f, 1.00f, 1.0f};
    const float comment[4] = {0.00f, 1.00f, 0.00f, 1.0f};
    const float number[4] = {0.00f, 1.00f, 1.00f, 1.0f};
    const float preproc[4] = {1.00f, 0.65f, 0.00f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes NortonCommander syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // Norton Commander DOS tribute — keyword bright yellow #FFFF55, string light red, comment light
    // gray, number bright cyan #55FFFF, preproc bright green. Source-of-truth in SmatchetTheme.cpp
    // ApplyNortonCommander.
    const float keyword[4] = {1.00f, 1.00f, 0.333f, 1.0f};
    const float strLit[4] = {1.00f, 0.50f, 0.50f, 1.0f};
    const float comment[4] = {0.667f, 0.667f, 0.667f, 1.0f};
    const float number[4] = {0.333f, 1.00f, 1.00f, 1.0f};
    const float preproc[4] = {0.333f, 1.00f, 0.333f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle writes ImGuiDefaultDark syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::ImGuiDefaultDark);
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();

    // ImGuiDefaultDark mirrors SmatchetDark's syntax family (only the chrome — provided by
    // ImGui::StyleColorsDark — diverges). Encoded explicitly so a future hue split between
    // the two trips the assertion instead of silently passing.
    const float keyword[4] = {0.78f, 0.50f, 1.00f, 1.0f};
    const float strLit[4] = {0.95f, 0.65f, 0.45f, 1.0f};
    const float comment[4] = {0.45f, 0.75f, 0.45f, 1.0f};
    const float number[4] = {0.65f, 0.85f, 1.00f, 1.0f};
    const float preproc[4] = {0.85f, 0.85f, 0.50f, 1.0f};

    CHECK(ApproxEq(s.Keyword, keyword));
    CHECK(ApproxEq(s.String, strLit));
    CHECK(ApproxEq(s.Comment, comment));
    CHECK(ApproxEq(s.Number, number));
    CHECK(ApproxEq(s.Preprocessor, preproc));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture,
                  "SmatchetTheme::ApplyStyle — every theme populates the slice-6 Identifier syntax color") {
    // Per-theme Identifier color pin (Slice 6 of code-syntax-coloring-and-tooltips.md).
    // Previously identifiers fell through to ImGuiCol_Text (no visible color
    // distinct from plain text); the per-theme Identifier field now drives
    // identifier rendering in CppSyntaxHighlight + CodeColorView. This test
    // pins the exact per-theme RGBA so silent regressions trip.
    struct ThemeIdentExpect {
        ThemeId id;
        float expected[4];
    };
    const ThemeIdentExpect cases[] = {
        {ThemeId::SmatchetDark, {0.62f, 0.80f, 0.92f, 1.0f}},
        {ThemeId::ModernDark, {0.62f, 0.80f, 0.92f, 1.0f}},
        {ThemeId::Vs2022Dark, {0.61f, 0.86f, 0.99f, 1.0f}},
        {ThemeId::Vs2022Light, {0.00f, 0.06f, 0.50f, 1.0f}},
        {ThemeId::HighContrast, {1.00f, 1.00f, 1.00f, 1.0f}},
        {ThemeId::NortonCommander, {1.00f, 1.00f, 1.00f, 1.0f}},
        {ThemeId::ImGuiDefaultDark, {0.62f, 0.80f, 0.92f, 1.0f}},
    };
    for (const ThemeIdentExpect& c : cases) {
        SmatchetTheme::ApplyStyle(c.id);
        const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();
        CHECK(ApproxEq(s.Identifier, c.expected));
    }
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle ImGuiDefaultDark restores ImGui default WindowBg") {
    // Pin the user-visible promise of ImGuiDefaultDark: WindowBg = ImGui's built-in dark default
    // (#0F0F0F — 0.0588f / 0x0F on each RGB channel). Switching SmatchetDark → ImGuiDefaultDark
    // must produce that exact bg, and round-tripping back to SmatchetDark must NOT poison the
    // ImGui defaults.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    SmatchetTheme::ApplyStyle(ThemeId::ImGuiDefaultDark);
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

    // ImGui::StyleColorsDark() sets WindowBg = ImVec4(0.06f, 0.06f, 0.06f, 0.94f) per imgui.cpp.
    // Encode the documented constants verbatim — any future ImGui upgrade that shifts the dark
    // palette baseline trips this assertion deliberately so we re-validate the user-facing
    // "show me what ImGui ships" promise.
    CHECK(std::fabs(bg.x - 0.06f) < 1e-3f);
    CHECK(std::fabs(bg.y - 0.06f) < 1e-3f);
    CHECK(std::fabs(bg.z - 0.06f) < 1e-3f);
    CHECK(std::fabs(bg.w - 0.94f) < 1e-3f);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle ImGuiDefaultDark skips ApplyCommonStyle rounding") {
    // ImGuiDefaultDark deliberately bypasses ApplyCommonStyle so rounding / padding stay at
    // the ImGui defaults (the whole point of this theme is to surface ImGui's out-of-the-box
    // look). ApplyCommonStyle would override WindowRounding to 6.0 — assert it stays at the
    // ImGui ctor default (0.0).
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    CHECK(std::fabs(ImGui::GetStyle().WindowRounding - 6.0f) < 1e-5f);

    SmatchetTheme::ApplyStyle(ThemeId::ImGuiDefaultDark);
    // ImGuiStyle{} ctor leaves WindowRounding at 0.0; ApplyImGuiDefaultDark must not touch it.
    CHECK(std::fabs(ImGui::GetStyle().WindowRounding) < 1e-5f);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle keyword color diverges between themes") {
    // Round-trip proof for the user-facing claim — switching theme actually recolors the syntax
    // palette in the next frame. Captures the SmatchetDark / Vs2022Dark / Vs2022Light /
    // HighContrast keyword channel and asserts pairwise inequality across the families. ModernDark
    // is intentionally not in this check because it currently shares SmatchetDark's syntax family.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const float smatchetKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float vsDarkKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const float vsLightKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::HighContrast);
    const float highContrastKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const float nortonKeyword[4] = {
        SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
        SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    CHECK_FALSE(ApproxEq(smatchetKeyword, vsDarkKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(smatchetKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, vsLightKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsDarkKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(vsLightKeyword, highContrastKeyword));
    CHECK_FALSE(ApproxEq(vsLightKeyword, nortonKeyword));
    CHECK_FALSE(ApproxEq(highContrastKeyword, nortonKeyword));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle is idempotent for syntax palette") {
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float first[4] = {SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
                            SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Dark);
    const float second[4] = {SmatchetTheme::GetSyntaxColors().Keyword[0], SmatchetTheme::GetSyntaxColors().Keyword[1],
                             SmatchetTheme::GetSyntaxColors().Keyword[2], SmatchetTheme::GetSyntaxColors().Keyword[3]};

    CHECK(ApproxEq(first, second));
}

namespace {
struct StyleSnapshot {
    ImVec4 colors[ImGuiCol_COUNT];
    float WindowRounding;
    float ChildRounding;
    float FrameRounding;
    float PopupRounding;
    float ScrollbarRounding;
    float GrabRounding;
    float TabRounding;
    ImVec2 WindowPadding;
    ImVec2 FramePadding;
    ImVec2 ItemSpacing;
    float ScrollbarSize;
};

StyleSnapshot SnapshotStyle() {
    const ImGuiStyle& s = ImGui::GetStyle();
    StyleSnapshot out;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        out.colors[i] = s.Colors[i];
    }
    out.WindowRounding = s.WindowRounding;
    out.ChildRounding = s.ChildRounding;
    out.FrameRounding = s.FrameRounding;
    out.PopupRounding = s.PopupRounding;
    out.ScrollbarRounding = s.ScrollbarRounding;
    out.GrabRounding = s.GrabRounding;
    out.TabRounding = s.TabRounding;
    out.WindowPadding = s.WindowPadding;
    out.FramePadding = s.FramePadding;
    out.ItemSpacing = s.ItemSpacing;
    out.ScrollbarSize = s.ScrollbarSize;
    return out;
}

bool Vec4Eq(const ImVec4& a, const ImVec4& b) {
    return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f &&
           std::fabs(a.w - b.w) < 1e-5f;
}

bool Vec2Eq(const ImVec2& a, const ImVec2& b) { return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f; }
} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture,
                  "SmatchetTheme::ApplyStyle round-trips full ImGuiStyle for SmatchetDark<->NortonCommander") {
    // Mimic the host's boot sequence (SmatchetImGuiHost.cpp:449-451): StyleColorsDark first, then
    // ApplyStyle. The round-trip path must reach the same state without re-running StyleColorsDark.
    ImGui::StyleColorsDark();
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot before = SnapshotStyle();

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);

    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot after = SnapshotStyle();

    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        INFO("ImGuiCol_ index ", i);
        CHECK(Vec4Eq(before.colors[i], after.colors[i]));
    }
    CHECK(before.WindowRounding == after.WindowRounding);
    CHECK(before.ChildRounding == after.ChildRounding);
    CHECK(before.FrameRounding == after.FrameRounding);
    CHECK(before.PopupRounding == after.PopupRounding);
    CHECK(before.ScrollbarRounding == after.ScrollbarRounding);
    CHECK(before.GrabRounding == after.GrabRounding);
    CHECK(before.TabRounding == after.TabRounding);
    CHECK(Vec2Eq(before.WindowPadding, after.WindowPadding));
    CHECK(Vec2Eq(before.FramePadding, after.FramePadding));
    CHECK(Vec2Eq(before.ItemSpacing, after.ItemSpacing));
    CHECK(before.ScrollbarSize == after.ScrollbarSize);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle round-trips full ImGuiStyle through every theme") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const StyleSnapshot before = SnapshotStyle();

    const ThemeId others[] = {ThemeId::ModernDark, ThemeId::Vs2022Dark, ThemeId::Vs2022Light, ThemeId::HighContrast,
                              ThemeId::NortonCommander};
    for (ThemeId t : others) {
        SmatchetTheme::ApplyStyle(t);
        SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
        const StyleSnapshot after = SnapshotStyle();
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            INFO("Detour theme=", static_cast<int>(t), " ImGuiCol_ index=", i);
            CHECK(Vec4Eq(before.colors[i], after.colors[i]));
        }
    }
}

// Field-by-field round-trip — pins EVERY ImGuiCol_* slot the runtime exposes (not just the 55 the
// SmatchetTheme apply functions explicitly write). Even one un-rewritten slot that the intermediate
// theme mutated (via a future PushStyleColor leak or a partial apply path) would surface here as
// a visible "residual color" the user reports as "switching back doesn't restore SmatchetDark".
//
// Layered onto the existing StyleSnapshot test: that one declared a StyleSnapshot with
// `ImVec4 colors[ImGuiCol_COUNT]` but only iterates `ImGuiCol_COUNT` slots — already comprehensive.
// This test exists to surface a *behavioural* contract the existing test phrased only as a
// property check: every slot the round-trip touches must be bit-identical to a fresh apply
// performed on a never-mutated style (an ImGuiStyle{} with StyleColorsDark seeded by its ctor).
TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle SmatchetDark every-slot matches fresh-style apply") {
    // Reference: a brand-new ImGuiStyle (ctor calls StyleColorsDark + sets every layout field).
    // ApplyStyle on this is what "fresh" SmatchetDark looks like. We capture the full Colors[]
    // array post-apply, then exercise the user-reported A->B->A path and assert every single slot
    // matches the fresh path — INCLUDING the 11 slots no SmatchetTheme apply function explicitly
    // writes (TextLink / TreeLines / NavCursor / TabSelected / etc.).
    ImGui::StyleColorsDark();
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    ImVec4 freshColors[ImGuiCol_COUNT];
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        freshColors[i] = ImGui::GetStyle().Colors[i];
    }

    // User-reported sequence: SmatchetDark → every other theme → SmatchetDark.
    const ThemeId others[] = {ThemeId::ModernDark, ThemeId::Vs2022Dark, ThemeId::Vs2022Light, ThemeId::HighContrast,
                              ThemeId::NortonCommander};
    for (ThemeId detour : others) {
        SmatchetTheme::ApplyStyle(detour);
        SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            INFO("Detour theme=", static_cast<int>(detour), " slot=", i, " fresh=(", freshColors[i].x, ",",
                 freshColors[i].y, ",", freshColors[i].z, ",", freshColors[i].w, ") after=(",
                 ImGui::GetStyle().Colors[i].x, ",", ImGui::GetStyle().Colors[i].y, ",", ImGui::GetStyle().Colors[i].z,
                 ",", ImGui::GetStyle().Colors[i].w, ")");
            CHECK(Vec4Eq(freshColors[i], ImGui::GetStyle().Colors[i]));
        }
    }
}

// Sentinel-injection regression — pins the seed-baseline contract added to ApplyStyle. Pokes a
// neon-magenta sentinel into the 11 slots that the per-theme override functions do NOT explicitly
// rewrite (TextLink, TreeLines, InputTextCursor, TabSelectedOverline, TabDimmedSelectedOverline,
// DragDropTargetBg, UnsavedMarker, NavCursor / aliases for TabSelected / TabDimmed /
// TabDimmedSelected). If ApplyStyle ever drops its StyleColorsDark/Light seed pass — the change
// that fixes the user-reported residual-color bug — the sentinel will survive the next ApplyStyle
// and this CHECK trips. Equivalent of forcing a PushStyleColor leak on those slots.
TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle re-seeds baseline so injected sentinels are wiped") {
    const ImVec4 sentinel(1.0f, 0.0f, 1.0f, 1.0f); // neon magenta — none of our themes use this
    const int slotsToProbe[] = {
        ImGuiCol_TextLink,
        ImGuiCol_TreeLines,
        ImGuiCol_InputTextCursor,
        ImGuiCol_TabSelected,
        ImGuiCol_TabSelectedOverline,
        ImGuiCol_TabDimmed,
        ImGuiCol_TabDimmedSelected,
        ImGuiCol_TabDimmedSelectedOverline,
        ImGuiCol_DragDropTargetBg,
        ImGuiCol_UnsavedMarker,
        ImGuiCol_NavCursor,
    };

    // Establish a known-good SmatchetDark baseline.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    ImVec4 cleanBaseline[ImGuiCol_COUNT];
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        cleanBaseline[i] = ImGui::GetStyle().Colors[i];
    }

    // Inject the sentinel into every probed slot (simulates an external PushStyleColor leak or a
    // future theme that touches more slots than the apply path expects).
    for (int slot : slotsToProbe) {
        ImGui::GetStyle().Colors[slot] = sentinel;
    }

    // Switch theme. ApplyStyle's seed-baseline pass should restore the unwritten slots.
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);

    // Every probed slot must match the original SmatchetDark baseline — the sentinel must be gone.
    for (int slot : slotsToProbe) {
        INFO("Probed slot=", slot, " expected=(", cleanBaseline[slot].x, ",", cleanBaseline[slot].y, ",",
             cleanBaseline[slot].z, ",", cleanBaseline[slot].w, ") actual=(", ImGui::GetStyle().Colors[slot].x, ",",
             ImGui::GetStyle().Colors[slot].y, ",", ImGui::GetStyle().Colors[slot].z, ",",
             ImGui::GetStyle().Colors[slot].w, ")");
        CHECK(Vec4Eq(cleanBaseline[slot], ImGui::GetStyle().Colors[slot]));
    }
}

// Non-color sentinel-injection — pins the FULL-style reset (`style = ImGuiStyle{}`) contract.
// The earlier sentinel test only proved the Colors[] array gets re-seeded. The deeper bug the user
// reported was that the ~50 layout fields ImGuiStyle exposes outside Colors[] — WindowBorderSize,
// IndentSpacing, ItemInnerSpacing, CellPadding, DisabledAlpha, ScrollbarPadding, AntiAliased*
// toggles, HoverDelay*, etc. — were leaking across switches because ApplyCommonStyle only resets
// 11 of them and no per-theme apply touches the rest. If ApplyStyle ever drops the full reset, a
// poisoned layout field will survive the next ApplyStyle and these CHECKs trip. Picks a
// representative subset of layout fields a future theme is likely to override.
TEST_CASE_FIXTURE(ImGuiCtxFixture,
                  "SmatchetTheme::ApplyStyle re-seeds full ImGuiStyle so non-color sentinels are wiped") {
    // Establish a known-good SmatchetDark baseline for the layout fields.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const float expectedWindowBorderSize = ImGui::GetStyle().WindowBorderSize;
    const float expectedChildBorderSize = ImGui::GetStyle().ChildBorderSize;
    const float expectedPopupBorderSize = ImGui::GetStyle().PopupBorderSize;
    const float expectedFrameBorderSize = ImGui::GetStyle().FrameBorderSize;
    const float expectedTabBorderSize = ImGui::GetStyle().TabBorderSize;
    const float expectedTabBarBorderSize = ImGui::GetStyle().TabBarBorderSize;
    const float expectedIndentSpacing = ImGui::GetStyle().IndentSpacing;
    const ImVec2 expectedItemInnerSpacing = ImGui::GetStyle().ItemInnerSpacing;
    const ImVec2 expectedCellPadding = ImGui::GetStyle().CellPadding;
    const float expectedGrabMinSize = ImGui::GetStyle().GrabMinSize;
    const float expectedScrollbarPadding = ImGui::GetStyle().ScrollbarPadding;
    const float expectedDisabledAlpha = ImGui::GetStyle().DisabledAlpha;
    const float expectedAlpha = ImGui::GetStyle().Alpha;

    // Poison every layout field a future per-theme override might mutate. None of these match the
    // ImGuiStyle{} ctor defaults so the assertion only passes if the reset path runs.
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowBorderSize = 99.0f;
    s.ChildBorderSize = 99.0f;
    s.PopupBorderSize = 99.0f;
    s.FrameBorderSize = 99.0f;
    s.TabBorderSize = 99.0f;
    s.TabBarBorderSize = 99.0f;
    s.IndentSpacing = 999.0f;
    s.ItemInnerSpacing = ImVec2(99.0f, 99.0f);
    s.CellPadding = ImVec2(99.0f, 99.0f);
    s.GrabMinSize = 99.0f;
    s.ScrollbarPadding = 99.0f;
    s.DisabledAlpha = 0.123f;
    s.Alpha = 0.456f;

    // Switch theme — both the detour and the return path must reset the poisoned fields.
    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);

    // Every layout field must match the original SmatchetDark baseline — the poison must be gone.
    CHECK(std::fabs(expectedWindowBorderSize - ImGui::GetStyle().WindowBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedChildBorderSize - ImGui::GetStyle().ChildBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedPopupBorderSize - ImGui::GetStyle().PopupBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedFrameBorderSize - ImGui::GetStyle().FrameBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedTabBorderSize - ImGui::GetStyle().TabBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedTabBarBorderSize - ImGui::GetStyle().TabBarBorderSize) < 1e-5f);
    CHECK(std::fabs(expectedIndentSpacing - ImGui::GetStyle().IndentSpacing) < 1e-5f);
    CHECK(Vec2Eq(expectedItemInnerSpacing, ImGui::GetStyle().ItemInnerSpacing));
    CHECK(Vec2Eq(expectedCellPadding, ImGui::GetStyle().CellPadding));
    CHECK(std::fabs(expectedGrabMinSize - ImGui::GetStyle().GrabMinSize) < 1e-5f);
    CHECK(std::fabs(expectedScrollbarPadding - ImGui::GetStyle().ScrollbarPadding) < 1e-5f);
    CHECK(std::fabs(expectedDisabledAlpha - ImGui::GetStyle().DisabledAlpha) < 1e-5f);
    CHECK(std::fabs(expectedAlpha - ImGui::GetStyle().Alpha) < 1e-5f);
}

// Density-preservation policy: SmatchetUI re-runs ApplyDensityToImGuiStyle after every
// SmatchetTheme::ApplyStyle. The test pins the policy — push density, apply a theme, re-push
// density, and assert the user's spacing survives. Without the re-push, ApplyCommonStyle
// reverts ItemSpacing / FramePadding to Normal defaults silently.
TEST_CASE_FIXTURE(ImGuiCtxFixture, "Density survives theme switch when re-applied after ApplyStyle") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    const ImVec2 compactItem = ImGui::GetStyle().ItemSpacing;
    const ImVec2 compactFrame = ImGui::GetStyle().FramePadding;
    CHECK(Vec2Eq(compactItem, ImVec2(4.0f, 2.0f)));
    CHECK(Vec2Eq(compactFrame, ImVec2(4.0f, 2.0f)));

    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    // Right after a raw ApplyStyle the spacing reverts to Normal density — that is the regression
    // SmatchetUI guards against by re-pushing density immediately after.
    CHECK_FALSE(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, compactFrame));

    // Round-trip back to the original theme also preserves density once re-pushed.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, compactItem));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, compactFrame));
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "ApplyDensityToImGuiStyle writes the documented constants") {
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Compact);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(4.0f, 2.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(4.0f, 2.0f)));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Comfortable);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(10.0f, 8.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(8.0f, 6.0f)));

    smatchet::ui_density::ApplyDensityToImGuiStyle(TrackerConfig::UiDensity::Normal);
    CHECK(Vec2Eq(ImGui::GetStyle().ItemSpacing, ImVec2(8.0f, 6.0f)));
    CHECK(Vec2Eq(ImGui::GetStyle().FramePadding, ImVec2(6.0f, 4.0f)));
}
