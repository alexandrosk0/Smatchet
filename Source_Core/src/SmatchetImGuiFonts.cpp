#include "SmatchetImGuiFonts.h"
#include "imgui_internal.h"

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

#if defined(IMGUI_USE_WCHAR32)
    // Supplementary Multilingual Plane: emoji / pictographs (Plane onboarding titles, GitHub, etc.).
    // Requires IMGUI_USE_WCHAR32 so glyph builder and text layout support codepoints > U+FFFF.
    static const ImWchar kRegionalIndicators[] = {0x1F1E6, 0x1F1FF, 0}; // flag pairs
    builder.AddRanges(kRegionalIndicators);
    static const ImWchar kEmojiAndPictographs[] = {0x1F300, 0x1FAFF, 0};
    builder.AddRanges(kEmojiAndPictographs);
    static const ImWchar kVariationSelectors[] = {0xFE00, 0xFE0F, 0};
    builder.AddRanges(kVariationSelectors);
#endif
}

#if defined(_WIN32)
bool FileExistsUtf8(const char* path) { return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
#endif

} // namespace

#include <string>
#include <mutex>
#include <atomic>

namespace {

const char* GetFontFilePath(const std::string& fontName) {
    if (fontName == "Segoe UI") return "C:\\Windows\\Fonts\\segoeui.ttf";
    if (fontName == "Consolas") return "C:\\Windows\\Fonts\\consola.ttf";
    if (fontName == "Arial") return "C:\\Windows\\Fonts\\arial.ttf";
    if (fontName == "Courier New") return "C:\\Windows\\Fonts\\cour.ttf";
    if (fontName == "Georgia") return "C:\\Windows\\Fonts\\georgia.ttf";
    if (fontName == "Lucida Console") return "C:\\Windows\\Fonts\\lucon.ttf";
    if (fontName == "Microsoft Sans Serif") return "C:\\Windows\\Fonts\\micross.ttf";
    if (fontName == "Trebuchet MS") return "C:\\Windows\\Fonts\\trebuc.ttf";
    if (fontName == "Verdana") return "C:\\Windows\\Fonts\\verdana.ttf";
    return nullptr; // Proggy (Clean/Default) or unknown
}

// Thread-safe state for font hot-reloading
std::mutex g_FontReloadMutex;
std::string g_CurrentFontName = "Segoe UI";
float g_CurrentFontSize = 16.0f;
std::string g_PendingFontName;
float g_PendingFontSize = 0.0f;
std::atomic<bool> g_FontReloadRequested{false};

} // namespace

void SmatchetApplyImGuiFont(ImGuiIO& io, const std::string& fontName, float fontSizePixels) {
    static ImVector<ImWchar> glyph_ranges;
    ImFontGlyphRangesBuilder builder;
    AddExtendedSymbolRanges(builder, io.Fonts);
    glyph_ranges.clear();
    builder.BuildRanges(&glyph_ranges);

    ImFontConfig main_cfg;
    main_cfg.GlyphRanges = glyph_ranges.Data;

    // Reset dangling pointers on active context and ImGuiIO to prevent access violation crashes during Clear()
    io.FontDefault = nullptr;
    if (ImGui::GetCurrentContext()) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        g.Font = nullptr;
        g.DrawListSharedData.Font = nullptr;
    }

    io.Fonts->Clear();

    ImFont* newFont = nullptr;

#if defined(_WIN32)
    const char* path = GetFontFilePath(fontName);
    if (path && FileExistsUtf8(path)) {
        newFont = io.Fonts->AddFontFromFileTTF(path, fontSizePixels, &main_cfg, glyph_ranges.Data);

        // Merge Segoe UI symbols and emojis for premium character rendering
        ImFontConfig merge_cfg;
        merge_cfg.MergeMode = true;
        merge_cfg.GlyphRanges = glyph_ranges.Data;
        merge_cfg.PixelSnapH = true;
        static const char kSegoeUiSymbolPath[] = "C:\\Windows\\Fonts\\seguisym.ttf";
        if (FileExistsUtf8(kSegoeUiSymbolPath)) {
            io.Fonts->AddFontFromFileTTF(kSegoeUiSymbolPath, fontSizePixels, &merge_cfg, glyph_ranges.Data);
        }
        static const char kSegoeUiEmojiPath[] = "C:\\Windows\\Fonts\\seguiemj.ttf";
        if (FileExistsUtf8(kSegoeUiEmojiPath)) {
            ImFontConfig emoji_cfg;
            emoji_cfg.MergeMode = true;
            emoji_cfg.GlyphRanges = glyph_ranges.Data;
            emoji_cfg.PixelSnapH = true;
            io.Fonts->AddFontFromFileTTF(kSegoeUiEmojiPath, fontSizePixels, &emoji_cfg, glyph_ranges.Data);
        }

        std::lock_guard<std::mutex> lock(g_FontReloadMutex);
        g_CurrentFontName = fontName;
        g_CurrentFontSize = fontSizePixels;
        io.FontDefault = newFont;
        return;
    }
#endif

    newFont = io.Fonts->AddFontDefault(&main_cfg);
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_CurrentFontName = "Proggy (Clean/Default)";
    g_CurrentFontSize = fontSizePixels;
    io.FontDefault = newFont;
}

void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io) {
    SmatchetApplyImGuiFont(io, "Segoe UI", 16.0f);
}

void SmatchetRequestFontReload(const std::string& fontName, float fontSizePixels) {
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_PendingFontName = fontName;
    g_PendingFontSize = fontSizePixels;
    g_FontReloadRequested.store(true);
}

bool SmatchetCheckAndApplyFontReload() {
    if (!g_FontReloadRequested.load()) {
        return false;
    }
    g_FontReloadRequested.store(false);

    std::string targetFontName;
    float targetFontSize = 16.0f;
    {
        std::lock_guard<std::mutex> lock(g_FontReloadMutex);
        targetFontName = g_PendingFontName;
        targetFontSize = g_PendingFontSize;
    }

    ImGuiIO& io = ImGui::GetIO();
    SmatchetApplyImGuiFont(io, targetFontName, targetFontSize);
    return true;
}





