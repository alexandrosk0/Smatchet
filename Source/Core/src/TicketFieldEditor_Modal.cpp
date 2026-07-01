#include "TicketFieldEditor.h"
#include "TicketFieldEditor_detail.h"

#include "AppController.h"
#include "CppSyntaxHighlight.h"
#include "DictationInsertionRouter.h"
#include "SmatchetFieldRender.h"
#include "MarkdownConvert.h"
#include "MarkdownPreviewRender.h"
#include "SmatchetLocalization.h"
#include "SmatchetThemedTextEditorPalette.h"
#include "TextEditor.h"
#include "TicketFieldEditorLongTextPure.h"
#include "TrackerFieldSchema.h"

#include "Ui/SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

// g_ui — unconditional extern (defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern is gated). Read for
// the persisted default-view preference (cfg.DefaultLongTextEditorPreview) when
// opening the long-text modal. Same shim DockGapSentinelScenario uses.
extern UiDrawSession g_ui;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using LongTextRichKind = TicketFieldEditorLongTextPure::LongTextRichKind;

/// Singleton state for the long-text / ADF field modal editor. Decoupled from `SpreadsheetState`
/// so the modal survives the originating cell scrolling out of view: the cell triggers
/// `JustOpened`, then the top-level `RenderLongTextModal` owns the lifecycle.
struct ActiveLongTextEditorState {
    std::string IssueId;
    TrackerField Field;
    std::string FieldLabel;
    std::string OriginalStrippedValue;
    /// Markdown that seeded the buffer (converted from `OriginalRichValue`). Used for
    /// "did the user change anything?" detection on save against the editor surface.
    std::string OriginalMarkdown;
    /// Rich payload (ADF JSON or HTML) at modal-open time. Empty for legacy tickets
    /// that had no cached rich value.
    std::string OriginalRichValue;
    LongTextRichKind RichKind = LongTextRichKind::None;
    /// True when HtmlSubsetToMarkdown tripped the fallback — modal shows a banner and
    /// edits the raw HTML directly.
    bool RawMode = false;
    /// ADF node types that AdfToMarkdown skipped (panels, mentions, smart links, ...).
    std::vector<std::string> DroppedAdfNodeTypes;

    static constexpr size_t kBufferSize = 64 * 1024;
    std::vector<char> Buffer;
    /// True when the seed (OriginalMarkdown, or OriginalRichValue in RawMode) exceeded
    /// kBufferSize - 1 and had to be truncated to fit Buffer. CommitLongTextEdit diffs
    /// against BufferSeedShown (the truncated copy actually loaded into Buffer) rather
    /// than the full untruncated seed, so an unmodified over-limit document is never
    /// re-saved — that would silently overwrite the tracker field with truncated text.
    bool SeedTruncated = false;
    /// Exact string written into Buffer at seed time (== seed, or its first kBufferSize-1
    /// bytes when SeedTruncated). The Save-diff baseline — see SeedTruncated.
    std::string BufferSeedShown;
    bool Active = false;
    bool JustOpened = false;
    /// Three-way preview mode. Ctrl+P cycles Edit -> Split -> Preview -> Edit.
    enum class PreviewModeT { EditOnly, Split, PreviewOnly };
    PreviewModeT PreviewMode = PreviewModeT::EditOnly;
    /// Editor's fraction of the horizontal pane area in Split mode. Clamped to [0.15, 0.85].
    float SplitterRatio = 0.5f;
    /// Last frame's scroll positions — used to mirror the pane the user scrolled in Split mode.
    float LastEditorScrollY = 0.0f;
    float LastPreviewScrollY = 0.0f;
    /// Round-trip preview cache: Markdown -> ADF/HTML -> Markdown. Recomputed lazily when the
    /// buffer changes (debounced 100 ms) so conversion loss surfaces before save.
    std::string LastRoundTripInput;
    std::string LastRoundTripOutput;
    bool LastRoundTripLossy = false;
    bool RoundTripPending = false;
    std::chrono::steady_clock::time_point RoundTripFireAt{};
    /// Pillar 2 — set while a worker thread computes the Markdown seed from a large rich value.
    /// Buffer holds a placeholder until the worker posts back via MainThreadDispatcher.
    bool LoadingMarkdown = false;
    /// Monotonically-increasing generation token. Worker compares against current LoadGen on
    /// post-back and discards if the user opened a different editor since dispatch.
    int LoadGen = 0;
};

ActiveLongTextEditorState s_ActiveLongTextState;

constexpr const char* kLongTextModalPopupId = "EditLongTextModal";

using TicketFieldEditorLongTextPure::ClassifyRichValue;

/// Pillar 2 — rich values above this size are parsed on a worker thread. 32 KB keeps normal
/// cases (sub-50 KB descriptions) on the sync path while gating pathological payloads (>1 MB ADF).
constexpr size_t kAsyncRichSeedThresholdBytes = 32 * 1024;

using TicketFieldEditorLongTextPure::ComputeLongTextSeed;
using TicketFieldEditorLongTextPure::ComputeRoundTripPreview;
using TicketFieldEditorLongTextPure::RoundTripPreview;

// Per-frame layout + mode snapshot for the long-text modal sections. Computed once in
// RenderLongTextModal and threaded into the editor / splitter / preview / footer helpers so the
// positional-ImGui pairs stay inside one helper each (the documented DrawCtx pattern).
struct LongTextModalCtx {
    ActiveLongTextEditorState::PreviewModeT EffectiveMode = ActiveLongTextEditorState::PreviewModeT::EditOnly;
    bool CtrlDown = false;
    float SplitterW = 6.0f;
    float PanesW = 0.0f;
    float EditorW = 0.0f;
    float PreviewW = 0.0f;
    float InputH = 0.0f;
    ImGuiID EditorId = 0;
};

// Draws the required-field marker and the loading / raw-HTML / dropped-ADF-nodes fidelity banners
// at the top of the modal. Extracted verbatim from RenderLongTextModal; behaviour byte-identical.
void DrawLongTextBanners() {
    ImGui::Text("Edit %s — %s", s_ActiveLongTextState.FieldLabel.c_str(), s_ActiveLongTextState.IssueId.c_str());
    // Required-field marker: red asterisk with tooltip, mirrors the inline-glyph in new-issue draft rows.
    if (s_ActiveLongTextState.Field.IsRequired) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextUnformatted("*");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", SmatchetLocalization::T("field.required_tooltip", "Required field"));
        }
    }

    // Pillar 2 — async-seed banner shown while worker computes the Markdown seed.
    if (s_ActiveLongTextState.LoadingMarkdown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("Loading description...");
        ImGui::PopStyleColor();
    }

    // Content exceeds the editor's fixed buffer — anything past kBufferSize - 1 bytes is not
    // loaded and is NOT part of the Save diff baseline (BufferSeedShown), so editing and saving
    // here will drop the untruncated tail. See CPP_CODE_AUDIT.md #1.
    if (s_ActiveLongTextState.SeedTruncated) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("This description is too large for the editor (over 64 KB) — only the "
                           "first 64 KB is shown. Saving will overwrite the tracker field with just "
                           "the truncated text below; edit elsewhere for the full document.");
        ImGui::PopStyleColor();
    }

    // Format-fidelity banners. RawMode is the strongest signal — editing raw HTML directly.
    // DroppedAdfNodeTypes is informational: Markdown is missing some constructs but still editable.
    if (s_ActiveLongTextState.RawMode) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
        ImGui::TextWrapped("Editing raw HTML — the source contains tags this build doesn't yet "
                           "translate to Markdown. Save will store your edits verbatim.");
        ImGui::PopStyleColor();
    } else if (!s_ActiveLongTextState.DroppedAdfNodeTypes.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
        std::string list;
        for (size_t i = 0; i < s_ActiveLongTextState.DroppedAdfNodeTypes.size() && i < 5; ++i) {
            if (!list.empty())
                list += ", ";
            list += s_ActiveLongTextState.DroppedAdfNodeTypes[i];
        }
        ImGui::TextWrapped("Note: this document contains constructs not in our Markdown subset (%s) — "
                           "saving will keep what you edit but those nodes are not shown.",
                           list.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

// Handles the Ctrl+P preview-mode cycle (Edit -> Split -> Preview -> Edit), disabled in raw mode.
// Extracted from RenderLongTextModal; mutates s_ActiveLongTextState.PreviewMode in place.
void HandleLongTextPreviewModeHotkey(bool ctrlDown) {
    using PreviewModeT = ActiveLongTextEditorState::PreviewModeT;
    if (!s_ActiveLongTextState.RawMode && ctrlDown && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        switch (s_ActiveLongTextState.PreviewMode) {
        case PreviewModeT::EditOnly:
            s_ActiveLongTextState.PreviewMode = PreviewModeT::Split;
            break;
        case PreviewModeT::Split:
            s_ActiveLongTextState.PreviewMode = PreviewModeT::PreviewOnly;
            break;
        case PreviewModeT::PreviewOnly:
            s_ActiveLongTextState.PreviewMode = PreviewModeT::EditOnly;
            break;
        }
    }
}

// Draws the Markdown text-editor pane (ImGuiColorTextEdit), syncing both ways against the modal
// buffer per frame (dictation/async-seed reseed in, user typing copied out) and swapping the
// language definition for callstack fields. Extracted from RenderLongTextModal; byte-identical.
void DrawLongTextEditorPane(const LongTextModalCtx& ctx) {
    // Long-text edit mode renders via ImGuiColorTextEdit TextEditor with the Markdown LD.
    // Synced both ways against s_ActiveLongTextState.Buffer per frame so dictation router
    // and async-seed paths continue to work without further hook changes:
    //   - Before render: Buffer c_str differs from editor text and IsTextChanged() didn't
    //     fire last frame → external change (dictation/async seed). Reseed editor.
    //   - After render: IsTextChanged() fires → user typed. Copy GetText() to Buffer.
    static TextEditor s_LongTextEditor;
    static bool s_LongTextEditorInitialized = false;
    if (!s_LongTextEditorInitialized) {
        s_LongTextEditorInitialized = true;
        s_LongTextEditor.SetShowWhitespaces(false);
        s_LongTextEditor.SetShowLineNumbers(false);
        s_LongTextEditor.SetLanguageDefinition(TextEditor::LanguageDefinition::Markdown());
        s_LongTextEditor.SetColorizerEnable(true);
    }
    // Slice 7 of code-syntax-coloring-and-tooltips — swap to C++ LD when
    // the active field is the configured callstack field. SetLanguageDefinition swaps the
    // table pointer; applied per-frame because the same editor instance hosts both kinds.
    static bool s_LongTextEditorIsCallstackLd = false;
    const bool wantCallstackLd = IsCallstackFieldId(s_ActiveLongTextState.Field.Id);
    if (wantCallstackLd != s_LongTextEditorIsCallstackLd) {
        s_LongTextEditor.SetLanguageDefinition(wantCallstackLd ? TextEditor::LanguageDefinition::CPlusPlus()
                                                               : TextEditor::LanguageDefinition::Markdown());
        s_LongTextEditorIsCallstackLd = wantCallstackLd;
        // Re-arm the colorizer after LD swap. SetLanguageDefinition calls Colorize() but
        // against the previous field's mLines. SetText below rebuilds mLines and colorizes
        // again. This is a belt-and-suspenders guard for the case where the buffer already
        // matches the editor, so SetText is skipped, yet the LD just changed.
        s_LongTextEditor.Colorize(0, -1);
    }
    {
        const std::string bufferAsStr(s_ActiveLongTextState.Buffer.data());
        const std::string editorAsStr = s_LongTextEditor.GetText();
        // TextEditor::GetText() appends a trailing '\n'; strip it for round-trip comparison.
        std::string editorTrimmed = editorAsStr;
        if (!editorTrimmed.empty() && editorTrimmed.back() == '\n') {
            editorTrimmed.pop_back();
        }
        if (bufferAsStr != editorTrimmed) {
            s_LongTextEditor.SetText(bufferAsStr);
            s_LongTextEditor.Colorize(0, -1);
        }
    }
    // Apply theme-aware palette per-frame. Without this the TextEditor uses its built-in
    // dark palette; same pattern as Lua console + AI chat editor.
    s_LongTextEditor.SetPalette(SmatchetTheme::GetThemedLuaConsolePalette());
    s_LongTextEditor.Render("##LongTextEditorBuf", ImVec2(ctx.EditorW, ctx.InputH), false);
    if (s_LongTextEditor.IsTextChanged()) {
        std::string newText = s_LongTextEditor.GetText();
        if (!newText.empty() && newText.back() == '\n') {
            newText.pop_back();
        }
        const std::size_t copy = (std::min)(newText.size(), ActiveLongTextEditorState::kBufferSize - 1);
        std::memcpy(s_ActiveLongTextState.Buffer.data(), newText.data(), copy);
        s_ActiveLongTextState.Buffer[copy] = '\0';
    }
}

// Draws the draggable Split-mode pane divider and updates SplitterRatio on drag. Extracted from
// RenderLongTextModal; byte-identical. Caller invokes only in Split mode.
void DrawLongTextSplitter(const LongTextModalCtx& ctx) {
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##LongTextSplitter", ImVec2(ctx.SplitterW, ctx.InputH));
    const bool splitterHover = ImGui::IsItemHovered();
    const bool splitterActive = ImGui::IsItemActive();
    if (splitterHover || splitterActive) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (splitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const float dx = ImGui::GetIO().MouseDelta.x;
        if (ctx.PanesW > 0.0f) {
            float r = s_ActiveLongTextState.SplitterRatio + dx / ctx.PanesW;
            if (r < 0.15f)
                r = 0.15f;
            if (r > 0.85f)
                r = 0.85f;
            s_ActiveLongTextState.SplitterRatio = r;
        }
    }
    // Faint vertical bar — brighter on hover/drag.
    const ImVec2 sMin = ImGui::GetItemRectMin();
    const ImVec2 sMax = ImGui::GetItemRectMax();
    const ImU32 col = ImGui::GetColorU32(splitterActive  ? ImGuiCol_SeparatorActive
                                         : splitterHover ? ImGuiCol_SeparatorHovered
                                                         : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddRectFilled(sMin, sMax, col);
    ImGui::SameLine(0.0f, 0.0f);
}

// Draws the preview pane: debounced round-trip of the buffer through the save converter, lossy
// banner, markdown (or callstack) render, and Split-mode scroll mirroring. Extracted from
// RenderLongTextModal; byte-identical. Caller invokes only when the mode shows a preview.
void DrawLongTextPreviewPane(const LongTextModalCtx& ctx) {
    using PreviewModeT = ActiveLongTextEditorState::PreviewModeT;
    ImGui::BeginChild("##LongTextPreview", ImVec2(ctx.PreviewW, ctx.InputH), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // Capture the preview window pointer while it's current — BeginChild munges the name
    // so FindWindowByName won't match the literal id.
    ::ImGuiWindow* previewWin = ::ImGui::GetCurrentWindow();
    const std::string md(s_ActiveLongTextState.Buffer.data());

    // Round-trip the buffer through the same converter that runs on save
    // (Markdown -> ADF / HTML -> Markdown), then render the result. Any conversion loss
    // surfaces here before the user clicks Save. Debounced: arm a 100 ms timer on each
    // buffer change; fire only when idle so rapid keystrokes don't re-convert every frame.
    if (md != s_ActiveLongTextState.LastRoundTripInput && !s_ActiveLongTextState.RoundTripPending) {
        s_ActiveLongTextState.RoundTripPending = true;
        s_ActiveLongTextState.RoundTripFireAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
    if (s_ActiveLongTextState.RoundTripPending &&
        std::chrono::steady_clock::now() >= s_ActiveLongTextState.RoundTripFireAt) {
        s_ActiveLongTextState.RoundTripPending = false;
        RoundTripPreview rt = ComputeRoundTripPreview(s_ActiveLongTextState.RichKind, md);
        s_ActiveLongTextState.LastRoundTripInput = md;
        s_ActiveLongTextState.LastRoundTripOutput = std::move(rt.Rendered);
        s_ActiveLongTextState.LastRoundTripLossy = rt.Lossy;
    }
    if (s_ActiveLongTextState.LastRoundTripLossy) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::TextUnformatted("(lossy round-trip — some constructs will be dropped on save)");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    // Slice 7 of code-syntax-coloring-and-tooltips — when the active field
    // is the configured callstack field, render via the semantic callstack tokenizer instead
    // of the markdown-prose pipeline. Use the raw buffer (md) rather than LastRoundTripOutput:
    // the ADF/HTML converter mangles callstack punctuation, losing the tokens the colorizer needs.
    if (IsCallstackFieldId(s_ActiveLongTextState.Field.Id)) {
        DrawColoredCallstackText(md.c_str());
    } else {
        MarkdownPreviewRender::Options previewOpts;
        previewOpts.selectableId = "##LongTextPreviewSel";
        MarkdownPreviewRender::Render(s_ActiveLongTextState.LastRoundTripOutput, previewOpts);
    }
    const float previewScroll = ImGui::GetScrollY();
    const float previewScrollMax = ImGui::GetScrollMaxY();
    ImGui::EndChild();

    // Scroll-sync: in Split mode, mirror whichever pane's scroll changed this frame onto the
    // other. Editor scroll comes from the multi-line text child, looked up by id. The editor
    // wins on ties, since typing-driven scroll is the common case.
    if (ctx.EffectiveMode == PreviewModeT::Split) {
        ::ImGuiWindow* editorWin = ::ImGui::FindWindowByID(ctx.EditorId);
        const float editorScroll = editorWin ? editorWin->Scroll.y : 0.0f;
        const float editorScrollMax = editorWin ? editorWin->ScrollMax.y : 0.0f;

        const bool editorChanged = std::fabs(editorScroll - s_ActiveLongTextState.LastEditorScrollY) > 0.5f;
        const bool previewChanged = std::fabs(previewScroll - s_ActiveLongTextState.LastPreviewScrollY) > 0.5f;

        if (editorChanged && previewWin && editorScrollMax > 0.0f && previewScrollMax > 0.0f) {
            const float r = editorScroll / editorScrollMax;
            previewWin->Scroll.y = r * previewScrollMax;
        } else if (previewChanged && editorWin && editorScrollMax > 0.0f && previewScrollMax > 0.0f) {
            const float r = previewScroll / previewScrollMax;
            editorWin->Scroll.y = r * editorScrollMax;
        }

        s_ActiveLongTextState.LastEditorScrollY = editorWin ? editorWin->Scroll.y : 0.0f;
        s_ActiveLongTextState.LastPreviewScrollY = previewWin ? previewWin->Scroll.y : previewScroll;
    } else {
        s_ActiveLongTextState.LastPreviewScrollY = previewScroll;
    }
}

// Commits the edited buffer (diffed against the seed to skip null edits) into pendingEdits, then
// closes the modal. Extracted from RenderLongTextModal's save branch; byte-identical.
void CommitLongTextEdit(std::vector<PendingFieldEdit>& pendingEdits) {
    const std::string newValue(s_ActiveLongTextState.Buffer.data());
    // Diff against BufferSeedShown — exactly what was loaded into Buffer at seed time (the
    // seeded markdown, or raw HTML in RawMode), truncated the same way if the seed exceeded
    // kBufferSize - 1. Diffing against the untruncated OriginalMarkdown/OriginalRichValue
    // instead would make an unmodified over-limit document always look "changed" (it can never
    // equal its own untruncated seed), queuing a save that overwrites the tracker field with
    // truncated text purely because it was opened and closed — see CPP_CODE_AUDIT.md #1.
    const std::string& seed = s_ActiveLongTextState.BufferSeedShown;
    if (newValue != seed) {
        PendingFieldEdit edit;
        edit.IssueId = s_ActiveLongTextState.IssueId;
        edit.Field = s_ActiveLongTextState.Field;
        edit.Values = {newValue};
        edit.Preformatted = s_ActiveLongTextState.RawMode;
        edit.OriginalRichValue = s_ActiveLongTextState.OriginalRichValue;
        pendingEdits.push_back(std::move(edit));
    }
    ImGui::CloseCurrentPopup();
    CloseLongTextEditor();
}

// Draws the footer hint, Save / Cancel buttons, and the Edit/Split/Preview segmented control, then
// applies the save (Ctrl+Enter / Save) or cancel (Esc / Cancel) action. Extracted from
// RenderLongTextModal; byte-identical.
void DrawLongTextFooter(const LongTextModalCtx& ctx, std::vector<PendingFieldEdit>& pendingEdits) {
    // Ctrl+Enter saves; Esc cancels. Both work even when the textarea is focused.
    const bool ctrlEnter = ctx.CtrlDown && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    const bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    const char* footerHint = s_ActiveLongTextState.RawMode
                                 ? "Ctrl+Enter to save · Esc to cancel · raw HTML mode (Markdown disabled)"
                                 : "Ctrl+Enter to save · Esc to cancel · Ctrl+P preview mode · Markdown — "
                                   "**bold**, *em*, # heading, - list, ```code```";
    ImGui::TextDisabled("%s", footerHint);

    bool save = ctrlEnter;
    bool cancel = escPressed;
    if (ImGui::Button("Save", ImVec2(100, 0))) {
        save = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        cancel = true;
    }
    // Three-way segmented control: Edit / Split / Preview. Disabled in RawMode. Ctrl+P cycles modes.
    ImGui::SameLine();
    if (s_ActiveLongTextState.RawMode)
        ImGui::BeginDisabled();
    auto segBtn = [](const char* label, ActiveLongTextEditorState::PreviewModeT mode) {
        const bool active = s_ActiveLongTextState.PreviewMode == mode;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label, ImVec2(80, 0))) {
            s_ActiveLongTextState.PreviewMode = mode;
        }
        if (active) {
            ImGui::PopStyleColor();
        }
    };
    segBtn("Edit", ActiveLongTextEditorState::PreviewModeT::EditOnly);
    ImGui::SameLine(0.0f, 2.0f);
    segBtn("Split", ActiveLongTextEditorState::PreviewModeT::Split);
    ImGui::SameLine(0.0f, 2.0f);
    segBtn("Preview", ActiveLongTextEditorState::PreviewModeT::PreviewOnly);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Ctrl+P to cycle Edit / Split / Preview");
    }
    if (s_ActiveLongTextState.RawMode)
        ImGui::EndDisabled();

    if (save) {
        CommitLongTextEdit(pendingEdits);
    } else if (cancel) {
        ImGui::CloseCurrentPopup();
        CloseLongTextEditor();
    }
}

// Copies `seed` into Buffer, truncating to kBufferSize - 1 if needed, and records exactly
// what was written (BufferSeedShown) plus whether truncation occurred (SeedTruncated). Buffer
// must already be sized (assign'd) by the caller before this runs.
void SeedLongTextBuffer(const std::string& seed) {
    const size_t copyLen = (std::min)(seed.size(), ActiveLongTextEditorState::kBufferSize - 1);
    std::memcpy(s_ActiveLongTextState.Buffer.data(), seed.data(), copyLen);
    s_ActiveLongTextState.Buffer[copyLen] = '\0';
    s_ActiveLongTextState.SeedTruncated = seed.size() > copyLen;
    s_ActiveLongTextState.BufferSeedShown.assign(seed.data(), copyLen);
}

} // namespace

void OpenLongTextEditor(AppController& app, const std::string& issueId, const TrackerField& field,
                        const std::string& label, const std::string& currentStrippedValue,
                        const std::string& currentRichValue) {
    s_ActiveLongTextState.IssueId = issueId;
    s_ActiveLongTextState.Field = field;
    s_ActiveLongTextState.FieldLabel = label.empty() ? field.Id : label;
    s_ActiveLongTextState.OriginalStrippedValue = currentStrippedValue;
    s_ActiveLongTextState.OriginalRichValue = currentRichValue;
    s_ActiveLongTextState.RichKind = ClassifyRichValue(currentRichValue);
    s_ActiveLongTextState.RawMode = false;
    // Seed the default view mode from the persisted preference. Preview opens the
    // rendered pane; Edit (default) opens the text editor. Ctrl+P still cycles
    // Edit/Split/Preview at runtime. RawMode (set later for non-round-trippable
    // HTML) forces EditOnly regardless via effectiveMode.
    s_ActiveLongTextState.PreviewMode = g_ui.cfg.DefaultLongTextEditorPreview
                                            ? ActiveLongTextEditorState::PreviewModeT::PreviewOnly
                                            : ActiveLongTextEditorState::PreviewModeT::EditOnly;
    s_ActiveLongTextState.DroppedAdfNodeTypes.clear();
    s_ActiveLongTextState.LoadingMarkdown = false;
    ++s_ActiveLongTextState.LoadGen;

    // Large rich values are parsed on a worker to keep the UI thread responsive; small ones stay inline.
    const bool deferSeed = currentRichValue.size() > kAsyncRichSeedThresholdBytes &&
                           (s_ActiveLongTextState.RichKind == LongTextRichKind::Adf ||
                            s_ActiveLongTextState.RichKind == LongTextRichKind::Html);

    if (!deferSeed) {
        std::string seed =
            ComputeLongTextSeed(s_ActiveLongTextState.RichKind, currentRichValue, currentStrippedValue,
                                s_ActiveLongTextState.DroppedAdfNodeTypes, s_ActiveLongTextState.RawMode);
        s_ActiveLongTextState.OriginalMarkdown = seed;
        s_ActiveLongTextState.Buffer.assign(ActiveLongTextEditorState::kBufferSize, '\0');
        SeedLongTextBuffer(seed);
    } else {
        // Show a placeholder so the modal renders immediately; worker replaces it on post-back.
        const std::string placeholder = "Loading description...";
        s_ActiveLongTextState.OriginalMarkdown.clear();
        s_ActiveLongTextState.Buffer.assign(ActiveLongTextEditorState::kBufferSize, '\0');
        SeedLongTextBuffer(placeholder);
        s_ActiveLongTextState.LoadingMarkdown = true;
        const int capturedGen = s_ActiveLongTextState.LoadGen;
        const std::string capturedIssueId = issueId;
        const LongTextRichKind capturedKind = s_ActiveLongTextState.RichKind;
        const std::string capturedRich = currentRichValue;
        const std::string capturedStripped = currentStrippedValue;
        app.LaunchBackgroundTask([&app, capturedGen, capturedIssueId, capturedKind, capturedRich, capturedStripped]() {
            std::vector<std::string> droppedNodes;
            bool rawMode = false;
            std::string seed = ComputeLongTextSeed(capturedKind, capturedRich, capturedStripped, droppedNodes, rawMode);
            app.mainThreadDispatcher.PostToMainThread([capturedGen, capturedIssueId, seed = std::move(seed),
                                                       droppedNodes = std::move(droppedNodes), rawMode]() mutable {
                // Discard if user opened a different editor since dispatch — LoadGen increases monotonically.
                if (!s_ActiveLongTextState.Active || s_ActiveLongTextState.LoadGen != capturedGen ||
                    s_ActiveLongTextState.IssueId != capturedIssueId) {
                    return;
                }
                s_ActiveLongTextState.OriginalMarkdown = seed;
                s_ActiveLongTextState.DroppedAdfNodeTypes = std::move(droppedNodes);
                s_ActiveLongTextState.RawMode = rawMode;
                s_ActiveLongTextState.Buffer.assign(ActiveLongTextEditorState::kBufferSize, '\0');
                SeedLongTextBuffer(seed);
                s_ActiveLongTextState.LoadingMarkdown = false;
            });
        });
    }
    s_ActiveLongTextState.Active = true;
    s_ActiveLongTextState.JustOpened = true;

    // Wire the editor buffer into the dictation router so transcribed text splices into the
    // focused long-text editor. Unregistered in CloseLongTextEditor; re-registering on subsequent
    // Opens picks up the freshly-resized Buffer.
    if (!s_ActiveLongTextState.Buffer.empty()) {
        g_dictationRouter.RegisterInputText(s_ActiveLongTextState.Buffer.data(), ActiveLongTextEditorState::kBufferSize,
                                            nullptr);
    }
}

void CloseLongTextEditor() {
    if (!s_ActiveLongTextState.Buffer.empty()) {
        g_dictationRouter.UnregisterInputText(s_ActiveLongTextState.Buffer.data());
    }
    s_ActiveLongTextState = ActiveLongTextEditorState{};
}

void TicketFieldEditor::RenderLongTextModal(std::vector<PendingFieldEdit>& pendingEdits) {
    if (!s_ActiveLongTextState.Active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (s_ActiveLongTextState.JustOpened) {
        // OpenPopup must be called at the same window-depth as BeginPopupModal. Both live here,
        // outside the table, so ImGui can correctly anchor the popup to this window.
        ImGui::OpenPopup(kLongTextModalPopupId);
        const ImVec2 modalSize(viewport->Size.x * 0.6f, viewport->Size.y * 0.65f);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    // The title-bar close (X) is driven by p_open, the same affordance as every other window. When
    // the user clicks it ImGui clears the open flag and auto-dismisses the popup; the dismissed-
    // without-intervention branch below then runs the editor-close path, giving the X the same
    // discard semantics as Esc and the Cancel button.
    bool modalOpen = true;
    if (ImGui::BeginPopupModal(kLongTextModalPopupId, &modalOpen,
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        DrawLongTextBanners();

        // Reserve room for the footer (status line + button row).
        const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 inputSize(-FLT_MIN, ImGui::GetContentRegionAvail().y - footerH);

        if (s_ActiveLongTextState.JustOpened) {
            ImGui::SetKeyboardFocusHere();
            s_ActiveLongTextState.JustOpened = false;
        }

        // Modifier state — declared up here so the Ctrl+P preview-mode shortcut sees it
        // before we render the panes. Ctrl+Enter / Esc are checked again below for save/cancel.
        const bool ctrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        HandleLongTextPreviewModeHotkey(ctrlDown);

        // Preview is disabled in raw-HTML mode — force EditOnly there regardless of the user's choice.
        using PreviewModeT = ActiveLongTextEditorState::PreviewModeT;
        LongTextModalCtx ctx;
        ctx.EffectiveMode = s_ActiveLongTextState.RawMode ? PreviewModeT::EditOnly : s_ActiveLongTextState.PreviewMode;
        ctx.CtrlDown = ctrlDown;
        ctx.InputH = inputSize.y;

        // Pane layout for Split mode: [editor] [splitter] [preview].
        // EditOnly / PreviewOnly: visible pane gets -FLT_MIN (fills the popup).
        // Split: explicit pixel widths so the splitter sits between two real columns.
        const float totalW = ImGui::GetContentRegionAvail().x;
        ctx.PanesW = (ctx.EffectiveMode == PreviewModeT::Split) ? (totalW - ctx.SplitterW) : totalW;
        const float splitEditorW = ctx.PanesW * s_ActiveLongTextState.SplitterRatio;
        const float splitPreviewW = ctx.PanesW - splitEditorW;
        ctx.EditorW = (ctx.EffectiveMode == PreviewModeT::Split) ? splitEditorW : -FLT_MIN;
        ctx.PreviewW = (ctx.EffectiveMode == PreviewModeT::Split) ? splitPreviewW : -FLT_MIN;
        ctx.EditorId = ImGui::GetID("##LongTextEditorBuf");

        if (ctx.EffectiveMode != PreviewModeT::PreviewOnly) {
            DrawLongTextEditorPane(ctx);
        }
        if (ctx.EffectiveMode == PreviewModeT::Split) {
            DrawLongTextSplitter(ctx);
        }
        if (ctx.EffectiveMode != PreviewModeT::EditOnly) {
            DrawLongTextPreviewPane(ctx);
        }

        DrawLongTextFooter(ctx, pendingEdits);

        ImGui::EndPopup();
    } else {
        // Modal dismissed without our intervention — stay self-consistent.
        CloseLongTextEditor();
    }
}
