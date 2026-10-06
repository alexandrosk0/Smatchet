- 2026-09-30 · deviation renewal · [debt] · P3 — toolbar and keybindings each parse args JSON and dispatch an internal command

  Details: `Ui/SmatchetToolbarUi.cpp` (toolbar button) and `Ui/SmatchetUI.cpp` (keybinding dispatch)
  both parse a stored args-JSON string with `json_safe::ParseBounded`, fall back to an empty object on
  a parse error, fill a `CommandContext` with `CommandSource::Internal` and call
  `app.Commands().Dispatch`. Only the log prefix differs. The toolbar copy carries a `rule=duplication`
  exemption (the marker inside `SmatchetToolbarUi::DispatchButton` in `SmatchetToolbarUi.cpp`, which
  carries its revisit date). `Ui/SmatchetImGuiHost.cpp` has a third, stricter variant (rejects
  bad JSON with a ValidationError envelope) that should stay separate.

  Concrete next action: add one UI-side helper, e.g.
  `DispatchInternalCommand(AppController& app, const std::string& commandId, const std::string& argsJson, const char* logSource)`,
  in an existing UI TU that already includes `AppController.h` (a new includer would trip the
  `app-controller-fan-in` gate). Use it from both call sites and delete the toolbar exemption.
  Status: open
  Last-reviewed: 2026-09-30
