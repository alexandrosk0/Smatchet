#pragma once

class AppController;
struct UiDrawSession;
struct ViewDefinition;

#if defined(SMATCHET_WITH_AI)
/// Right-anchored, pinned non-docked Smatchet Assistant side panel. Reads + mutates the
/// `assistant*` fields on `UiDrawSession`; dispatches Submit / Cancel through
/// `AppController::GetAiAssistantController`. Always called every frame from
/// `SmatchetUI::Draw` — the function itself early-returns when `d.assistantPanelOpen`
/// is false (the View menu toggles that flag).
/// `activeView` is the currently-active `ViewDefinition*` from `SmatchetUI::ViewState`
/// (may be null). Phase C uses it to populate the ActiveView auto-context block; the
/// pointer is read-only and only valid for the duration of the call.
/// embedded=true (dual-ui slice 4): mobile AI page-body fill — skip the window chrome
/// (Begin/End + the `assistantPanelOpen` gate + dock/focus mechanics + open-state persist)
/// and draw the header/history/input body directly into the caller's region. Default false
/// = the desktop dock window, byte-identical to the pre-slice-4 path.
void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                                  bool embedded = false);

/// Drop every per-conversation render cache (plan / height / body-hash). Callers that swap
/// `assistantHistory` wholesale (e.g. a scenario restoring saved history) must invoke this —
/// the index-keyed caches otherwise reuse stale entries when the size is unchanged.
void SmatchetClearAiRenderCaches();
#endif
