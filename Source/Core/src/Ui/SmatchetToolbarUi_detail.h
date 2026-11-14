#ifndef SMATCHET_UI_TOOLBAR_UI_DETAIL_H
#define SMATCHET_UI_TOOLBAR_UI_DETAIL_H

// Pure (ImGui-free) helpers extracted from SmatchetToolbarUi so the draw bodies
// stay layout-only and the value-formatting / index math / shortcut resolution is
// bucket-A testable. See docs/plans/shipped/decompose-top-20-monoliths.md.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Commands/FuzzyMatch.h"
#include "Config/KeybindingsConfig.h" // BoundHotkeyDisplayAll — tooltip shortcut resolution
#include "Config/ToolbarConfig.h"
#include "Json/BoundedJsonParse.h"
#include "Ui/SmatchetIconPickerUi.h" // SmatchetToolbarIconGlyph

namespace smatchet {
namespace toolbar_editor {

// Label shown for one button row in the editor's left-hand list. Separators get a
// fixed marker; real buttons get an icon prefix (a FontAwesome glyph when loaded,
// otherwise a bracket placeholder) plus the tooltip, the command id, or a fallback.
inline std::string EditorRowLabel(const ToolbarButton& b, bool faLoaded) {
    if (b.Kind == ToolbarButtonKind::Separator) {
        return "--- separator ---";
    }
    const std::string glyph = b.IconGlyph.empty() ? SmatchetToolbarIconGlyph(b.IconName) : b.IconGlyph;
    std::string label = (faLoaded && !glyph.empty()) ? (glyph + "  ") : std::string("[ ]  ");
    if (!b.Tooltip.empty()) {
        label += b.Tooltip;
    } else if (!b.CommandId.empty()) {
        label += b.CommandId;
    } else {
        label += "(unset)";
    }
    return label;
}

// Destination index for a drag-drop reorder that erases `src` then inserts before
// the original target `dst`. Erasing an earlier element shifts the target left by
// one, so the insert position is dst-1 when src precedes dst, dst otherwise.
inline int DragDropDestIndex(int src, int dst) { return (src < dst) ? (dst - 1) : dst; }

// Shortcut text for one toolbar button's hover tooltip: every combo bound to the
// button's OWN (CommandId, ArgsJson) identity — the very pair DispatchButton
// dispatches. The args matter: BoundHotkeyDisplayAll matches args semantically and
// has NO command-id-only fallback, so resolving against a hardcoded "{}" returned
// nothing for a button bound with non-default args (e.g. `view.sidebar.primary
// {"action":"toggle"}`) and the tooltip silently lost its shortcut. Empty for
// separators, Lua buttons, an unset command id, or an unbound command.
inline std::string ToolbarButtonShortcutDisplay(const std::vector<Keybinding>& bindings, const ToolbarButton& b) {
    if (b.Kind != ToolbarButtonKind::Command || b.CommandId.empty()) {
        return std::string();
    }
    return BoundHotkeyDisplayAll(bindings, b.CommandId, b.ArgsJson);
}

// `s` without leading and trailing ASCII whitespace.
inline std::string TrimAscii(const std::string& s) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isSpace(s[b])) {
        ++b;
    }
    while (e > b && isSpace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

// What a toolbar button's args text is, for the editor's check and for the click on the bar.
enum class ArgsJsonStatus { Ok = 0, Invalid = 1, TooLarge = 2, TooDeep = 3, NotObject = 4 };

// Parses `text` as a toolbar button's args. Empty or whitespace-only text is Ok and means {}. Anything else
// must be a JSON object: the command registry swaps any other JSON value for {} without saying so, which
// runs the command with different args than the button shows. On Ok `out` holds the object; on every other
// status it is {}.
inline ArgsJsonStatus ParseToolbarArgsJson(const std::string& text, nlohmann::json& out) {
    out = nlohmann::json::object();
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty()) {
        return ArgsJsonStatus::Ok;
    }
    std::string err;
    nlohmann::json parsed = smatchet::json_safe::ParseBounded(trimmed, err);
    if (!err.empty()) {
        if (err == smatchet::json_safe::TooLargeError()) {
            return ArgsJsonStatus::TooLarge;
        }
        if (err == smatchet::json_safe::OverflowError()) {
            return ArgsJsonStatus::TooDeep;
        }
        return ArgsJsonStatus::Invalid;
    }
    if (!parsed.is_object()) {
        return ArgsJsonStatus::NotObject;
    }
    out = std::move(parsed);
    return ArgsJsonStatus::Ok;
}

// Indices into `names`, filtered and ranked for the editor's command list. `haystacks[i]` is the text
// entry i is matched against (its name plus its title); a missing entry falls back to the name. The query
// is trimmed. An empty query keeps every entry in its given order. Otherwise the match is the command
// palette's: case-insensitive, the query's characters in order, best score first, ties by name.
inline std::vector<int> RankCommandMatches(const std::vector<std::string>& names,
                                           const std::vector<std::string>& haystacks, const std::string& rawQuery) {
    const std::string query = TrimAscii(rawQuery);
    std::vector<int> out;
    std::vector<int> scores(names.size(), 1);
    out.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!query.empty()) {
            scores[i] = smatchet::cmd::FuzzyScore(query, i < haystacks.size() ? haystacks[i] : names[i]);
        }
        if (scores[i] > 0) {
            out.push_back(static_cast<int>(i));
        }
    }
    if (!query.empty()) {
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
            const std::size_t ia = static_cast<std::size_t>(a);
            const std::size_t ib = static_cast<std::size_t>(b);
            if (scores[ia] != scores[ib]) {
                return scores[ia] > scores[ib];
            }
            return names[ia] < names[ib];
        });
    }
    return out;
}

} // namespace toolbar_editor
} // namespace smatchet

#endif // SMATCHET_UI_TOOLBAR_UI_DETAIL_H
