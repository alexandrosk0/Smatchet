// Extracted from AnnotateAnalysisUi_Modals.cpp DrawClTooltipAsync (user-info-window
// Slice 1). Tooltip body + async fetch + detach/reap lifecycle are unchanged; the
// hover slot and describe cache moved off the Annotate pimpl singleton into this
// module so any window can preview a CL.

#include "Ui/P4ClPreview.h"

#include "Logger.h"
#include "OfflineFirstPure.h" // kLookupRetryAfterSeconds — when a shown failure is asked again

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
    /// HoverCl's finished describe, taken out of HoverFut once: a future holding an exception would
    /// rethrow (and log) on every frame.
    P4ChangelistDetails HoverResult;
    bool HoverResultReady = false;
    std::chrono::steady_clock::time_point HoverResultAt{}; ///< when HoverResult was taken (or last retried)
    std::vector<std::shared_future<P4ChangelistDetails>> DetachedHoverFuts;
};

ClPreviewState& S() {
    static ClPreviewState s;
    return s;
}

ImVec4 ColFromRgba(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

// A shown failure is asked again after the describe cache's own failure backoff, so the retry runs p4.
constexpr std::chrono::seconds kRetryShownFailureAfter{smatchet::offline::kLookupRetryAfterSeconds};

// Describe `cl` off the UI thread into HoverFut (the describe cache answers repeats without p4). When no
// worker can be started (std::async throws std::system_error once threads run out), the failure becomes
// the tooltip's error and is retried after the usual backoff, instead of escaping the frame or leaving a
// "Loading" that never ends.
void StartDescribe(const AnnotateAnalysisConfig& cfg, const std::string& cl,
                   std::chrono::steady_clock::time_point now) {
    try {
        AnnotateAnalysisConfig cfgCopy = cfg;
        S().HoverFut =
            std::async(std::launch::async, [cfgCopy, cl]() { return S().Cache.GetOrFetch(cfgCopy, cl); }).share();
    } catch (const std::exception& ex) {
        LOG_WARN("P4ClPreview: could not start the changelist describe for CL %s: %s", cl.c_str(), ex.what());
        S().HoverFut = std::shared_future<P4ChangelistDetails>();
        S().HoverResult = P4ChangelistDetails();
        S().HoverResult.Error = std::string("Could not load CL info: ") + ex.what();
        S().HoverResultReady = true;
        S().HoverResultAt = now;
    }
}

// The finished describe in `fut`. A worker that threw becomes an error to show (never a load that looks as
// if it were still running); it is not a describe answer, so it leaves Loaded false and nothing caches it.
P4ChangelistDetails TakeFinishedDescribe(const std::shared_future<P4ChangelistDetails>& fut) {
    try {
        return fut.get();
    } catch (const std::exception& ex) {
        LOG_WARN("P4ClPreview: changelist describe worker failed: %s", ex.what());
        P4ChangelistDetails failed;
        failed.Error = std::string("Could not load CL info: ") + ex.what();
        return failed;
    } catch (...) {
        LOG_WARN("P4ClPreview: changelist describe worker failed with a non-standard exception");
        P4ChangelistDetails failed;
        failed.Error = "Could not load CL info: unknown error";
        return failed;
    }
}

// Tooltip body for a finished describe: its error, or the CL header and details.
void DrawDescribeDetails(const std::string& cl, const P4ChangelistDetails& d, const AnnotateUiThemeColors& theme) {
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
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (S().HoverCl != cl) {
        // Detach a still-pending previous fetch before overwriting: HoverFut comes from
        // std::async, so destroying the last reference to an unready state blocks until the
        // task finishes — a p4-describe-length stall on the UI thread (Pillar 2).
        if (S().HoverFut.valid() && S().HoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            S().DetachedHoverFuts.push_back(S().HoverFut);
        }
        S().HoverCl = cl;
        S().HoverResult = P4ChangelistDetails();
        S().HoverResultReady = false;
        StartDescribe(cfg, cl, now);
    } else if (S().HoverResultReady && !S().HoverResult.Error.empty() && !S().HoverFut.valid() &&
               now - S().HoverResultAt >= kRetryShownFailureAfter) {
        // Still hovering a CL whose describe failed: ask again once the describe cache's backoff has
        // passed (a server that was unreachable may be back). The error stays up until the answer lands.
        S().HoverResultAt = now;
        StartDescribe(cfg, cl, now);
    }
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Left-click this changelist cell to open it in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (S().HoverFut.valid() && S().HoverFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        S().HoverResult = TakeFinishedDescribe(S().HoverFut);
        S().HoverResultReady = true;
        S().HoverResultAt = now;
        S().HoverFut = std::shared_future<P4ChangelistDetails>(); // ready, so releasing it never blocks
    }
    if (S().HoverResultReady) {
        DrawDescribeDetails(cl, S().HoverResult, theme);
    } else {
        // Nothing to show yet: the describe for this CL is still running.
        ImGui::TextUnformatted("Loading CL info...");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DetachInFlight() {
    S().HoverCl.clear();
    S().HoverResult = P4ChangelistDetails();
    S().HoverResultReady = false;
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
