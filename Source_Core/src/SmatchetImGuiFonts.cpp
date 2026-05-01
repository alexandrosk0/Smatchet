#include "SmatchetImGuiFonts.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// Extra blocks beyond ImGui helpers: bullets (U+2022), ● (U+25CF), dingbats, arrows, box drawing, etc.
void AddExtendedSymbolRanges(ImFontGlyphRangesBuilder& builder, ImFontAtlas* atlas) {
    builder.AddRanges(atlas->GetGlyphRangesGreek());
    builder.AddRanges(atlas->GetGlyphRangesCyrillic());
    builder.AddRanges(atlas->GetGlyphRangesVietnamese());

    static const ImWchar kLatin1Supplement[] = {0x00A0, 0x00FF, 0};
    builder.AddRanges(kLatin1Supplement);
    static const ImWchar kGeneralPunctuation[] = {0x2000, 0x206F, 0};
    builder.AddRanges(kGeneralPunctuation);
    static const ImWchar kSupplementalPunctuation[] = {0x2E00, 0x2E7F, 0};
    builder.AddRanges(kSupplementalPunctuation);
    static const ImWchar kCurrencyLetterlikeNumberForms[] = {0x20A0, 0x218F, 0};
    builder.AddRanges(kCurrencyLetterlikeNumberForms);
    static const ImWchar kArrows[] = {0x2190, 0x21FF, 0};
    builder.AddRanges(kArrows);
    static const ImWchar kMathematicalOperators[] = {0x2200, 0x22FF, 0};
    builder.AddRanges(kMathematicalOperators);
    static const ImWchar kMiscTechnical[] = {0x2300, 0x243F, 0};
    builder.AddRanges(kMiscTechnical);
    static const ImWchar kEnclosedAlphanumerics[] = {0x2460, 0x24FF, 0};
    builder.AddRanges(kEnclosedAlphanumerics);
    static const ImWchar kBoxDrawingBlockElements[] = {0x2500, 0x259F, 0};
    builder.AddRanges(kBoxDrawingBlockElements);
    static const ImWchar kGeometricShapes[] = {0x25A0, 0x25FF, 0};
    builder.AddRanges(kGeometricShapes);
    static const ImWchar kMiscSymbols[] = {0x2600, 0x26FF, 0};
    builder.AddRanges(kMiscSymbols);
    static const ImWchar kDingbats[] = {0x2700, 0x27BF, 0};
    builder.AddRanges(kDingbats);
    static const ImWchar kMiscMathSymbolsA[] = {0x27C0, 0x27EF, 0};
    builder.AddRanges(kMiscMathSymbolsA);
    static const ImWchar kSupplementalArrowsA[] = {0x27F0, 0x27FF, 0};
    builder.AddRanges(kSupplementalArrowsA);
    static const ImWchar kBraille[] = {0x2800, 0x28FF, 0};
    builder.AddRanges(kBraille);
    static const ImWchar kMiscSymbolsAndArrows[] = {0x2B00, 0x2BFF, 0};
    builder.AddRanges(kMiscSymbolsAndArrows);
    static const ImWchar kCjkSymbolsAndPunctuation[] = {0x3000, 0x303F, 0};
    builder.AddRanges(kCjkSymbolsAndPunctuation);
}

#if defined(_WIN32)
bool FileExistsUtf8(const char* path) { return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
#endif

} // namespace

void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io) {
    static ImVector<ImWchar> glyph_ranges;
    ImFontGlyphRangesBuilder builder;
    AddExtendedSymbolRanges(builder, io.Fonts);
    glyph_ranges.clear();
    builder.BuildRanges(&glyph_ranges);

    ImFontConfig main_cfg;
    main_cfg.GlyphRanges = glyph_ranges.Data;

    io.Fonts->Clear();

    // Pixel size aligned with common ImGui default vector font (~16px).
    constexpr float kFontPixels = 16.0f;

#if defined(_WIN32)
    // Embedded ImGui fonts omit many symbol outlines; Segoe UI ships with Windows and covers ● • ∑ etc.
    static const char kSegoeUiPath[] = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (FileExistsUtf8(kSegoeUiPath)) {
        io.Fonts->AddFontFromFileTTF(kSegoeUiPath, kFontPixels, &main_cfg, glyph_ranges.Data);
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        merge_cfg.GlyphRanges = glyph_ranges.Data;
        merge_cfg.PixelSnapH = true;
        static const char kSegoeUiSymbolPath[] = "C:\\Windows\\Fonts\\seguisym.ttf";
        if (FileExistsUtf8(kSegoeUiSymbolPath)) {
            io.Fonts->AddFontFromFileTTF(kSegoeUiSymbolPath, kFontPixels, &merge_cfg, glyph_ranges.Data);
        }
        return;
    }
#endif

    io.Fonts->AddFontDefault(&main_cfg);
}
