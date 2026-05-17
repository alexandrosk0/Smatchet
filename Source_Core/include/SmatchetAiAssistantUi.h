#pragma once

class AppController;
struct UiDrawSession;

#if defined(SMATCHET_WITH_AI)
/// Right-anchored, pinned non-docked Smatchet Assistant side panel. Reads + mutates the
/// `assistant*` fields on `UiDrawSession`; dispatches Submit / Cancel through
/// `AppController::GetAiAssistantController`. Always called every frame from
/// `SmatchetUI::Draw` — the function itself early-returns when `d.assistantPanelOpen`
/// is false (the View menu toggles that flag).
void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d);
#endif
