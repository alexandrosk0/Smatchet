#pragma once

#include "BlameAnalysisUi.h"
#include "Commands/CommandPaletteUi.h"
#include "ConfigManager.h"
#include "SmatchetThemeIds.h"
#include "TicketGridModel.h"
#include "Views.h"

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

class SmatchetUI {
  public:
    void Draw(AppController& app);

    /// Ring-buffer LRU of recently toggled view command ids (capacity 5, oldest-first on read).
    class RecentViewLru {
      public:
        static const int kCapacity = 5;

        RecentViewLru() : size_(0) {}

        /// Record a view toggle command id (e.g. "view.toggle.log"). O(N), N <= 5.
        void Touch(const std::string& commandId) {
            // Remove existing occurrence to avoid duplicates.
            for (int i = 0; i < size_; ++i) {
                if (entries_[i] == commandId) {
                    for (int j = i; j < size_ - 1; ++j) {
                        entries_[j] = entries_[j + 1];
                    }
                    --size_;
                    break;
                }
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
    BlameAnalysisUi blameAnalysisUi_;
    GridFrameContext gridFrameCtx_;
    smatchet::cmd::CommandPaletteUi commandPalette_;
    // Tracks the palette currently applied to ImGui::GetStyle() so SmatchetUI::Draw can detect
    // a cfg.Theme change and re-apply once per dirty event (not every frame).
    ThemeId lastAppliedTheme_ = ThemeId::SmatchetDark;
    TrackerConfig::UiDensity lastAppliedDensity_ = TrackerConfig::UiDensity::Normal;
    RecentViewLru recentViews_;

    void drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d);
    void drawMainMenuBar(AppController& app, UiDrawSession& d);
    void drawPreferencesWindow(AppController& app, UiDrawSession& d);
    void drawViewsDashboardWindow(AppController& app, UiDrawSession& d);
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d);
    void drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d);
    static void drawAuditWindow(AppController& app, UiDrawSession& d);
    static void drawLogWindow(UiDrawSession& d);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
