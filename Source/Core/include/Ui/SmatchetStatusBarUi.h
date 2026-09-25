#pragma once

#include "Ui/StatusBarAutoHidePure.h"

class AppController;
struct UiDrawSession;

/// Draws the VS Code-style status bar pinned to the bottom of the main viewport.
/// Caller must check d.cfg.ShowStatusBar before calling; this function always draws.
void DrawStatusBar(AppController& app, const UiDrawSession& d);

/// Draws the floating status bar overlay when in auto-hide mode.
/// The bar floats above docked panels and reveals on attention signals or pointer hover.
void DrawStatusBarAutoHide(AppController& app, const UiDrawSession& d, StatusBarAutoHidePure::State& state);
