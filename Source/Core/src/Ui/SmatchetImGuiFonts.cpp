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
#include <vector>

// WCHAR32 ABI parity guard (build-quality-velocity-hardening #23). IMGUI_USE_WCHAR32 is
// hand-synced across three points — SmatchetImConfig.h, the Unreal SmatchetImGuiPlugin.Build.cs,
// and the packaged imconfig.h — with no compile-time check. A desync silently narrows ImWchar to
// 16-bit, which would corrupt the glyph-range arrays built below (and the font-atlas ABI at
// large). This TU compiles into both the GLFW and DX12 targets, so the assert guards both worlds:
// a future desync fails the build instead of corrupting text memory at runtime.
static_assert(sizeof(ImWchar) == 4, "IMGUI_USE_WCHAR32 desynced: ImWchar must be 32-bit. Check SmatchetImConfig.h, "
                                    "SmatchetImGuiPlugin.Build.cs, and the packaged imconfig.h.");

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
// The TTF is committed at assets/fonts/fa-solid-900.ttf (SIL OFL 1.1) — see
// assets/fonts/README.md for the sourcing and license notes.
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

// Host-injected font bytes (#12, mobile host-injection seam). A host with no system-font
// path Core can read (Android — Core TUs carry no <android/asset_manager.h>) calls
// SmatchetSetInjectedFontBytes with a raw TTF blob *before* the first font build; the bytes
// are copied into this Core-owned buffer and loaded via AddFontFromMemoryTTF on any platform.
// Empty buffer means "no injection — use the per-platform default font path below".
//
// CPP_CODE_AUDIT.md #28: the live atlas holds a raw pointer into this buffer
// (FontDataOwnedByAtlas=false — see ApplyInjectedFontIfPresent below), valid for the
// atlas's lifetime, i.e. until the next SmatchetApplyImGuiFont call Clear()s it. A
// SmatchetSetInjectedFontBytes call that mutates g_InjectedFontBytes in place while a
// built atlas still references the OLD bytes would dangle that pointer (reallocation).
// Host re-injections therefore land in g_PendingInjectedFontBytes instead; the swap into
// the live buffer happens in SmatchetApplyImGuiFont right after io.Fonts->Clear(), by
// which point nothing references the old buffer anymore.
std::vector<unsigned char> g_InjectedFontBytes;
float g_InjectedFontSizePixels = 0.0f;
std::vector<unsigned char> g_PendingInjectedFontBytes;
float g_PendingInjectedFontSizePixels = 0.0f;
bool g_HasPendingInjectedFontBytes = false;

// Set by SmatchetApplyImGuiFont after the atlas rebuild. Pointers remain valid
// until the next SmatchetApplyImGuiFont call clears the atlas. Consumers go
// through SmatchetGetPreviewFonts() rather than reading this directly.
SmatchetPreviewFonts g_PreviewFonts;

#if defined(_WIN32)
// Markdown-variant + monospace fonts resolved alongside the body font. Each is a separate ImFont
// (no MergeMode) so the renderer can PushFont/PopFont per inline run.
struct MarkdownVariantFonts {
    ImFont* bold = nullptr;
    ImFont* italic = nullptr;
    ImFont* boldItalic = nullptr;
    ImFont* mono = nullptr;
};

// Font Awesome 6 Solid merge — Phase 4 of ai-chat-claude-desktop-parity.
// Drops the FA6 PUA range onto the atlas so the AI-chat hover row, pin
// strip, and header buttons can render `ICON_FA_COPY`, `ICON_FA_THUMBTACK`,
// `ICON_FA_THUMBTACK_SLASH`, `ICON_FA_XMARK`, `ICON_FA_TRASH`. Merge uses
// a separate kFontAwesomeRange[] static (NOT the BuildRanges output) —
// ImGui merges only intersect glyph ranges that overlap, so passing the
// narrower FA-specific range avoids the atlas building empty bins for
// every kFA glyph the body font doesn't have. Missing TTF → LOG_WARN +
// g_FaIconsLoaded stays false; UI falls back to text labels.
void MergeFontAwesomeSolid(ImGuiIO& io, float fontSizePixels) {
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
}

// Markdown preview style variants + the Consolas monospace pin. Each variant is a separate ImFont
// (no MergeMode). Symbol and emoji merging is intentionally skipped on variants — the atlas would
// ~5x in size; bold / italic text containing emoji is rare enough to live with the missing-glyph
// fallback for now. Consolas is pinned regardless of body font choice for inline `code` blocks.
MarkdownVariantFonts BuildMarkdownVariantFonts(ImGuiIO& io, const std::string& fontName, float fontSizePixels,
                                               const ImWchar* glyphRanges) {
    MarkdownVariantFonts out;
    const FontVariantPaths variants = GetFontVariantPaths(fontName);
    ImFontConfig variant_cfg;
    variant_cfg.GlyphRanges = glyphRanges;
    if (variants.bold && FileExistsUtf8(variants.bold)) {
        out.bold = io.Fonts->AddFontFromFileTTF(variants.bold, fontSizePixels, &variant_cfg, glyphRanges);
    }
    if (variants.italic && FileExistsUtf8(variants.italic)) {
        out.italic = io.Fonts->AddFontFromFileTTF(variants.italic, fontSizePixels, &variant_cfg, glyphRanges);
    }
    if (variants.boldItalic && FileExistsUtf8(variants.boldItalic)) {
        out.boldItalic = io.Fonts->AddFontFromFileTTF(variants.boldItalic, fontSizePixels, &variant_cfg, glyphRanges);
    }
    static const char kConsolasPath[] = "C:\\Windows\\Fonts\\consola.ttf";
    if (FileExistsUtf8(kConsolasPath)) {
        out.mono = io.Fonts->AddFontFromFileTTF(kConsolasPath, fontSizePixels, &variant_cfg, glyphRanges);
    }
    return out;
}

// Bug-report censor font. ImGui 1.92 bakes glyphs ON DEMAND, so restricting
// the glyph range does NOT stop other glyphs loading — instead we load the
// body font + merged FA normally, then AddRemapChar() every TEXT codepoint to
// U+2588 (█). When this font is active the whole UI renders as blocks while FA
// icons stay real. Used for the bug-report screenshot capture frame.
ImFont* BuildRedactionFont(ImGuiIO& io, const char* path, float fontSizePixels, const ImWchar* glyphRanges) {
    ImFontConfig redact_cfg;
    redact_cfg.GlyphRanges = glyphRanges;
    ImFont* redactFont = io.Fonts->AddFontFromFileTTF(path, fontSizePixels, &redact_cfg, glyphRanges);
    if (redactFont != nullptr) {
        const std::string faRedactPath = ResolveAssetTtfPath("fa-solid-900.ttf");
        if (!faRedactPath.empty()) {
            ImFontConfig fa_redact;
            fa_redact.MergeMode = true;
            fa_redact.PixelSnapH = true;
            fa_redact.GlyphOffset.y = 1.0f;
            fa_redact.GlyphMinAdvanceX = fontSizePixels;
            io.Fonts->AddFontFromFileTTF(faRedactPath.c_str(), fontSizePixels, &fa_redact, kFontAwesomeRange);
        }
        redactFont->FallbackChar = static_cast<ImWchar>(0x2588);
        // Point every text codepoint at the block glyph; keep the FA icon
        // range (0xe005..0xf8ff) and the block itself real.
        for (const ImWchar* gr = glyphRanges; gr[0] != 0 && gr[1] != 0; gr += 2) {
            for (unsigned cp = gr[0]; cp <= gr[1]; ++cp) {
                if ((cp >= 0xe005 && cp <= 0xf8ff) || cp == 0x2588) {
                    continue;
                }
                redactFont->AddRemapChar(static_cast<ImWchar>(cp), static_cast<ImWchar>(0x2588));
            }
        }
    }
    return redactFont;
}
#endif

// Build the UI font from host-injected TTF bytes (#12, mobile seam). Returns true and fully
// populates io.FontDefault + g_PreviewFonts + current-font state when injection is active and
// the blob parses; returns false (atlas left as the caller's prior Clear left it) when no bytes
// are injected or the TTF fails to parse, so the caller falls through to its per-platform default.
// Body font + extended glyph_ranges only — FA / symbol / markdown-variant merges live behind
// #if _WIN32 (system-font helpers), so they're absent here and every preview font aliases the
// body font, exactly like the AddFontDefault fallback. FA-from-injected-blob is a follow-up.
bool TryBuildInjectedFont(ImGuiIO& io, const ImFontConfig& main_cfg, const ImWchar* glyph_ranges) {
    if (g_InjectedFontBytes.empty() || g_InjectedFontSizePixels <= 0.0f) {
        return false;
    }
    // Pre-validate the blob before AddFontFromMemoryTTF. In asserts-on builds (debug + the
    // ninja-msvc-asan CI preset) ImGui's stb_truetype path IM_ASSERTs on a malformed blob and
    // aborts; a NULL return only degrades gracefully under NDEBUG. Checking the sfnt magic + a
    // sane minimum size here makes the documented fallback deterministic on every build config
    // (Pillar 3 — never crash). 100 bytes is below any real font yet above truncated garbage.
    if (g_InjectedFontBytes.size() < 100) {
        LOG_WARN("Injected font blob too small (%d bytes); falling back to default font",
                 static_cast<int>(g_InjectedFontBytes.size()));
        return false;
    }
    const unsigned char* magicBytes = g_InjectedFontBytes.data();
    const unsigned int sfntMagic =
        (static_cast<unsigned int>(magicBytes[0]) << 24) | (static_cast<unsigned int>(magicBytes[1]) << 16) |
        (static_cast<unsigned int>(magicBytes[2]) << 8) | static_cast<unsigned int>(magicBytes[3]);
    const bool sfntMagicOk = sfntMagic == 0x00010000u || // TrueType outlines
                             sfntMagic == 0x4F54544Fu || // 'OTTO' — OpenType / CFF
                             sfntMagic == 0x74727565u || // 'true' — legacy Apple TrueType
                             sfntMagic == 0x74797031u || // 'typ1' — PostScript Type 1
                             sfntMagic == 0x74746366u;   // 'ttcf' — TrueType Collection
    if (!sfntMagicOk) {
        LOG_WARN("Injected font blob has no valid sfnt magic (0x%08X); falling back to default font", sfntMagic);
        return false;
    }
    ImFontConfig mem_cfg = main_cfg;
    mem_cfg.FontDataOwnedByAtlas = false; // bytes live in g_InjectedFontBytes for the atlas lifetime
    ImFont* newFont =
        io.Fonts->AddFontFromMemoryTTF(g_InjectedFontBytes.data(), static_cast<int>(g_InjectedFontBytes.size()),
                                       g_InjectedFontSizePixels, &mem_cfg, glyph_ranges);
    if (!newFont) {
        LOG_WARN("Injected font bytes failed to load (%d bytes); falling back to default font",
                 static_cast<int>(g_InjectedFontBytes.size()));
        return false;
    }
    g_PreviewFonts.Regular = newFont;
    g_PreviewFonts.Bold = newFont;
    g_PreviewFonts.Italic = newFont;
    g_PreviewFonts.BoldItalic = newFont;
    g_PreviewFonts.Mono = newFont;
    g_PreviewFonts.Redaction = newFont; // injected path: no real block glyph; degrade to no-op
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_CurrentFontName = "Injected (host)";
    g_CurrentFontSize = g_InjectedFontSizePixels;
    io.FontDefault = newFont;
    return true;
}

} // namespace

void SmatchetSetInjectedFontBytes(const void* data, int dataSize, float sizePixels) {
    // CPP_CODE_AUDIT.md #28: stage into g_PendingInjectedFontBytes rather than mutating
    // g_InjectedFontBytes directly — see the comment on that declaration for why. The
    // pending buffer swaps into the live one inside SmatchetApplyImGuiFont, right after
    // io.Fonts->Clear() has already invalidated any atlas pointer into the old bytes.
    if (!data || dataSize <= 0 || sizePixels <= 0.0f) {
        g_PendingInjectedFontBytes.clear();
        g_PendingInjectedFontSizePixels = 0.0f;
        g_HasPendingInjectedFontBytes = true;
        return;
    }
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    g_PendingInjectedFontBytes.assign(bytes, bytes + dataSize);
    g_PendingInjectedFontSizePixels = sizePixels;
    g_HasPendingInjectedFontBytes = true;
}

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

    // Safe to swap in a pending re-injection now — Clear() just dropped every atlas
    // pointer into the old g_InjectedFontBytes buffer (CPP_CODE_AUDIT.md #28).
    if (g_HasPendingInjectedFontBytes) {
        g_InjectedFontBytes = std::move(g_PendingInjectedFontBytes);
        g_InjectedFontSizePixels = g_PendingInjectedFontSizePixels;
        g_PendingInjectedFontBytes.clear();
        g_HasPendingInjectedFontBytes = false;
    }

    ImFont* newFont = nullptr;

    // Host-injected font path (#12, mobile seam): an Android host has no readable system-font
    // path and injects raw TTF bytes through SmatchetSetInjectedFontBytes before this runs.
    // TryBuildInjectedFont consumes them on any platform and short-circuits the per-OS path
    // below. Returns false (no injection, or the blob failed to parse) → fall through to the
    // per-platform default rather than leave a null FontDefault.
    if (TryBuildInjectedFont(io, main_cfg, glyph_ranges.Data)) {
        return;
    }

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

        MergeFontAwesomeSolid(io, fontSizePixels);

        const MarkdownVariantFonts variantFonts =
            BuildMarkdownVariantFonts(io, fontName, fontSizePixels, glyph_ranges.Data);
        ImFont* boldFont = variantFonts.bold;
        ImFont* italicFont = variantFonts.italic;
        ImFont* boldItalicFont = variantFonts.boldItalic;
        ImFont* monoFont = variantFonts.mono;

        ImFont* redactFont = BuildRedactionFont(io, path, fontSizePixels, glyph_ranges.Data);

        g_PreviewFonts.Regular = newFont;
        g_PreviewFonts.Bold = boldFont ? boldFont : newFont;
        g_PreviewFonts.Italic = italicFont ? italicFont : newFont;
        g_PreviewFonts.BoldItalic = boldItalicFont ? boldItalicFont : (boldFont ? boldFont : newFont);
        g_PreviewFonts.Mono = monoFont ? monoFont : newFont;
        g_PreviewFonts.Redaction = redactFont ? redactFont : newFont;

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
    g_PreviewFonts.Redaction = newFont; // default-font path: no real block glyph; degrade to no-op
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_CurrentFontName = "Proggy (Clean/Default)";
    g_CurrentFontSize = fontSizePixels;
    io.FontDefault = newFont;
}

const SmatchetPreviewFonts& SmatchetGetPreviewFonts() { return g_PreviewFonts; }

namespace {
SmatchetPreviewFonts g_RedactFontBackup;
ImFont* g_FontDefaultBackup = nullptr;
bool g_RedactActive = false;
} // namespace

void SmatchetPushRedactionFonts() {
    if (g_RedactActive) {
        return;
    }
    ImFont* r = g_PreviewFonts.Redaction;
    if (r == nullptr) {
        return; // no redaction font — leave the frame normal
    }
    g_RedactFontBackup = g_PreviewFonts;
    ImGuiIO& io = ImGui::GetIO();
    g_FontDefaultBackup = io.FontDefault;
    io.FontDefault = r;
    // Repoint every preview font so default text AND the explicit PushFont sites
    // (markdown preview, selectable text) all render blocks for this frame.
    g_PreviewFonts.Regular = r;
    g_PreviewFonts.Bold = r;
    g_PreviewFonts.Italic = r;
    g_PreviewFonts.BoldItalic = r;
    g_PreviewFonts.Mono = r;
    g_RedactActive = true;
}

void SmatchetPopRedactionFonts() {
    if (!g_RedactActive) {
        return;
    }
    g_PreviewFonts = g_RedactFontBackup;
    ImGui::GetIO().FontDefault = g_FontDefaultBackup;
    g_RedactActive = false;
}

bool SmatchetAreFaIconsLoaded() { return g_FaIconsLoaded.load(std::memory_order_acquire); }

void SmatchetApplyImGuiDefaultFontWithExtendedGlyphs(ImGuiIO& io) { SmatchetApplyImGuiFont(io, "Segoe UI", 16.0f); }

void SmatchetRequestFontReload(const std::string& fontName, float fontSizePixels) {
    std::lock_guard<std::mutex> lock(g_FontReloadMutex);
    g_PendingFontName = fontName;
    g_PendingFontSize = fontSizePixels;
    g_FontReloadRequested.store(true);
}

bool SmatchetIsFontAvailable(const std::string& fontName) {
#if defined(_WIN32)
    const char* path = GetFontFilePath(fontName);
    return FileExistsUtf8(path);
#else
    (void)fontName;
    return false;
#endif
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
