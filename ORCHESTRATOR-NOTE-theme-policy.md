# Orchestrator policy update (user-mandated, 2026-06-07)

**All golden images and screenshot captures must use the DEFAULT ImGui theme/style — never `SmatchetTheme::ApplyStyle`.**

Rationale: goldens pinned to the custom theme are invalidated by every palette retune (the trivial-visual envelope makes those frequent); stock `ImGui::StyleColorsDark()` is stable upstream.

For your Slice-2 work:
1. If your bucket-E smoke or any screenshot capture path applies the Smatchet theme at boot, add a deterministic opt-out (e.g. env `SMATCHET_TEST_DEFAULT_IMGUI_THEME=1` checked where `ApplyStyle` is called at bootstrap) and set it in the test driver. Keep the knob tiny + documented at the call site.
2. Any golden you stage for approval must be captured under the default theme.
3. Note the knob + policy in your report so the orchestrator can codify it in `docs/agent-rules/golden-image-approval.md`.
