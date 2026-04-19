#pragma once

#include "Views.h"

#include "BlameAnalysisUi.h"

class AppController; // Forward declaration

class SmatchetUI {
public:
    void Draw(AppController& app);

private:
    Views ViewState;
    BlameAnalysisUi blameAnalysisUi_;

    void drawEnsureCatalogAndInitialSync(AppController& app);
    void drawMainMenuBar(AppController& app);
    void drawPreferencesWindow(AppController& app);
    void drawViewsDashboardWindow(AppController& app);
    void drawActiveProjectWindow(AppController& app);
    void drawAttachmentPreviewWindow(AppController& app);
    void drawAIAssistantWindow(AppController& app);
    void drawLogWindow();
    void drawBulkImportWindow(AppController& app);
    void drawBulkExportWindow(AppController& app);
};