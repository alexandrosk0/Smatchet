- 2026-08-18 · claude-code · [test] · P2 — the About modal has **no** bucket-C golden and **no** bucket-E case, so both About fixes in [PR #2111](https://github.com/alexandrosk0/Smatchet/pull/2111) shipped on a manual eye-check

  `Source/Core/src/Ui/SmatchetAboutUi.cpp` is invisible to automated verification:
  `tests/golden/` holds only code-syntax-coloring, command-palette-fuzzy, dock-gap-sentinel and
  the four user-info PNGs, and no `tests/ui/*.test.cpp` opens the modal. That is what fired the
  ship-loop visual-validation exception (`docs/agent-rules/ship-loops.md` § exception 5) and put a
  human in the loop for a two-line guard change.

  Two fixes landed with zero CI coverage:

  1. **#2093 — the popup-open guard.** The dismissal check called
     `IsPopupOpen(const char*, ImGuiPopupFlags_AnyPopupLevel)`, which `imgui.cpp` hard-asserts on;
     it now hashes via `::ImGui::GetID()`. A bucket-E case that opens Help → About, steps a frame
     and asserts the popup is still open would have caught both the assert **and** the
     self-dismiss failure mode, and would pin the `ImHashStr` `###`-reset behaviour the fix
     depends on — exactly the kind of upstream-internal assumption a vendored-ImGui bump breaks.
  2. **#2066 — the translated `unknown` placeholder.** A bucket-A test pins the dictionary row,
     but nothing asserts the modal actually *renders* it for an empty field value.

  Shape: one bucket-E case — open the About modal via the menu path, `ImGuiTestEngine` step,
  assert `IsPopupOpen` true after N frames, then assert the placeholder string appears in the
  item list with a field forced empty. That single case retires the manual step for every future
  About change, not just these two.
