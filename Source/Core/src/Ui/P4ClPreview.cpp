// Extracted from AnnotateAnalysisUi_Modals.cpp DrawClTooltipAsync (user-info-window
// Slice 1). Tooltip body + async fetch + detach/reap lifecycle are unchanged; the
// hover slot and describe cache moved off the Annotate pimpl singleton into this
// module so any window can preview a CL.

#include "Ui/P4ClPreview.h"

#include "Logger.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <chrono>
#include <exception>
#include <future>
#include <string>
#include <vector>

namespace P4ClPreview {

namespace {

struct ClPreviewState {
    P4ChangelistDescribeCache Cache{512};
    std::string HoverCl;
    std::shared_future<P4ChangelistDetails> HoverFut;
    std::vector<std::shared_future<P4ChangelistDetails>> DetachedHoverFuts;
    /// The describe answer for HoverCl, taken from HoverFut once (a worker exception becomes an
    /// Error here, logged once). Stays on screen while a retry for the same CL runs.
    P4ChangelistDetails HoverResult;
    bool HoverResolved = false;
    /// ImGui frame of the last tooltip draw: a gap means the cursor left the cell and came back.
    int LastDrawFrame = -1;
};

ClPreviewState& S() {
    static ClPreviewState s;
    return s;
}

ImVec4 ColFromRgba(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

P4ChangelistDetails FailedDetails(const std::string& error) {
    P4ChangelistDetails d;
    d.Loaded = true;
    d.Error = error;
    return d;
}

void LaunchHoverFetch(const std::string& cl, const AnnotateAnalysisConfig& cfg) {
    // Detach a still-pending previous fetch before overwriting: HoverFut comes from
    // std::async, so destroying the last reference to an unready state blocks until the
    // task finishes — a p4-describe-length stall on the UI thread (Pillar 2).
    if (S().HoverFut.valid() && S().HoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        S().DetachedHoverFuts.push_back(S().HoverFut);
    }
    S().HoverFut = std::shared_future<P4ChangelistDetails>();
    AnnotateAnalysisConfig cfgCopy = cfg;
    try {
        S().HoverFut =
            std::async(std::launch::async, [cfgCopy, cl]() { return S().Cache.GetOrFetch(cfgCopy, cl); }).share();
    } catch (const std::exception& ex) {
        // std::async throws std::system_error when no thread can start: show it instead of "Loading".
        LOG_WARN("P4ClPreview: could not start describe for CL %s: %s", cl.c_str(), ex.what());
        S().HoverResult = FailedDetails(std::string("Could not start p4 describe: ") + ex.what());
        S().HoverResolved = true;
    }
}

void TakeHoverResultIfReady() {
    if (!S().HoverFut.valid() || S().HoverFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const std::shared_future<P4ChangelistDetails> fut = S().HoverFut;
    S().HoverFut = std::shared_future<P4ChangelistDetails>();
    try {
        S().HoverResult = fut.get();
    } catch (const std::exception& ex) {
        LOG_WARN("P4ClPreview: describe for CL %s failed with an exception: %s", S().HoverCl.c_str(), ex.what());
        S().HoverResult = FailedDetails(std::string("p4 describe failed: ") + ex.what());
    } catch (...) {
        LOG_WARN("P4ClPreview: describe for CL %s failed with an unknown exception", S().HoverCl.c_str());
        S().HoverResult = FailedDetails("p4 describe failed: unknown error");
    }
    S().HoverResolved = true;
}

void DrawClDetails(const std::string& cl, const P4ChangelistDetails& d, const AnnotateUiThemeColors& theme) {
    if (!d.Error.empty()) {
        ImGui::TextUnformatted(d.Error.c_str());
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ColFromRgba(theme.ClTooltipTitle));
    ImGui::Text("CL %s", cl.c_str());
    ImGui::PopStyleColor();
    if (!d.Author.empty()) {
        ImGui::TextUnformatted(("by " + d.Author).c_str());
    }
    if (!d.Date.empty()) {
        ImGui::TextUnformatted(d.Date.c_str());
    }
    if (!d.Description.empty()) {
        ImGui::TextWrapped("%s", d.Description.c_str());
    }
    if (d.Author.empty() && d.Date.empty() && d.Description.empty()) {
        ImGui::TextUnformatted("(no describe details)");
    }
}

} // namespace

P4ChangelistDescribeCache& Cache() { return S().Cache; }

void DrawClTooltipAsync(const std::string& cl, const AnnotateAnalysisConfig& cfg, const AnnotateUiThemeColors& theme) {
    if (cl.empty()) {
        return;
    }
    const int frame = ImGui::GetFrameCount();
    const bool reHovered = S().LastDrawFrame < frame - 1;
    S().LastDrawFrame = frame;
    if (S().HoverCl != cl) {
        S().HoverCl = cl;
        S().HoverResult = P4ChangelistDetails();
        S().HoverResolved = false;
        LaunchHoverFetch(cl, cfg);
    } else if (reHovered && S().HoverResolved && !S().HoverResult.Error.empty() && !S().HoverFut.valid()) {
        // Back on a CL whose describe failed: ask again. The cache answers from memory inside its
        // retry window (and for a CL the server called unknown), so this re-runs p4 only when due.
        LaunchHoverFetch(cl, cfg);
    }
    TakeHoverResultIfReady();
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Left-click this changelist cell to open it in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (S().HoverResolved) {
        DrawClDetails(cl, S().HoverResult, theme);
    } else {
        ImGui::TextUnformatted("Loading CL info...");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DetachInFlight() {
    S().HoverCl.clear();
    if (S().HoverFut.valid() && S().HoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        S().DetachedHoverFuts.push_back(S().HoverFut);
    }
    S().HoverFut = std::shared_future<P4ChangelistDetails>();
}

void ReapDetached() {
    std::vector<std::shared_future<P4ChangelistDetails>>& futs = S().DetachedHoverFuts;
    for (size_t i = 0; i < futs.size();) {
        if (!futs[i].valid() || futs[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            futs.erase(futs.begin() +
                       static_cast<std::vector<std::shared_future<P4ChangelistDetails>>::difference_type>(i));
        } else {
            ++i;
        }
    }
}

void DrainForShutdown(std::chrono::milliseconds timeout) {
    // CPP_CODE_AUDIT.md #30: give the in-flight fetch (if any) up to `timeout` to finish
    // on its own, logging either outcome, instead of silently hanging inside the
    // ClPreviewState Meyers singleton's destructor at static-destruction time with zero
    // diagnostic. See the header doc comment for why the default timeout value makes
    // this a real bound (not just a shorter wait before the same eventual block).
    if (S().HoverFut.valid() && S().HoverFut.wait_for(timeout) != std::future_status::ready) {
        LOG_WARN("P4ClPreview: shutdown drain timed out (%lld ms) waiting on hover CL '%s' describe fetch",
                 static_cast<long long>(timeout.count()), S().HoverCl.c_str());
    }
    for (const std::shared_future<P4ChangelistDetails>& fut : S().DetachedHoverFuts) {
        if (fut.valid() && fut.wait_for(timeout) != std::future_status::ready) {
            LOG_WARN("P4ClPreview: shutdown drain timed out (%lld ms) waiting on a detached describe fetch",
                     static_cast<long long>(timeout.count()));
        }
    }
}

} // namespace P4ClPreview
