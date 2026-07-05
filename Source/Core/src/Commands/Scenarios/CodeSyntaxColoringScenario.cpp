// CodeSyntaxColoringScenario — bucket-C screenshot-diff golden for the
// multi-language code-coloring feature (docs/plans/active/code-syntax-coloring-
// and-tooltips.md). Closes the deferred tooling.md golden-image gap (P3,
// 2026-05-15): the per-theme palette round-trip is covered by bucket-A
// doctests, but the on-screen paint of DrawColoredCode had no pixel-level gate
// until now. This scenario draws its own deterministic, fully-opaque window and
// renders one DrawColoredCodeBlock per representative language — the vendored
// path (C++ delegates to DrawColoredCppText, plus Lua), the hand-rolled LDs
// (Python, Bash, JSON), and the Plain fallback. Fixed window position, size,
// and sample text make the capture reproducible frame-to-frame so the diff in
// scripts/dev/test-screenshot-diff.sh stays stable. A scenario rather than a
// one-shot CLI because the screenshot must fire after the window has rendered
// at least once and the CodeColorView cache is warm; a few warm-up frames
// guarantee both without putting timing policy in the bash driver.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "CodeColorView.h"
#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "Commands/Scenarios/ScenarioScreenshotPath.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetUiSession.h"

#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

// g_ui — unconditional extern. Defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern in
// SmatchetUiSession.h is gated, so scenarios that need the singleton re-declare
// it at file scope, matching the DockGapSentinelScenario shim.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

std::string StringArg(const nlohmann::json& args, const char* key, const std::string& fallback) {
    if (!args.contains(key))
        return fallback;
    const auto& v = args[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number())
        return std::to_string(v.get<long long>());
    return fallback;
}

int IntArg(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.contains(key))
        return fallback;
    const auto& v = args[key];
    if (v.is_number())
        return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

// One labelled code sample per language tier. Kept short + ASCII so the
// captured glyphs are font-stable across machines (the L∞ ≤ 4 tolerance
// absorbs hinting noise). Each sample is chosen to exercise the distinct
// palette buckets — keyword, string, comment, number, preprocessor/variable.
struct LangSample {
    smatchet::code_color::CodeLang lang;
    const char* tag; // langTagOrigin — drives the slice-4 badge tooltip text.
    const char* code;
};

const std::vector<LangSample>& Samples() {
    static const std::vector<LangSample> kSamples = {
        {smatchet::code_color::CodeLang::CPlusPlus, "cpp",
         "// delegate path\nint add(int a, int b) {\n    return a + b; // sum\n}"},
        {smatchet::code_color::CodeLang::Python, "python",
         "# hand-rolled LD\ndef greet(name):\n    print(\"hi\", name)\n    return True"},
        {smatchet::code_color::CodeLang::Lua, "lua", "-- vendored LD\nlocal function f(x)\n    return x * 2\nend"},
        {smatchet::code_color::CodeLang::Bash, "bash",
         "# shell vars\nfor i in 1 2 3; do\n    echo \"item ${i}\"\ndone"},
        {smatchet::code_color::CodeLang::Json, "json",
         "{\n  \"name\": \"smatchet\",\n  \"count\": 42,\n  \"ok\": true\n}"},
        {smatchet::code_color::CodeLang::Plain, "", "plain text — no language tag\nflat-orange fallback tint"},
    };
    return kSamples;
}

class CodeSyntaxColoringScenario : public IScenario {
  public:
    std::string Name() const override { return "code-syntax-coloring"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        warmupFrames_ = IntArg(args, "warmupFrames", 8);
        if (warmupFrames_ < 1) {
            warmupFrames_ = 1;
        }
        captureSize_ = ParseScenarioCaptureSize(args);
        screenshotPath_ = StringArg(args, "screenshotPath", std::string());
        if (screenshotPath_.empty()) {
            outErr = "code-syntax-coloring: screenshotPath is required";
            return;
        }
        // Confine the caller-supplied path under <userData>/screenshots/ — MCP/Lua-reachable
        // scenario, so an unconfined path is an arbitrary-file-write primitive (#1566 class).
        if (!ConfineScenarioScreenshotPathInPlace("code-syntax-coloring", screenshotPath_, outErr))
            return;

        g_ui.requestWindowWidth = captureSize_.Width;
        g_ui.requestWindowHeight = captureSize_.Height;
        g_ui.requestWindowResize = true;
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // Draw a fully-opaque window covering the framebuffer so the capture is
        // dominated by the coloured code, not the dock chrome behind it. Fixed
        // pos + size (ImGuiCond_Always) keep the layout deterministic.
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(static_cast<float>(captureSize_.Width), static_cast<float>(captureSize_.Height)), ImGuiCond_Always);
        // Fully opaque + raised above the dockspace every frame — otherwise the
        // main UI's full-viewport dockspace draws over this window and the
        // screenshot captures the app chrome instead of the code blocks.
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::SetNextWindowFocus();
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##scenario_code_syntax_coloring", nullptr, flags)) {
            const SmatchetPreviewFonts& fonts = SmatchetGetPreviewFonts();
            for (const LangSample& s : Samples()) {
                ImGui::TextDisabled("%s", smatchet::code_color::CanonicalName(s.lang));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
                ImGui::BeginChild(s.tag[0] ? s.tag : "plain", ImVec2(-FLT_MIN, 132.0f), true);
                if (fonts.Mono)
                    ImGui::PushFont(fonts.Mono);
                smatchet::code_color::DrawColoredCodeBlock(s.code, s.lang, s.tag);
                if (fonts.Mono)
                    ImGui::PopFont();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
        }
        ImGui::End();
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= warmupFrames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        // Trigger the screenshot after the warm-up frames have rendered +
        // the colour cache is warm. Source/Standalone/main.cpp's post-swap
        // handler consumes the request and writes the PNG.
        g_ui.requestScreenshotPath = screenshotPath_;
        g_ui.requestScreenshot = true;

        nlohmann::json out;
        out["scenario"] = Name();
        out["warmupFrames"] = warmupFrames_;
        out["windowWidth"] = captureSize_.Width;
        out["windowHeight"] = captureSize_.Height;
        out["screenshotPath"] = screenshotPath_;
        out["sampleCount"] = static_cast<int>(Samples().size());
        out["captureRequested"] = true;
        return out;
    }

  private:
    int warmupFrames_ = 8;
    ScenarioCaptureSize captureSize_;
    std::string screenshotPath_;
};

} // namespace

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCodeSyntaxColoringScenario() {
    return std::make_unique<smatchet::cmd::CodeSyntaxColoringScenario>();
}
