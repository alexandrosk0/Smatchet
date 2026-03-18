#pragma once

#include "Views.h"

class AppController; // Forward declaration

class SmatchetUI {
public:
    void Draw(AppController& app);

private:
    Views ViewState;

    void drawEnsureCatalogAndInitialSync(AppController& app);
    void drawMainMenuBar();
    void drawNetworkStatsWindow();
    void drawJiraCredentialsModal(AppController& app);
    void drawViewsDashboardWindow(AppController& app);
    void drawLuaAutomationWindow(AppController& app);
    void drawActiveProjectWindow(AppController& app);
    void drawAIAssistantWindow(AppController& app);
    void drawLogWindow();
};