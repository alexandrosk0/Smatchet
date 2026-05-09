#pragma once

#include "Views.h"

#include "BlameAnalysisUi.h"

class AppController; // Forward declaration
struct UiDrawSession;

/**
 * Blocking join for UiDrawSession std::async futures that capture `app` (field catalog fetch,
 * grid field commit, new-issue create, bulk-import rows). `~AppController` calls this first; safe
 * to call again if futures are already drained.
 */
void DrainUiDrawSessionFuturesBeforeAppTeardown(AppController& app);

class SmatchetUI {
  public:
    void Draw(AppController& app);

  private:
    Views ViewState;
    BlameAnalysisUi blameAnalysisUi_;

    void drawEnsureCatalogAndInitialSync(AppController& app, UiDrawSession& d);
    void drawMainMenuBar(AppController& app, UiDrawSession& d);
    void drawPreferencesWindow(AppController& app, UiDrawSession& d);
    void drawViewsDashboardWindow(AppController& app, UiDrawSession& d);
    void drawActiveProjectWindow(AppController& app, UiDrawSession& d);
    void drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d);
    static void drawAuditWindow(AppController& app, UiDrawSession& d);
    static void drawAIAssistantWindow(AppController& app, UiDrawSession& d);
    static void drawLogWindow(UiDrawSession& d);
    static void drawBulkImportWindow(AppController& app, UiDrawSession& d);
    static void drawBulkExportWindow(AppController& app, UiDrawSession& d);
    static void prepareTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float defaultW, float defaultH,
                                      bool requestFocus = false);
    static void repairTopLevelWindow(const UiDrawSession& d, const char* layoutKey, float minW, float minH);
    void resetWindowLayoutToDefault(UiDrawSession& d);
};
