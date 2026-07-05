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
#include <future>
#include <vector>

namespace P4ClPreview {

namespace {

struct ClPreviewState {
    P4ChangelistDescribeCache Cache{512};
    std::string HoverCl;
    std::shared_future<P4ChangelistDetails> HoverFut;
    std::vector<std::shared_future<P4ChangelistDetails>> DetachedHoverFuts;
};

ClPreviewState& S() {
    static ClPreviewState s;
    return s;
}

ImVec4 ColFromRgba(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

} // namespace

P4ChangelistDescribeCache& Cache() { return S().Cache; }

void DrawClTooltipAsync(const std::string& cl, const AnnotateAnalysisConfig& cfg, const AnnotateUiThemeColors& theme) {
    if (cl.empty()) {
        return;
    }
    if (S().HoverCl != cl) {
        // Detach a still-pending previous fetch before overwriting: HoverFut comes from
        // std::async, so destroying the last reference to an unready state blocks until the
        // task finishes — a p4-describe-length stall on the UI thread (Pillar 2).
        if (S().HoverFut.valid() && S().HoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            S().DetachedHoverFuts.push_back(S().HoverFut);
        }
        S().HoverCl = cl;
        AnnotateAnalysisConfig cfgCopy = cfg;
        S().HoverFut =
            std::async(std::launch::async, [cfgCopy, cl]() { return S().Cache.GetOrFetch(cfgCopy, cl); }).share();
    }
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Left-click this changelist cell to open it in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (!S().HoverFut.valid()) {
        ImGui::TextUnformatted("Loading CL info...");
    } else if (S().HoverFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ImGui::TextUnformatted("Loading CL info...");
    } else {
        try {
            const P4ChangelistDetails d = S().HoverFut.get();
            if (!d.Error.empty()) {
                ImGui::TextUnformatted(d.Error.c_str());
            } else {
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
        } catch (const std::exception& ex) {
            LOG_WARN("P4ClPreview: changelist detail future exception: %s", ex.what());
            ImGui::TextUnformatted("Loading CL info...");
        } catch (...) {
            LOG_WARN("P4ClPreview: changelist detail future unknown exception");
            ImGui::TextUnformatted("Loading CL info...");
        }
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
