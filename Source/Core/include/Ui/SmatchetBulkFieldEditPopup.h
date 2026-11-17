#pragma once

// The grid's "Set <Field> for N rows" popup (docs/plans/bulk-edit-rows.md); P1 covers choice fields.

class AppController;
struct UiDrawSession;
struct GridPane;

/// Draws the popup when the pending request belongs to `pane`; a no-op otherwise. UI thread only, memory only.
void DrawBulkFieldEditPopup(AppController& app, UiDrawSession& d, GridPane& pane, bool readOnlyMode);
