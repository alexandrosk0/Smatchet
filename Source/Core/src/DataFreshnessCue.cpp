#include "DataFreshnessCue.h"

#include "SmatchetLocalization.h"
#include "imgui.h"

namespace DataFreshnessCue {

const char* CueText(smatchet::offline::DataFreshness f) {
    switch (f) {
    case smatchet::offline::DataFreshness::Fresh:
        return "";
    case smatchet::offline::DataFreshness::Refreshing:
        return SmatchetLocalization::T("freshness.refreshing", "Refreshing…");
    case smatchet::offline::DataFreshness::CachedOffline:
        return SmatchetLocalization::T("freshness.cached_offline", "Offline — showing saved data");
    case smatchet::offline::DataFreshness::CachedStale:
        return SmatchetLocalization::T("freshness.cached_stale", "Showing saved data — the last refresh failed");
    case smatchet::offline::DataFreshness::LoadingNoCache:
        return SmatchetLocalization::T("freshness.loading_no_cache", "Loading…");
    case smatchet::offline::DataFreshness::UnavailableNoCache:
        return SmatchetLocalization::T("freshness.unavailable_offline", "Not available offline yet");
    }
    return "";
}

void Draw(smatchet::offline::DataFreshness f, const char* detail) {
    const char* text = CueText(f);
    if (text[0] == '\0') {
        return;
    }
    ImGui::TextDisabled("%s", text);
    if (detail && detail[0] && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", detail);
    }
}

void DrawInlineBadge(smatchet::offline::DataFreshness f, const char* detail) {
    const char* badge = nullptr;
    switch (f) {
    case smatchet::offline::DataFreshness::Refreshing:
    case smatchet::offline::DataFreshness::LoadingNoCache:
        badge = SmatchetLocalization::T("freshness.badge_refreshing", "(refreshing)");
        break;
    case smatchet::offline::DataFreshness::CachedOffline:
    case smatchet::offline::DataFreshness::CachedStale:
        badge = SmatchetLocalization::T("freshness.badge_cached", "(saved)");
        break;
    default:
        return;
    }
    ImGui::TextDisabled("%s", badge);
    if (detail && detail[0] && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", detail);
    }
}

} // namespace DataFreshnessCue
