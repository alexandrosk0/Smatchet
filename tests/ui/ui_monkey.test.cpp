// ui_monkey.test.cpp — Layer 2 of the nightly "code monkey"
// (docs/plans/nightly-monkey-tester.md): a random-INPUT UI monkey.
//
// It boots the REAL app (via UiTestScenario, same as funcsize_main_ui_smoke.test.cpp)
// and fires a long SEEDED stream of random keyboard/mouse INPUT events at the live ImGui
// UI — the classic "monkey at a keyboard" model (cf. Android's Monkey). It asserts
// NOTHING with IM_CHECK: the only failure mode is a real crash / in-frame IM_ASSERT that
// the ImGui Test Engine traps. The value is exactly the stochastic input the scripted UI
// tests never generate.
//
// SAFETY / OPT-IN: registration is gated behind the env var SMATCHET_UI_MONKEY=1, so this
// test is INERT under the normal `ui_test.run --all` (the required Bucket-E lane never
// sets it) and cannot destabilize existing CI. It is exercised only by an advisory step
// that sets the var and runs `ui_test.run --name="UiMonkey"` (the CATEGORY, substring-
// matched — the engine filter is never a glob; Issue #1666). Determinism: the seed
// (env SMATCHET_UI_MONKEY_SEED, else a fixed default) is logged up front via
// ctx->LogInfo("ui-monkey seed=..."), so a crash in the captured spawn log replays.
//
// Deliberately uses ONLY test-engine primitives already exercised across tests/ui/
// (KeyPress/KeyChars/MouseMoveToPos/MouseClick/Yield/SetRef/LogInfo) — no GatherItems —
// so it compiles against the pinned imgui_test_engine without relying on unverified API.
// It avoids modifier keys (no Ctrl/Alt/Super) so it can't trip a global quit/destructive
// shortcut, and presses Escape periodically so it never wedges inside a modal.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — the proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// std::getenv is deprecated under MSVC (C4996); the ui-test target builds warnings-as-errors
// on MSVC, so suppress locally (mirrors UiTestScenario.cpp's getenv dance).
std::string EnvOr(const char* name, const std::string& def) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char* v = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return (v != nullptr && *v != '\0') ? std::string(v) : def;
}

bool MonkeyEnabled() { return EnvOr("SMATCHET_UI_MONKEY", "") == "1"; }

std::uint32_t MonkeySeed() {
    const std::string s = EnvOr("SMATCHET_UI_MONKEY_SEED", "");
    if (!s.empty()) {
        return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    }
    return 0x5eedu; // fixed default so the walk is reproducible frame-for-frame
}

int MonkeySteps() {
    const std::string s = EnvOr("SMATCHET_UI_MONKEY_STEPS", "");
    if (!s.empty()) {
        const long n = std::strtol(s.c_str(), nullptr, 10);
        if (n > 0 && n < 1000000) {
            return static_cast<int>(n);
        }
    }
    return 1500;
}

// Modifier-free keys only — a random modifier chord could hit a global quit/destructive
// shortcut. These are pure navigation / editing keys, safe to spray at any focused widget.
ImGuiKey RandomNavKey(std::mt19937& rng) {
    static const ImGuiKey kKeys[] = {
        ImGuiKey_Tab,       ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow,
        ImGuiKey_DownArrow, ImGuiKey_Enter,     ImGuiKey_Space,      ImGuiKey_Backspace,
        ImGuiKey_Delete,    ImGuiKey_Home,      ImGuiKey_End,        ImGuiKey_PageUp,
        ImGuiKey_PageDown,  ImGuiKey_Escape,
    };
    const std::size_t n = sizeof(kKeys) / sizeof(kKeys[0]);
    return kKeys[rng() % n];
}

char RandomPrintable(std::mt19937& rng) {
    static const char kChars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_/.";
    const std::size_t n = sizeof(kChars) - 1;
    return kChars[rng() % n];
}

void RegisterRandomWalk(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "UiMonkey", "RandomWalk");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        const std::uint32_t seed = MonkeySeed();
        const int steps = MonkeySteps();
        ctx->LogInfo("ui-monkey seed=%u steps=%d — random-input walk (no assertions; crash-only failure)",
                     static_cast<unsigned>(seed), steps);

        // Wait for the app to settle (main menu bar draws every frame) before spraying input.
        const bool barLive = YieldUntil(ctx, [&] { return WindowIsLive("##MainMenuBar"); });
        if (!barLive) {
            ctx->LogWarning("ui-monkey: main UI never settled; skipping walk");
            return;
        }

        std::mt19937 rng(seed);
        for (int step = 0; step < steps; ++step) {
            const unsigned action = rng() % 10u;
            if (action < 4u) {
                ctx->KeyPress(RandomNavKey(rng));
            } else if (action < 6u) {
                const char s[2] = {RandomPrintable(rng), '\0'};
                ctx->KeyChars(s);
            } else if (action < 9u) {
                const ImVec2 disp = ImGui::GetIO().DisplaySize;
                if (disp.x > 1.0f && disp.y > 1.0f) {
                    const float x = static_cast<float>(rng() % static_cast<unsigned>(disp.x));
                    const float y = static_cast<float>(rng() % static_cast<unsigned>(disp.y));
                    ctx->MouseMoveToPos(ImVec2(x, y));
                    if ((rng() & 1u) != 0u) {
                        ctx->MouseClick(ImGuiMouseButton_Left);
                    }
                }
            } else {
                ctx->Yield(); // an idle frame
            }

            // Periodically escape so a random click that opened a modal/menu can't wedge the
            // walk (and so we exercise the open→dismiss paths repeatedly).
            if (step % 37 == 0) {
                ctx->KeyPress(ImGuiKey_Escape);
            }
            ctx->Yield();
        }

        // Leave the UI in a clean-ish state for any subsequent test.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield();
        ctx->LogInfo("ui-monkey: completed %d steps without a crash", steps);
    };
}

} // namespace

extern "C" void SmatchetRegisterUiMonkeyTests(ImGuiTestEngine* engine) {
    // Opt-in only: inert unless SMATCHET_UI_MONKEY=1 so the required `ui_test.run --all`
    // lane never runs the random walker. An advisory step sets the var and runs it via
    // `--name="UiMonkey"` (category substring — the engine filter is never a glob).
    if (!MonkeyEnabled()) {
        return;
    }
    RegisterRandomWalk(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
