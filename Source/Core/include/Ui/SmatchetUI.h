#pragma once

#include "AnnotateAnalysisUi.h"
#include "Commands/CommandPaletteUi.h"
#include "ConfigManager.h"
#include "SmatchetToolbarUi.h"
#include "SmatchetThemeIds.h"
#include "TicketGridModel.h"
#include "Views.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AppController; // Forward declaration
struct UiDrawSession;

/// Per-frame cache for TrackerFieldCatalogIndex + TicketGridColumns, keyed by catalog revision and
/// active view id. Built once per frame in SmatchetUI::Draw before drawMainMenuBar and
/// drawActiveProjectWindow so neither rebuilds it independently.
struct GridFrameContext {
    std::uint64_t catalogRevision = 0;
    std::uint64_t viewsRevision = 0;
    std::string activeViewId;
    std::unique_ptr<TrackerFieldCatalogIndex> catalogIndex;
    std::vector<TicketGridColumn> columns;
};

/**
 * Blocking join for UiDrawSession std::async futures that capture `app` (field catalog fetch,
 * grid field commit, new-issue create, bulk-import rows). `~AppController` calls this first; safe
 * to call again if futures are already drained.
 */
void DrainUiDrawSessionFuturesBeforeAppTeardown(AppController& app);

/**
 * Free-function shim so command handlers (which hold no SmatchetUI*) can trigger a full dock
 * layout reset. Must be called on the UI thread. Equivalent to the menu Workspace > Reset
 * Workspace Layout path.
 */
void SmatchetUI_ResetLayoutToDefault(UiDrawSession& d);

class SmatchetUI {
  public:
    void Draw(AppController& app);

    // Forwarding shims for split-TU helpers in SmatchetPreferencesUi_*.cpp.
    const ViewsStore& GetViewsStore() const { return ViewState.GetStore(); }
    void DrawAnnotatePreferencesTabForwarded(const AppController& app) {
        annotateAnalysisUi_.DrawAnnotatePreferencesTab(app);
    }

    /// Ring-buffer LRU of recently toggled view command ids (capacity 5, oldest-first on read).
    class RecentViewLru {
      public:
        static const int kCapacity = 5;

        RecentViewLru() : size_(0) {}

        /// Record a view toggle command id (e.g. "view.toggle.log"). O(N), N <= 5.
        /// Silently ignores ids that do not start with "view.toggle." — only known
        /// view-toggle commands may enter the LRU, preventing future callers from
        /// accidentally dispatching arbitrary command names via the Recent Views submenu.
        void Touch(const std::string& commandId) {
            static const char kPrefix[] = "view.toggle.";
            if (commandId.compare(0U, sizeof(kPrefix) - 1U, kPrefix) != 0) {
                return;
            }
            // Remove existing occurrence to avoid duplicates.
            std::string* const end = entries_ + size_;
            std::string* const pos = std::find(entries_, end, commandId);
            if (pos != end) {
                for (std::string* p = pos; p < end - 1; ++p) {
                    *p = *(p + 1);
                }
                --size_;
            }
            // Evict oldest if at capacity.
            if (size_ == kCapacity) {
                for (int i = 0; i < kCapacity - 1; ++i) {
                    entries_[i] = entries_[i + 1];
                }
                --size_;
            }
            entries_[size_++] = commandId;
        }

        /// Returns up to kCapacity entries, oldest-first (most-recently used last).
        std::vector<std::string> Snapshot() const {
            std::vector<std::string> result;
            result.reserve(static_cast<size_t>(size_));
            for (int i = 0; i < size_; ++i) {
                result.push_back(entries_[i]);
            }
            return result;
        }

      private:
        std::string entries_[5];
        int size_;
    };

  private:
    Views ViewState;
    AnnotateAnalysisUi annotateAnalysisUi_;
    GridFrameContext gridFrameCtx_;
    smatchet::cmd::CommandPaletteUi commandPalette_;
    SmatchetToolbarUi toolbar_;
    // Tracks the palette currently applied to ImGui::GetStyle() so SmatchetUI::Draw can detect
    // a cfg.Theme change and re-apply once per dirty event (not every frame).
    ThemeId lastAppliedTheme_ = ThemeId::SmatchetDark;
    TrackerConfig::UiDensity lastAppliedDensity_ = TrackerConfig::UiDensity::Normal;
    RecentViewLru recentViews_;

    void drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d);
    void drawMainMenuBar(AppController& app, UiDrawSession& d);
#if defined(SMATCHET_WITH_AI)
    /// Right-anchored Smatchet Assistant side panel. Delegates to the free function in
    /// `SmatchetAiAssistantUi.cpp` after `drawAuditWindow` runs; early-returns inside
    /// the free function when `d.assistantPanelOpen` is false.
    void drawAiAssistantPanel(AppController& app, UiDrawSession& d);
#endif
    void drawPreferencesWindow(AppController& app, UiDrawSession& d);
    void drawViewsDashboardWindow(AppController& app, UiDrawSession& d);
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d);
    void drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d);
    static void drawAuditWindow(AppController& app, UiDrawSession& d);
    static void drawLogWindow(UiDrawSession& d);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
