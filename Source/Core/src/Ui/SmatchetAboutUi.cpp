// SmatchetAboutUi — the "About Smatchet" modal. Renders the diagnostics::AboutInfo
// snapshot (version / build / git / runtime / third-party) and offers the two
// actions a user actually needs from an About box: open the repo and copy the
// whole thing for a bug thread. Deliberately no "Check for Updates" — Help
// already owns that item, and a second entry point is pure duplication.
// This TU is render-only: every fact comes from AboutInfo.cpp, which is the sole
// includer of the generated SmatchetBuildInfo.h. Nothing here touches CMake
// codegen, and nothing here re-derives a value.
// docs/plans/shipped/about-dialog-help-menu.md Slice 4.

#include "SmatchetAboutUi.h"

// The three things this modal needs live on AppController and nowhere narrower:
// OpenUrl (which carries the scheme allowlist and the host-callback indirection
// that keep a link safe under Unreal), GetAppVersion, and GetGitHubReleaseRepo.
// DrawAboutModal takes an AppController reference by signature, exactly as every
// sibling modal TU already does, so there is no lighter dependency to swap in.
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=calls OpenUrl/GetAppVersion; owner=alexk; revisit=2026-12-31)
#include "AppController.h"
#include "Diagnostics/AboutInfo.h"
#include "IconsFontAwesome6.h"
#include "Privacy/TextRedaction.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetAboutIcon.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization wrapper.
#define ImGui SmatchetLocalizedImGui

#include <cfloat>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using smatchet::diagnostics::AboutDep;
using smatchet::diagnostics::AboutInfo;

const char* const kAboutPopupId = "About Smatchet";

/// Per-frame state shared by the section helpers. Mirrors the MainMenuDrawCtx
/// pattern (docs/guides/imgui-draw-pattern.md) so each helper stays short and
/// none of them re-fetches the snapshot.
struct AboutDrawCtx {
    AppController& app;
    UiDrawSession& d;
    const AboutInfo& info;
};

/// Renders `value` as a frameless read-only InputText rather than static text, so
/// the user can drag-select part of a SHA or a path and Ctrl+C it — ImGui gates
/// cut/paste/undo on !ReadOnly but never copy.
/// The per-frame buffer copy is deliberate and safe: ImGui's read-only path reads
/// straight from the caller's buffer, never writes to it, and only dereferences it
/// for the duration of the call ("For _ReadOnly fields, pointer will be null
/// outside the InputText() call", imgui_internal.h). So there is no cross-frame
/// lifetime to manage and no const_cast onto the std::string's own storage.
/// `width` 0 fills the remaining space; a positive value fits the field to its
/// text so a SameLine() can follow. `dim` renders in the disabled text colour,
/// matching the TextDisabled() call this replaced.
void SelectableValue(const char* id, const std::string& value, float width = 0.0f, bool dim = false) {
    const char* text = value.empty() ? "unknown" : value.c_str();
    const std::size_t len = std::strlen(text);
    std::vector<char> buf(text, text + len + 1);

    // Only FrameBg is cleared — FrameBgHovered/Active stay, and that hover
    // highlight is the affordance that tells the user the value is selectable.
    ::ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    if (dim) {
        ::ImGui::PushStyleColor(ImGuiCol_Text, ::ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }
    // Zero padding so the field is exactly one text line tall and every row keeps
    // the height it had while these were Text() calls.
    ::ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ::ImGui::SetNextItemWidth(width > 0.0f ? width : -FLT_MIN);
    ::ImGui::InputText(id, buf.data(), buf.size(), ImGuiInputTextFlags_ReadOnly);
    ::ImGui::PopStyleVar();
    ::ImGui::PopStyleColor(dim ? 2 : 1);

    // The field clips and scrolls instead of wrapping, so anything wider than its
    // column needs the whole value on hover.
    if (::ImGui::CalcTextSize(text).x > ::ImGui::GetItemRectSize().x) {
        ::ImGui::SetItemTooltip("%s", text);
    }
}

/// Right-click menu for the item drawn immediately before the call. Left-click is
/// deliberately NOT a link activation: the values are text-selectable now, and a
/// stray click while selecting must never launch a browser.
void LinkContextMenu(const AboutDrawCtx& ctx, const char* popupId, const std::string& url) {
    if (url.empty()) {
        return;
    }
    if (!::ImGui::BeginPopupContextItem(popupId)) {
        return;
    }
    if (ImGui::MenuItem("Open in Browser")) {
        // Same allowlisted, host-indirected path the "Open on GitHub" button uses.
        ctx.app.OpenUrl(url);
    }
    if (ImGui::MenuItem("Copy Link")) {
        ::ImGui::SetClipboardText(url.c_str());
        SmatchetToastManager::Instance().Push("About", "Link copied to clipboard.", ToastType::Success, 2500);
    }
    ::ImGui::EndPopup();
}

/// Two-column key/value table. Keys are localized; values NEVER are — a SHA, a
/// compiler version or a filesystem path must stay copy-pasteable verbatim.
bool BeginFactTable(const char* id) {
    if (!::ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        return false;
    }
    ::ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ::ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// One key/value row. The value is a SelectableValue, which also subsumes what the
/// old FactRowWrapped() existed for: a long path or SHA is bounded by its column
/// and scrolls inside the field instead of wrapping the modal wider, and the full
/// text is one hover away.
void FactRow(const char* label, const std::string& value) {
    ::ImGui::TableNextRow();
    ::ImGui::TableSetColumnIndex(0);
    // "%s" as the format string, never `label` itself — a non-literal format
    // string is both a -Wformat-security warning and a varargs hazard.
    ::ImGui::TextDisabled("%s", SmatchetLocalization::TranslateSource(label));
    ::ImGui::TableSetColumnIndex(1);
    // The label is unique within its table, so it is the row's ID scope; the
    // field itself only needs a hidden ("##") label.
    ::ImGui::PushID(label);
    SelectableValue("##value", value);
    ::ImGui::PopID();
}

const float kAboutIconSize = 48.0f;

// Rocket-launch easter egg (Ctrl+Shift+click the icon). Three phases: an accelerating climb out
// of the modal, a beat spent off-frame, then a free-fall back into the slot.
const double kRocketAscentSecs = 0.55;
const double kRocketAwaySecs = 0.25;
const double kRocketReturnSecs = 0.70;
const double kRocketTotalSecs = kRocketAscentSecs + kRocketAwaySecs + kRocketReturnSecs;
/// Apex offset. Only the first ~200 px are ever visible — the window clip rect swallows the rest —
/// but a large value is what makes the exit read as "gone", not "parked above the title bar".
const float kRocketApexPx = 900.0f;

/// Vertical offset above the resting slot at `elapsed` seconds into the launch.
/// @return false during the coast phase, where the icon is off-frame and must not be drawn at all.
bool RocketOffsetAt(double elapsed, float& outDy) {
    if (elapsed < kRocketAscentSecs) {
        const float u = static_cast<float>(elapsed / kRocketAscentSecs);
        outDy = kRocketApexPx * u * u; // Quadratic: slow off the pad, fast at the top.
        return true;
    }
    if (elapsed < kRocketAscentSecs + kRocketAwaySecs) {
        return false;
    }
    const float u = static_cast<float>((elapsed - kRocketAscentSecs - kRocketAwaySecs) / kRocketReturnSecs);
    outDy = kRocketApexPx * (1.0f - u * u); // Mirrored: drifts in, then drops the last stretch fast.
    return true;
}

/// Exhaust plume under the icon, fading with distance from the nozzle and fading in with speed
/// (dy stands in for velocity — it is small on the pad and at touchdown, large mid-flight).
void DrawRocketTrail(ImDrawList* dl, const ImVec2& iconPos, float dy) {
    float intensity = dy / 240.0f;
    intensity = intensity > 1.0f ? 1.0f : intensity;
    const char* flame = SmatchetAreFaIconsLoaded() ? ICON_FA_FIRE : "^";
    for (int i = 1; i <= 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        const ImVec2 p(iconPos.x + kAboutIconSize * 0.34f, iconPos.y + kAboutIconSize * (0.75f + t * 0.9f));
        const ImU32 col =
            IM_COL32(255, static_cast<int>(190.0f - 120.0f * t), 40, static_cast<int>(220.0f * intensity * (1.0f - t)));
        dl->AddText(::ImGui::GetFont(), kAboutIconSize * (0.55f - 0.09f * t), p, col, flame);
    }
}

/// The app icon, drawn from the baked-in .ico via the texture cache, with a Font Awesome glyph
/// fallback for renderers with no texture support (and for a build with no icon baked in).
void DrawAboutIcon(const AboutDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;

    // The hit target is the resting slot and never moves: the layout below cannot shift mid-flight,
    // and a re-launch stays clickable while the icon is still in the air.
    const ImVec2 slot = ::ImGui::GetCursorScreenPos();
    ::ImGui::InvisibleButton("##about.icon", ImVec2(kAboutIconSize, kAboutIconSize));
    const ImGuiIO& io = ::ImGui::GetIO();
    if (::ImGui::IsItemClicked(ImGuiMouseButton_Left) && io.KeyCtrl && io.KeyShift) {
        d.aboutRocketStart = ::ImGui::GetTime();
    }

    float dy = 0.0f;
    if (d.aboutRocketStart >= 0.0) {
        const double elapsed = ::ImGui::GetTime() - d.aboutRocketStart;
        if (elapsed >= kRocketTotalSecs) {
            d.aboutRocketStart = -1.0;
        } else if (!RocketOffsetAt(elapsed, dy)) {
            return; // Off-frame this beat.
        }
    }

    const ImVec2 p(slot.x, slot.y - dy);
    ImDrawList* dl = ::ImGui::GetWindowDrawList();
    if (dy > 0.0f) {
        DrawRocketTrail(dl, p, dy);
    }
    SmatchetLoadedIconTexture tex;
    if (smatchet::ui::TryGetAboutIconTexture(tex)) {
        dl->AddImage(tex.Texture->GetTexRef(), p, ImVec2(p.x + kAboutIconSize, p.y + kAboutIconSize));
        return;
    }
    const char* glyph = SmatchetAreFaIconsLoaded() ? ICON_FA_CUBE : "[S]";
    dl->AddText(::ImGui::GetFont(), kAboutIconSize * 0.8f, p, ::ImGui::GetColorU32(ImGuiCol_Text), glyph);
}

void DrawAboutIdentity(const AboutDrawCtx& ctx) {
    DrawAboutIcon(ctx);
    ::ImGui::SameLine();
    ::ImGui::BeginGroup();
    ImGui::Text("%s", ctx.info.AppName.c_str());
    ImGui::SameLine();
    // Width-fitted so the version keeps sitting beside the name; it is the single
    // most-copied value in the dialog, so it is selectable like the rest.
    SelectableValue("##about.version", ctx.info.Version, ::ImGui::CalcTextSize(ctx.info.Version.c_str()).x + 4.0f,
                    true);
    if (!ctx.info.RepoUrl.empty()) {
        SelectableValue("##about.repo", ctx.info.RepoUrl, 0.0f, true);
        LinkContextMenu(ctx, "##about.repo.menu", ctx.info.RepoUrl);
    }
    ::ImGui::EndGroup();
}

void DrawAboutBuild(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Build");
    if (!BeginFactTable("AboutBuildFacts")) {
        return;
    }
    const smatchet::diagnostics::AboutBuildInfo& b = ctx.info.Build;
    FactRow("configuration", b.Config);
    FactRow("target", b.BuildTag);
    FactRow("built", b.DateTime);
    FactRow("compiler", b.CompilerId + " " + b.CompilerVersion);
    FactRow("cmake", b.CMakeVersion);
    FactRow("imgui", b.ImGuiVersion);
    FactRow("architecture", b.TargetArch);
    ::ImGui::EndTable();
}

void DrawAboutGit(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Source");
    if (!BeginFactTable("AboutGitFacts")) {
        return;
    }
    FactRow("commit", smatchet::diagnostics::FormatAboutGitLine(ctx.info.Git));
    // Full SHA on its own row: the one-line form above is abbreviated, and a bug
    // thread wants the unambiguous 40-char value.
    if (!ctx.info.Git.Sha.empty() && ctx.info.Git.Sha != ctx.info.Git.ShaShort) {
        FactRow("full sha", ctx.info.Git.Sha);
    }
    ::ImGui::EndTable();
}

void DrawAboutRuntime(const AboutDrawCtx& ctx) {
    ImGui::SeparatorText("Runtime");
    if (!BeginFactTable("AboutRuntimeFacts")) {
        return;
    }
    const smatchet::diagnostics::AboutRuntimeInfo& r = ctx.info.Runtime;
    FactRow("os", r.Os);
    if (r.EmulationResolved && !r.NativeArch.empty()) {
        // Omitted rather than guessed when the probe never resolved (pre-1709
        // Windows, non-Windows) — matches BuildAboutReportText.
        FactRow("host cpu", r.NativeArch + (r.Emulated ? " (emulated)" : ""));
    }
    FactRow("tracker", r.Tracker);
    FactRow("data dir", r.UserDataDir);
    FactRow("config", r.ConfigPath);
    ::ImGui::EndTable();
}

void DrawAboutCredits(const AboutDrawCtx& ctx) {
    if (ctx.info.Deps.empty()) {
        return;
    }
    ImGui::SeparatorText("Third-party");
    // Fixed height + border: the list is generated from the CMake dependency
    // manifest, so its length changes with the build's feature flags.
    ::ImGui::BeginChild("AboutCredits", ImVec2(0.0f, 132.0f), ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < ctx.info.Deps.size(); ++i) {
        const AboutDep& dep = ctx.info.Deps[i];
        ::ImGui::PushID(static_cast<int>(i));
        // Name and version as one selectable run: that is what a user pastes when
        // reporting "which build of X am I on".
        const std::string nameAndVersion = dep.Name + " " + dep.Version;
        SelectableValue("##dep", nameAndVersion, ::ImGui::CalcTextSize(nameAndVersion.c_str()).x + 4.0f);
        // dep.Url comes straight from the CMake dependency manifest and had no
        // surface in the UI until now.
        LinkContextMenu(ctx, "##dep.menu", dep.Url);
        if (!dep.License.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", dep.License.c_str());
        }
        ::ImGui::PopID();
    }
    ::ImGui::EndChild();
}

/// Toast text for the Nth "Copy to Clipboard" press of the current open. The clipboard payload is
/// identical every time, so the message is what escalates. Raw literals on purpose: toast strings do
/// not route through the localization shim, so this adds no translation rows.
const char* CopyToastText(int presses) {
    if (presses <= 1) {
        return "Build info copied to clipboard.";
    }
    if (presses == 2) {
        return "Copied again.";
    }
    if (presses == 3) {
        return "Still the same build.";
    }
    if (presses == 4) {
        return "Yes, still the same build.";
    }
    return "I admire the persistence.";
}

void DrawAboutActions(const AboutDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;

    ::ImGui::BeginDisabled(ctx.info.RepoUrl.empty());
    if (ImGui::Button("Open on GitHub")) {
        // OpenUrl carries the scheme allowlist + the host-callback indirection
        // that keeps this safe under Unreal.
        ctx.app.OpenUrl(ctx.info.RepoUrl);
    }
    ::ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Copy to Clipboard")) {
        // Same egress scrubber the bug-report path uses. It matches by shape
        // (emails, tokens, URL userinfo) — not usernames — so it will not touch
        // the filesystem paths; it is here so a tracker URL or address that ever
        // reaches this report cannot be pasted into a public thread. Git SHAs are
        // deliberately preserved by RedactLogLine.
        const std::string report =
            smatchet::privacy::RedactLogText(smatchet::diagnostics::BuildAboutReportText(ctx.info));
        ::ImGui::SetClipboardText(report.c_str());
        ++d.aboutCopyPresses;
        SmatchetToastManager::Instance().Push("About", CopyToastText(d.aboutCopyPresses), ToastType::Success, 2500);
    }

    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        d.showAbout = false;
        ::ImGui::CloseCurrentPopup();
    }
}

/// Drop everything that only makes sense within one open: the cached snapshot (so the next open
/// re-reads config), the copy-press counter, and any rocket still in the air.
void ResetAboutTransient(UiDrawSession& d) {
    d.aboutInfo.reset();
    d.aboutCopyPresses = 0;
    d.aboutRocketStart = -1.0;
}

} // namespace

void DrawAboutModal(AppController& app, UiDrawSession& d) {
    if (!d.showAbout) {
        ResetAboutTransient(d);
        return;
    }

    if (d.aboutOpenLatch || !d.aboutInfo) {
        d.aboutOpenLatch = false;
        // GatherAboutInfo reads the disk-backed config — snapshot once per open
        // and cache it, never per frame (Pillar 2).
        d.aboutInfo = std::make_shared<AboutInfo>(
            smatchet::diagnostics::GatherAboutInfo(app.GetAppVersion(), app.GetGitHubReleaseRepo()));
        ImGui::OpenPopup(kAboutPopupId);
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(kAboutPopupId, &d.showAbout, ImGuiWindowFlags_NoSavedSettings)) {
        // Two different states return false here: the popup is gone (dismissed
        // via Escape or the title-bar X), or it is still open but a popup at a
        // deeper stack level owns this frame. Only the first is a dismissal —
        // clearing state on the second silently drops the user's request.
        if (!::ImGui::IsPopupOpen(kAboutPopupId, ImGuiPopupFlags_AnyPopupLevel)) {
            // Drop the flag and the snapshot so the next open re-reads config,
            // and so the menu item is not re-opening a popup ImGui already closed.
            d.showAbout = false;
            ResetAboutTransient(d);
        }
        return;
    }

    const AboutDrawCtx ctx{app, d, *d.aboutInfo};
    DrawAboutIdentity(ctx);
    DrawAboutBuild(ctx);
    DrawAboutGit(ctx);
    DrawAboutRuntime(ctx);
    DrawAboutCredits(ctx);
    // Both affordances added below are invisible until used — a one-line hint is
    // the whole discoverability story for them.
    ImGui::TextDisabled("Select any value to copy it. Right-click a link for actions.");
    ImGui::Separator();
    DrawAboutActions(ctx);

    ::ImGui::EndPopup();
}

#undef ImGui
