// SmatchetWhisperSetupBanner — see header for design pointers.
//
// Renders an ImGui window pinned to the top of the main viewport. Three
// collapsed-state heights:
//   - 52 px  three-button row,
//   - 120 px model-size picker expanded,
//   - 70 px  active download (progress bar + cancel).
//
// All strings flow through SmatchetLocalization::T so future locale work
// only touches SmatchetLocalization.cpp. The TU is added to CORE_SOURCES
// only when SMATCHET_WITH_WHISPER=ON (see root CMakeLists.txt) so call sites
// can probe `cfg.WhisperSetupCompleted` without a per-call ifdef in
// SmatchetUI.cpp.

#include "SmatchetWhisperSetupBanner.h"

class AppController; // fan-in Phase 6: this TU only names AppController& in signatures / pass-throughs — fwd-decl
                     // suffices
#include "ConfigManager.h"
#include "Logger.h"
#include "ModelCatalog.h"
#include "ModelDownloader.h"
#include "SmatchetLocalization.h"
#include "WhisperConsentGate.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace smatchet {
namespace whisper {
namespace banner {

namespace {

// Per-banner UI state — persists across frames while the banner is open.
struct BannerState {
    bool pickerExpanded = false; // user has clicked "Enable ▾"
    // Set when user clicks "Decide later" — suppresses the banner for the
    // rest of this process so the user isn't blocked while they think
    // about it. WhisperSetupCompleted stays false so the banner returns
    // on next launch (the documented "decide later" contract). Cleared
    // when Preferences "Re-run setup banner" toggles the cfg flag.
    bool dismissedThisSession = false;
    int selectedIndex = 1; // default to "Recommended" (ggml-base.en, index 1)
    std::string lastError; // last download error surfaced to the banner
    // True once the banner has observed an active download transition this
    // session (Downloading or Verifying). Auto-completion is gated on this
    // so a leftover State::Complete in the static downloader from a prior
    // session does not flicker the banner into auto-dismiss on its first
    // frame after "Re-run setup banner" flips WhisperSetupCompleted back
    // to false.
    bool sawActiveDownloadThisSession = false;
    // Sticky observation of cfg.WhisperSetupCompleted — flipping it from
    // true -> false (the Preferences "Re-run setup banner" path) resets the
    // session-scoped flags so a fresh banner cycle starts clean.
    bool lastCfgSetupCompleted = false;
};

BannerState& Bs() {
    static BannerState s;
    return s;
}

ModelDownloader& OwnedDownloader() {
    static ModelDownloader d;
    return d;
}

std::string FormatSizeMb(std::uint64_t bytes) {
    if (bytes == 0) {
        return std::string("?");
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    return std::string(buf);
}

// Map ModelCatalog index → cfg.WhisperModel id + remember the user's pick
// so Preferences shows it consistently. Returns "ggml-base.en" on out-of-
// range index.
std::string CatalogIdAt(int idx) {
    const auto& all = smatchet::whisper::catalog::All();
    if (idx < 0 || static_cast<std::size_t>(idx) >= all.size()) {
        return std::string("ggml-base.en");
    }
    return all[static_cast<std::size_t>(idx)].Id;
}

const char* TranslatedDisplayName(const std::string& id, const char* fallback) {
    // English fallback used both when locale lookup misses and as the
    // canonical user-visible string for the catalog table.
    if (id == "ggml-tiny.en") {
        return SmatchetLocalization::T("whisper.modelPicker.smaller", "Smaller, faster (40 MB)");
    }
    if (id == "ggml-base.en") {
        return SmatchetLocalization::T("whisper.modelPicker.recommended", "Recommended (150 MB)");
    }
    if (id == "ggml-small.en") {
        return SmatchetLocalization::T("whisper.modelPicker.higher", "Higher accuracy (500 MB)");
    }
    return fallback;
}

void StampFreshConsent(TrackerConfig& cfg) {
#if defined(SMATCHET_WITH_WHISPER)
    cfg.WhisperConsentTimestampSec = smatchet::whisper::consent::NowEpochSec();
#else
    (void)cfg;
#endif
}

bool DispatchDownload(AppController& app, TrackerConfig& cfg, std::string& outError) {
    const std::string modelId = CatalogIdAt(Bs().selectedIndex);
    cfg.WhisperModel = modelId;
    cfg.WhisperEnabled = true; // banner intent — Preferences can flip later
    // SetupCompleted only flips on successful download (the next-launch
    // banner-suppress contract). Persisting Enabled+Model early lets the
    // CLI commands see them; the banner stays visible until Complete.
    StampFreshConsent(cfg);
    ConfigManager::Save(cfg);

    const std::string sharedDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    if (sharedDir.empty()) {
        outError = "platform shared user-data directory unavailable";
        return false;
    }
    std::string destDir = sharedDir;
    if (!destDir.empty() && destDir.back() != '/' && destDir.back() != '\\') {
        destDir.push_back('/');
    }
    destDir += "whisper";

    return OwnedDownloader().Start(app, modelId, destDir, outError);
}

// Banner height by phase, scaled by the user's font zoom (clamped to >=1.0).
float ComputeBannerHeight(bool downloading, bool pickerExpanded, float uiScale) {
    float bannerHeight = 64.0f * uiScale;
    if (downloading) {
        bannerHeight = 84.0f * uiScale;
    } else if (pickerExpanded) {
        bannerHeight = 140.0f * uiScale;
    }
    return bannerHeight;
}

// Push the banner's accent-blue background, amber border, white text, and padding style.
// Pushes 4 colors + 2 style vars — the caller pops the same counts after ImGui::End.
void PushBannerStyles(float uiScale) {
    // Distinct accent-blue background + a visible bottom border so the banner
    // reads as a notification strip rather than blending into the chrome.
    // The colors are picked to remain legible on both light and dark ImGui
    // themes (high-saturation blue background, white text, contrasting border).
    // TextDisabled is overridden too so the (size) labels in the model picker
    // stay readable against the saturated background (the theme default goes
    // low-contrast against blue).
    const ImVec4 bgColor(0.10f, 0.42f, 0.78f, 1.00f);     // saturated blue
    const ImVec4 borderColor(0.95f, 0.78f, 0.20f, 1.00f); // amber attention bar
    const ImVec4 textColor(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 textDisabledColor(0.84f, 0.90f, 1.00f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bgColor);
    ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, textDisabledColor);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * uiScale, 8.0f * uiScale));
}

// Active-download phase: progress text + bar + cancel button.
void RenderDownloadingBody(const ModelDownloader::Progress& prog) {
    const float frac =
        (prog.bytesExpected > 0)
            ? std::min(1.0f, static_cast<float>(prog.bytesReceived) / static_cast<float>(prog.bytesExpected))
            : 0.0f;
    const char* label = (prog.state == ModelDownloader::State::Verifying)
                            ? SmatchetLocalization::T("whisper.banner.verifying", "Verifying download...")
                            : SmatchetLocalization::T("whisper.banner.downloading", "Downloading speech model...");
    ::ImGui::Text("%s %s (%.0f%%)", label, prog.modelId.c_str(), frac * 100.0f);
    ::ImGui::ProgressBar(frac, ImVec2(-1, 0));
    if (::ImGui::Button(SmatchetLocalization::T("whisper.modelPicker.cancel", "Cancel"))) {
        OwnedDownloader().Cancel();
    }
}

// Expanded model-picker phase: radio list + download/cancel + last-error line.
void RenderPickerBody(AppController& app, TrackerConfig& cfg, BannerState& s, bool& cfgChanged) {
    ::ImGui::TextUnformatted(SmatchetLocalization::T("whisper.banner.pickModel", "Choose speech model:"));
    const auto& all = smatchet::whisper::catalog::All();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string id = all[i].Id;
        // Phase F — ggml-medium.en is a power-user / Preferences-only model.
        // The first-run banner intentionally only surfaces tiny / base / small
        // to keep the guided pick fast and the disk-cost surprise small.
        if (id == "ggml-medium.en") {
            continue;
        }
        int& sel = s.selectedIndex;
        const bool selected = (static_cast<int>(i) == sel);
        if (::ImGui::RadioButton(TranslatedDisplayName(id, all[i].DisplayName.c_str()), selected)) {
            sel = static_cast<int>(i);
        }
        ::ImGui::SameLine();
        ::ImGui::TextDisabled("(%s)", FormatSizeMb(all[i].SizeBytes).c_str());
    }
    if (::ImGui::Button(SmatchetLocalization::T("whisper.modelPicker.download", "Download + enable"))) {
        std::string err;
        if (!DispatchDownload(app, cfg, err)) {
            s.lastError = err;
            LOG_WARN("Whisper banner: download dispatch failed: %s", err.c_str());
        } else {
            s.lastError.clear();
        }
        cfgChanged = true; // DispatchDownload writes cfg
    }
    ::ImGui::SameLine();
    if (::ImGui::Button(SmatchetLocalization::T("whisper.modelPicker.cancel", "Cancel"))) {
        s.pickerExpanded = false;
    }
    if (!s.lastError.empty()) {
        ::ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", s.lastError.c_str());
    }
}

// Initial prompt phase: copy + Enable / Decide later / No thanks buttons.
void RenderPromptBody(TrackerConfig& cfg, BannerState& s, bool& cfgChanged) {
    // TextWrapped lets the long copy fit on narrow viewports without being
    // truncated to a single line. The leading dot-prefix substitutes for a
    // missing icon glyph in the default ImGui font; the banner color +
    // border already do most of the attention-grabbing work.
    ::ImGui::TextWrapped(
        "* %s", SmatchetLocalization::T("whisper.banner.title",
                                        "Enable voice dictation? Push-to-talk transcribes into any text field. "
                                        "Optional, off by default. No audio leaves your machine when the local "
                                        "model is used."));
    if (::ImGui::Button(SmatchetLocalization::T("whisper.banner.enable", "Enable"))) {
        s.pickerExpanded = true;
    }
    ::ImGui::SameLine();
    if (::ImGui::Button(SmatchetLocalization::T("whisper.banner.later", "Decide later"))) {
        // Session-only dismiss. WhisperSetupCompleted stays false so the
        // banner returns next launch; the dismissedThisSession flag
        // hides it for the rest of the current process so the user can
        // get on with their work.
        s.dismissedThisSession = true;
        LOG_INFO("Whisper banner: user picked 'Decide later' — hidden until next launch");
    }
    ::ImGui::SameLine();
    if (::ImGui::Button(SmatchetLocalization::T("whisper.banner.disable", "No thanks"))) {
        cfg.WhisperEnabled = false;
        cfg.WhisperSetupCompleted = true;
        cfg.WhisperSetupChoice = "disabled";
        cfgChanged = true;
        LOG_INFO("Whisper banner: user declined.");
    }
}

} // namespace

ModelDownloader& BannerOwnedDownloader() { return OwnedDownloader(); }

bool Render(AppController& app, TrackerConfig& cfg) {
#if !defined(SMATCHET_WITH_WHISPER)
    (void)app;
    (void)cfg;
    return false;
#else
    BannerState& s = Bs();

    // Detect "Preferences re-run setup banner just flipped Completed false".
    // Reset session-scoped flags so a leftover ModelDownloader::State::Complete
    // from a prior successful download in the same process can't auto-dismiss
    // the new banner on its first frame (the flicker bug).
    if (s.lastCfgSetupCompleted && !cfg.WhisperSetupCompleted) {
        s.sawActiveDownloadThisSession = false;
        s.pickerExpanded = false;
        s.lastError.clear();
        // Re-run banner also clears the session dismiss so the user sees
        // the banner again in the same process.
        s.dismissedThisSession = false;
    }
    s.lastCfgSetupCompleted = cfg.WhisperSetupCompleted;

    if (cfg.WhisperSetupCompleted || s.dismissedThisSession) {
        return false;
    }

    bool cfgChanged = false;

    // Pin to the top of the main viewport work area. Work-area excludes the
    // main menu bar so SetNextWindowPos lands the banner directly under it.
    ImGuiViewport* vp = ::ImGui::GetMainViewport();
    if (vp == nullptr) {
        return false;
    }
    const ImVec2 workPos = vp->WorkPos;
    const ImVec2 workSize = vp->WorkSize;

    // Compute banner height by phase. The taller heights below give the text
    // + buttons room to breathe at the larger background contrast we use now —
    // a too-tight banner looked indistinguishable from the menu bar.
    const ModelDownloader::Progress prog = OwnedDownloader().GetProgress();
    const bool downloading =
        (prog.state == ModelDownloader::State::Downloading) || (prog.state == ModelDownloader::State::Verifying);
    if (downloading) {
        s.sawActiveDownloadThisSession = true;
    }
    // Scale all the hardcoded pixel sizes by the user's font zoom so the
    // banner doesn't clip copy/buttons at higher zoom levels (per the
    // project's font-zoom guideline). Clamped to >=1.0 so a user with
    // FontGlobalScale<1.0 doesn't get an unreadable squashed banner.
    const float uiScale = (std::max)(1.0f, ::ImGui::GetIO().FontGlobalScale);
    const float bannerHeight = ComputeBannerHeight(downloading, s.pickerExpanded, uiScale);

    ::ImGui::SetNextWindowPos(workPos);
    ::ImGui::SetNextWindowSize(ImVec2(workSize.x, bannerHeight));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;

    PushBannerStyles(uiScale);
    if (!::ImGui::Begin("##WhisperSetupBanner", nullptr, flags)) {
        ::ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        return false;
    }

    if (downloading) {
        RenderDownloadingBody(prog);
    } else if (s.pickerExpanded) {
        RenderPickerBody(app, cfg, s, cfgChanged);
    } else {
        RenderPromptBody(cfg, s, cfgChanged);
    }

    // Banner auto-dismisses when a download completes; flip the setup flag
    // and the next frame stops drawing. Gated on `sawActiveDownloadThisSession`
    // so a leftover State::Complete from a previous successful download in
    // the same process cannot auto-dismiss a re-opened banner (the "flickers
    // in and out" bug after Preferences -> Re-run setup banner).
    if (s.sawActiveDownloadThisSession && prog.state == ModelDownloader::State::Complete) {
        cfg.WhisperEnabled = true;
        cfg.WhisperSetupCompleted = true;
        cfg.WhisperSetupChoice = "enabled";
        cfg.WhisperModel = prog.modelId.empty() ? cfg.WhisperModel : prog.modelId;
        cfgChanged = true;
        s.pickerExpanded = false;
        s.sawActiveDownloadThisSession = false;
        LOG_INFO("Whisper banner: setup completed (model=%s).", cfg.WhisperModel.c_str());
    }

    ::ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return cfgChanged;
#endif
}

} // namespace banner
} // namespace whisper
} // namespace smatchet
