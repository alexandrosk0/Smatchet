#ifndef SMATCHET_UI_SMATCHET_ABOUT_UI_H
#define SMATCHET_UI_SMATCHET_ABOUT_UI_H

// "About Smatchet" modal — version, build provenance, git commit, runtime
// environment and third-party credits, plus copy-to-clipboard / open-repo /
// check-for-updates actions. Core/Ui, dual-target (no GL/GLFW).
// All data comes from diagnostics::AboutInfo; this TU only renders it.
// docs/plans/shipped/about-dialog-help-menu.md Slice 4.

class AppController;
struct UiDrawSession;

void DrawAboutModal(AppController& app, UiDrawSession& d);

#endif // SMATCHET_UI_SMATCHET_ABOUT_UI_H
