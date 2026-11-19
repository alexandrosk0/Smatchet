#pragma once

#include <string>
#include <vector>

// The shared body of every value dropdown (docs/plans/shared-dropdown-recents.md): a filter field pinned on
// top, an optional "Recent" section with the last picks, the option rows, and a keyboard-hint footer.
//
// Use: open a combo or popup, fill a list of Item, call DrawBody between the begin and end calls, and
// act on the Event it returns (a pick, a clear, a create).
//
// The caller owns the popup (so cell-editor arming and popup ids stay as they are) and whatever it draws
// above the body (a freshness cue, say). The body owns everything else:
//   - Typing always reaches the filter. The filter field takes focus when the popup opens and takes it
//     back after a row click, a wheel scroll or a scrollbar drag; a character typed in between is put
//     into the filter directly, so no keystroke is lost.
//   - Up / Down / PageUp / PageDown move a highlight, Enter acts on it, Esc closes the popup.
//   - A pick is recorded in smatchet::recents under Options::RecentsScope, and the Recent section reads
//     from there (memory only: no I/O on the frame thread, works offline).
//   - A single-select pick closes the popup. A multi-select pick leaves it open.
// Per-popup state (filter text, highlight) is kept inside, keyed by the popup window, and starts fresh
// each time the popup opens.
//
// Combo callers pass ImGuiComboFlags_HeightLargest so the combo's own height limit does not add a second
// scrollbar around the body's list.
//
// Lives at the include/ root, with no Ui/ dependency: the domain-side cell editors use it too.
namespace smatchet {
namespace dropdown {

/// One option. The pointers are read during DrawBody only; null reads as "".
struct Item {
    const char* Id = "";             // what a pick reports and what recents store
    const char* Label = "";          // row text; the filter matches it
    const char* Secondary = nullptr; // dimmed text on the right (an e-mail, a key); the filter matches it too
    const char* Tooltip = nullptr;   // shown on hover
    bool Checked = false;            // the current value (single-select) or a ticked value (multi-select)
    bool Disabled = false;           // listed, cannot be picked
};

struct Options {
    const char* Hint = nullptr;         // filter placeholder; null = "Filter"
    bool MultiSelect = false;           // tick-box rows; a pick toggles and the popup stays open
    bool KeepCheckedVisible = false;    // ticked options stay listed when the filter does not match them
    bool AllowCreate = false;           // offer an 'Add "<text>"' row when no option is named like the filter
    const char* ClearLabel = nullptr;   // non-null: a clear row above the list
    const char* RecentsScope = nullptr; // smatchet::recents scope; null or "" = no recents
    int RecentsMinOptions = 10;         // the Recent section needs more options than this
    bool RecentsShowUnknown = false;    // also list recent ids that are not among the options
    const char* EmptyText = nullptr;    // shown when there are no options (also under a lone clear row); null =
                                        // "No options", "" = nothing
    const char* EmptyHint = nullptr;    // extra wrapped line under EmptyText
    bool ShowFooter = true;             // the keyboard-hint row
    int MaxVisibleRows = 10;            // list height in rows before it scrolls
};

enum class EventKind {
    None,
    Picked,  // an option row: Id (and ItemIndex, or -1 for a recent id outside the options)
    Cleared, // the clear row
    Created, // the 'Add "<text>"' row: Id is the trimmed filter text
};

struct Event {
    EventKind Kind = EventKind::None;
    int ItemIndex = -1;
    std::string Id;
};

/// Draws the body inside the current popup window and returns what the user did this frame.
Event DrawBody(const Item* items, int count, const Options& options);
Event DrawBody(const std::vector<Item>& items, const Options& options);

/// Options for a plain single-select picker that remembers its picks under `recentsScope`. The returned
/// Options points into `recentsScope`, which must outlive the DrawBody call (so no temporary).
Options SingleSelect(const std::string& recentsScope);
Options SingleSelect(std::string&&) = delete;

/// Options for a tick-box picker: a pick toggles, ticked values stay listed under a filter, and
/// `clearAllLabel` names the row that clears them all. Same lifetime rule for `recentsScope`.
Options TickBoxes(const std::string& recentsScope, const char* clearAllLabel);
Options TickBoxes(std::string&&, const char*) = delete;

} // namespace dropdown
} // namespace smatchet
