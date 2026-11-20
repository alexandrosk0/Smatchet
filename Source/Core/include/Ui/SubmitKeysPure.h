#pragma once

/// Pure layout and display rules behind the submit-key widgets in Ui/SmatchetSubmitKeys.h: a button
/// that prints its key inside the frame, and a multiline field that draws its placeholder inside the
/// box. No ImGui here, so the rules are unit-tested on their own; the widgets measure the text and
/// draw.
namespace smatchet {
namespace submitkeys {

/// Where a key-hint button puts its parts, in pixels from the button's left edge.
struct KeyHintButtonLayout {
    float Width = 0.0f; ///< the button's width
    float HintX = 0.0f; ///< where the key name starts; 0 when there is none
};

/// The label sits at the left padding and the key name ends at the right padding, `gap` apart at
/// least. The button grows to fit both, and never shrinks below `minWidth` (a caller's fixed width,
/// so buttons in a row keep lining up); a wider button keeps the key name at its right edge. With
/// no key name (`hintWidth` <= 0) this is a plain button's width. Negative inputs count as 0.
inline KeyHintButtonLayout ComputeKeyHintButtonLayout(float labelWidth, float hintWidth, float gap, float padX,
                                                      float minWidth) {
    const float label = labelWidth > 0.0f ? labelWidth : 0.0f;
    const float pad = padX > 0.0f ? padX : 0.0f;
    const bool hasHint = hintWidth > 0.0f;
    const float hint = hasHint ? hintWidth : 0.0f;
    const float spacing = hasHint && gap > 0.0f ? gap : 0.0f;
    const float needed = pad + label + spacing + hint + pad;
    KeyHintButtonLayout out;
    out.Width = minWidth > needed ? minWidth : needed;
    out.HintX = hasHint ? out.Width - pad - hint : 0.0f;
    return out;
}

/// The placeholder shows while the box holds no text at all, with or without the caret in it, the
/// way ImGui's own single-line hint does. A box holding only spaces shows none: the user typed them.
inline bool ShowsPlaceholder(const char* buf) { return buf == nullptr || buf[0] == '\0'; }

} // namespace submitkeys
} // namespace smatchet
