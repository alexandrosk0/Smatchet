#pragma once

class AppController;
struct UiDrawSession;

/// Draws the VS Code-style status bar pinned to the bottom of the main viewport.
/// Caller must check d.cfg.ShowStatusBar before calling; this function always draws.
void DrawStatusBar(AppController& app, UiDrawSession& d);
