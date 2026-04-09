#pragma once

#include <string>

class AppController;

class BlameAnalysisUi {
public:
    /** Call each frame with whether the blame panel is shown (used to detect open/close). */
    void SetBlamePanelOpen(bool open);

    /** Join worker / merge results / poll detail futures (call every frame). */
    void ServiceBackground();

    void DrawWindow(AppController& app, bool* pOpen, const std::string& selectedJiraIssueKey);

private:
    bool cfgLoaded_ = false;
    bool blamePanelOpen_ = false;
    bool blameOpenPrev_ = false;
};
