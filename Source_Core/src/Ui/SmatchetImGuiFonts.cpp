#include "SmatchetImGuiFonts.h"
#include "Logger.h"
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

#include <cstdio>
#include <string>

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
    if (fontName == "Segoe UI")
        return "C:\\Windows\\Fonts\\segoeui.ttf";
    if (fontName == "Consolas")
        return "C:\\Windows\\Fonts\\consola.ttf";
    if (fontName == "Arial")
        return "C:\\Windows\\Fonts\\arial.ttf";
    if (fontName == "Courier New")
        return "C:\\Windows\\Fonts\\cour.ttf";
    if (fontName == "Georgia")
        return "C:\\Windows\\Fonts\\georgia.ttf";
    if (fontName == "Lucida Console")
        return "C:\\Windows\\Fonts\\lucon.ttf";
    if (fontName == "Microsoft Sans Serif")
        return "C:\\Windows\\Fonts\\micross.ttf";
    if (fontName == "Trebuchet MS")
        return "C:\\Windows\\Fonts\\trebuc.ttf";
    if (fontName == "Verdana")
        return "C:\\Windows\\Fonts\\verdana.ttf";
    return nullptr; // Proggy (Clean/Default) or unknown
}

// Style-variant TTF paths for each supported font family. Used by the Markdown
// preview pane to render bold / italic / bold-italic runs without per-glyph
// font tricks. Any null/missing variant falls back to regular at load time.
// cppcheck-suppress unusedStructMember ; members read on Windows preview-font load path only
struct FontVariantPaths {
    const char* bold;
    const char* italic;
    const char* boldItalic;
};

FontVariantPaths GetFontVariantPaths(const std::string& fontName) {
    if (fontName == "Segoe UI") {
        return {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\segoeuii.ttf",
                "C:\\Windows\\Fonts\\segoeuiz.ttf"};
    }
    if (fontName == "Arial") {
        return {"C:\\Windows\\Fonts\\arialbd.ttf", "C:\\Windows\\Fonts\\ariali.ttf", "C:\\Windows\\Fonts\\arialbi.ttf"};
    }
    if (fontName == "Consolas") {
        return {"C:\\Windows\\Fonts\\consolab.ttf", "C:\\Windows\\Fonts\\consolai.ttf",
                "C:\\Windows\\Fonts\\consolaz.ttf"};
    }
    if (fontName == "Courier New") {
        return {"C:\\Windows\\Fonts\\courbd.ttf", "C:\\Windows\\Fonts\\couri.ttf", "C:\\Windows\\Fonts\\courbi.ttf"};
    }
    if (fontName == "Georgia") {
        return {"C:\\Windows\\Fonts\\georgiab.ttf", "C:\\Windows\\Fonts\\georgiai.ttf",
                "C:\\Windows\\Fonts\\georgiaz.ttf"};
    }
    if (fontName == "Trebuchet MS") {
        return {"C:\\Windows\\Fonts\\trebucbd.ttf", "C:\\Windows\\Fonts\\trebucit.ttf",
                "C:\\Windows\\Fonts\\trebucbi.ttf"};
    }
    if (fontName == "Verdana") {
        return {"C:\\Windows\\Fonts\\verdanab.ttf", "C:\\Windows\\Fonts\\verdanai.ttf",
                "C:\\Windows\\Fonts\\verdanaz.ttf"};
    }
    // Lucida Console, Microsoft Sans Serif, default — no separate variants on disk.
    return {nullptr, nullptr, nullptr};
}

// Font Awesome 6 TTF asset resolver (Phase 4 of ai-chat-claude-desktop-parity).
// The TTF ships separately at assets/fonts/fa-solid-900.ttf (SIL OFL 1.1) and is
// NOT committed to git — see assets/fonts/README.md for the drop-in contract.
// Resolution order: exe-dir first (POST_BUILD-copied by CMake at install) then
// SMATCHET_ASSETS_SOURCE_DIR/fonts (dev-tree fallback for runs out of the build
// directory / tests / scenarios). Missing in both → caller LOG_WARN + g_FaIconsLoaded=false.
#if defined(_WIN32)
std::string ResolveAssetTtfPath(const char* fileName) {
    if (!fileName || !*fileName) {
        return std::string();
    }
    // 1. exe-dir lookup. GetModuleFileNameW(nullptr, ...) returns the running
    //    process's full path; the FA TTF is POST_BUILD-copied to that same
    //    directory by the CMakeLists `add_custom_command` chain. Use the wide
    //    API + a wide→narrow ANSI conversion so non-ASCII install paths work.
    wchar_t exePathW[MAX_PATH] = {0};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        wchar_t* lastSlash = exePathW;
        for (wchar_t* p = exePathW; *p; ++p) {
            if (*p == L'\\' || *p == L'/') {
                lastSlash = p;
            }
        }
        *lastSlash = L'\0';
        char exeDirA[MAX_PATH] = {0};
        const int conv =
            WideCharToMultiByte(CP_UTF8, 0, exePathW, -1, exeDirA, static_cast<int>(sizeof(exeDirA)), nullptr, nullptr);
        if (conv > 0) {
            std::string candidate = std::string(exeDirA) + "\\" + fileName;
            if (FileExistsUtf8(candidate.c_str())) {
                return candidate;
            }
        }
    }
    // 2. Source-tree fallback (SMATCHET_ASSETS_SOURCE_DIR is defined at compile
    //    time by CMakeLists). Lets `Smatchet.exe` run from a dev shell / out of
    //    a sibling build directory still find the font on the source tree.
#if defined(SMATCHET_ASSETS_SOURCE_DIR)
    {
        std::string candidate = std::string(SMATCHET_ASSETS_SOURCE_DIR) + "/fonts/" + fileName;
        if (FileExistsUtf8(candidate.c_str())) {
            return candidate;
        }
    }
#endif
    return std::string();
}
#endif // _WIN32

// Set by SmatchetApplyImGuiFont after the atlas rebuild. UI consumers read via
// SmatchetAreFaIconsLoaded() — false → fall back to text labels. Atomic because
// the font-reload path may run on a worker (theme change picker) while the UI
// thread reads it during render.
std::atomic<bool> g_FaIconsLoaded{false};

// FA6 glyph range — covers ICON_MIN_FA (0xe005, includes THUMBTACK_SLASH) through
// ICON_MAX_FA (0xf8ff, includes COPY/THUMBTACK/TRASH/XMARK). Matches the upstream
// IconFontCppHeaders FA6 metadata. ImWchar element type required by ImGui's
// AddRanges() / atlas builder; final 0 sentinel mandated by the API.
static const ImWchar kFontAwesomeRange[] = {0xe005, 0xf8ff, 0};

// Thread-safe state for font hot-reloading
std::mutex g_FontReloadMutex;
std::string g_CurrentFontName = "Segoe UI";
float g_CurrentFontSize = 16.0f;
std::string g_PendingFontName;
float g_PendingFontSize = 0.0f;
std::atomic<bool> g_FontReloadRequested{false};

// Set by SmatchetApplyImGuiFont after the atlas rebuild. Pointers remain valid
// until the next SmatchetApplyImGuiFont call clears the atlas. Consumers go
// through SmatchetGetPreviewFonts() rather than reading this directly.
SmatchetPreviewFonts g_PreviewFonts;

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
    g_PreviewFonts = SmatchetPreviewFonts{};

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

        // Font Awesome 6 Solid merge — Phase 4 of ai-chat-claude-desktop-parity.
        // Drops the FA6 PUA range onto the atlas so the AI-chat hover row, pin
        // strip, and header buttons can render `ICON_FA_COPY`, `ICON_FA_THUMBTACK`,
        // `ICON_FA_THUMBTACK_SLASH`, `ICON_FA_XMARK`, `ICON_FA_TRASH`. Merge uses
        // a separate kFontAwesomeRange[] static (NOT the BuildRanges output) —
        // ImGui merges only intersect glyph ranges that overlap, so passing the
        // narrower FA-specific range avoids the atlas building empty bins for
        // every kFA glyph the body font doesn't have. Missing TTF → LOG_WARN +
        // g_FaIconsLoaded stays false; UI falls back to text labels.
        g_FaIconsLoaded.store(false, std::memory_order_release);
        const std::string faTtfPath = ResolveAssetTtfPath("fa-solid-900.ttf");
        if (!faTtfPath.empty()) {
            ImFontConfig fa_cfg;
            fa_cfg.MergeMode = true;
            fa_cfg.PixelSnapH = true;
            // FA glyphs visually align tighter when nudged ~1px down at typical UI
            // sizes — matches the upstream Dear ImGui FA-merge convention.
            fa_cfg.GlyphOffset.y = 1.0f;
            fa_cfg.GlyphMinAdvanceX = fontSizePixels;
            const ImFont* faFont =
                io.Fonts->AddFontFromFileTTF(faTtfPath.c_str(), fontSizePixels, &fa_cfg, kFontAwesomeRange);
            if (faFont != nullptr) {
                g_FaIconsLoaded.store(true, std::memory_order_release);
            } else {
                LOG_WARN("FA Solid TTF load failed (path=%s); falling back to text labels", faTtfPath.c_str());
            }
        } else {
            LOG_WARN("FA Solid TTF not found in exe-dir or SMATCHET_ASSETS_SOURCE_DIR; "
                     "AI chat hover row will render text labels. See assets/fonts/README.md.");
        }

        // Markdown preview style variants. Each variant is a separate ImFont (no
        // MergeMode) so the renderer can PushFont/PopFont per inline run. Symbol
        // and emoji merging is intentionally skipped on variants — the atlas would
        // ~5x in size; bold / italic text containing emoji is rare enough to live
        // with the missing-glyph fallback for now.
        const FontVariantPaths variants = GetFontVariantPaths(fontName);
        ImFontConfig variant_cfg;
        variant_cfg.GlyphRanges = glyph_ranges.Data;
        ImFont* boldFont = nullptr;
        ImFont* italicFont = nullptr;
        ImFont* boldItalicFont = nullptr;
        if (variants.bold && FileExistsUtf8(variants.bold)) {
            boldFont = io.Fonts->AddFontFromFileTTF(variants.bold, fontSizePixels, &variant_cfg, glyph_ranges.Data);
        }
        if (variants.italic && FileExistsUtf8(variants.italic)) {
            italicFont = io.Fonts->AddFontFromFileTTF(variants.italic, fontSizePixels, &variant_cfg, glyph_ranges.Data);
        }
        if (variants.boldItalic && FileExistsUtf8(variants.boldItalic)) {
            boldItalicFont =
                io.Fonts->AddFontFromFileTTF(variants.boldItalic, fontSizePixels, &variant_cfg, glyph_ranges.Data);
        }

        // Monospace pin: Consolas always, regardless of body font choice. This is
        // what users expect for inline `code` and ```code blocks``` in the preview.
        ImFont* monoFont = nullptr;
        static const char kConsolasPath[] = "C:\\Windows\\Fonts\\consola.ttf";
        if (FileExistsUtf8(kConsolasPath)) {
            monoFont = io.Fonts->AddFontFromFileTTF(kConsolasPath, fontSizePixels, &variant_cfg, glyph_ranges.Data);
        }

        g_PreviewFonts.Regular = newFont;
        g_PreviewFonts.Bold = boldFont ? boldFont : newFont;
        g_PreviewFonts.Italic = italicFont ? italicFont : newFont;
        g_PreviewFonts.BoldItalic = boldItalicFont ? boldItalicFont : (boldFont ? boldFont : newFont);
        g_PreviewFonts.Mono = monoFont ? monoFont : newFont;

        std::lock_guard<std::mutex> lock(g_FontReloadMutex);
        g_CurrentFontName = fontName;
        g_CurrentFontSize = fontSizePixels;
        io.FontDefault = newFont;
        return;
    }
#endif

    newFont = io.Fonts->AddFontDefault(&main_cfg);
    g_PreviewFonts.Regular = newFont;
    g_PreviewFonts.Bold = newFont;
    g_PreviewFonts.Italic = newFont;
    g_PreviewFonts.BoldItalic = newFont;
    g_PreviewFonts.Mono = newFont;
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_CurrentFontName = "Proggy (Clean/Default)";
    g_CurrentFontSize = fontSizePixels;
    io.FontDefault = newFont;
}

const SmatchetPreviewFonts& SmatchetGetPreviewFonts() { return g_PreviewFonts; }

bool SmatchetAreFaIconsLoaded() { return g_FaIconsLoaded.load(std::memory_order_acquire); }

void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io) { SmatchetApplyImGuiFont(io, "Segoe UI", 16.0f); }

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
