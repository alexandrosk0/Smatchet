# Phase-0 Android — manual verification checklist

Single consolidated checklist for the Phase-0 (epic [#1018](https://github.com/alexandrosk0/Smatchet/issues/1018)) acceptance criteria that **cannot** be covered by CI today — the residue left after the automated gates (dual-target build, ctest Buckets A–E, `mobile-android-ndk` / `mobile-android-apk` / `mobile-emulator-smoke` boot+first-frame). Each check is a confirmed-manual step, not an out-of-scope deferral: the deferred-automation residue is tracked in [`docs/self-improvement/categories/tooling.md`](../self-improvement/categories/tooling.md).

> **Device-safety preconditions (read first — non-negotiable).**
> - **Input injection is EMULATOR-ONLY.** Before any `adb … input` / coordinate injection, run `adb devices` and confirm the target. **Pin `-s emulator-5554`** on every injecting `adb` command. The physical Pixel 10 Pro (`59271FDCH0007U`) may be connected — **never** coordinate-inject it.
> - **Live-ticket edits are an external-service mutation.** Any step that issues a real PUT against a real backend ticket must be **confirmed before running** and must target a disposable/scratch ticket, never production data. The *discard* paths (no-PUT) are safe to exercise freely.
> - **Never print/commit secrets.** When capturing device logs or pulling the on-device config, filter out `token|secret|authorization|bearer`. The desktop Jira token is DPAPI-encrypted (`token_enc`); on Android today it is **plaintext** (`token` key — Phase-1 P1.0 closes this). `chmod 600` any pulled config; never write a token-bearing file under a tracked path.
> - adb path note (MSYS): prefix device-path adb commands with `MSYS_NO_PATHCONV=1`; local pull/save destinations must be Windows-form.

---

## VC-12 — EGL context-loss recreate (item 12)

**Precondition:** debug APK installed on the emulator; app at the grid with a view loaded.

**Steps:**
1. Background the app (Home), open 3–4 other GPU apps to force the framework to reclaim the surface, then foreground Smatchet. (Or, on a device that supports it, toggle "Don't keep activities" in Developer Options and rotate.)
2. Watch logcat for the surface-destroyed → surface-created transition.

**Expected:** the EGL context is recreated, textures (font atlas + any attachment thumbnails) are re-uploaded, and the **first frame after recreate renders correctly** — no black screen, no crash, no leaked context. The boot-error panel does **not** appear.

**Pass / fail:** ☐ pass ☐ fail — notes: ____________________

---

## VC-14 — SAVE_STATE / restore across process death (item 14)

**Precondition:** app at the grid with a view loaded; Developer Options → "Don't keep activities" ON (forces `onSaveInstanceState` → process death → restore).

**Steps:**
1. Note the current view / scroll position.
2. Background the app; confirm logcat shows the activity destroyed (process death).
3. Foreground the app.

**Expected:** the app restores to a usable state (re-boots Core, re-loads the data dir at `filesDir`, no crash). The on-device SQLite cache (`tickets_v2`) survives — the previously-fetched tickets are present without a fresh network fetch. No data corruption in the config or cache DB.

**Pass / fail:** ☐ pass ☐ fail — notes: ____________________

---

## VC-15 — In-place density rebuild within Pillar-2 budget (item 15)

**Precondition:** app running; ability to change the system font scale / display size (Settings → Display → Font size) or rotate the device.

**Steps:**
1. With the app foregrounded, change the system font scale one notch (or rotate portrait↔landscape).
2. Observe the UI rebuild and time the stall (instrument with the existing `SMATCHET_UI_PERF_SCOPE` on the rebuild path, or eyeball for a visible freeze).

**Expected:** the theme + font atlas rebuild **in place** (no app restart), the density scale (`ApplyUiDensityScale`) re-applies, and the rebuild stall is **< 100 ms** (UX Pillar 2 — no UI-thread block > 100 ms without a visible cue). Text re-renders at the new scale; no clipped/overlapping layout.

**Pass / fail:** ☐ pass ☐ fail — measured rebuild ms: ______  notes: ____________________

---

## VC-PERF — Real-hardware steady-state perf (UX Pillar 1)

**Precondition:** debug (or, better, release) APK on **real hardware** (the Pixel 10 Pro — observation only, no injection); a view with a non-trivial row count loaded.

**Steps:**
1. Scroll the grid continuously for ~10 s.
2. Capture per-frame UI work via the in-app perf snapshot (`perf.snapshot` over the CLI/MCP bridge if wired, or the on-screen frame-time overlay).

**Expected:** steady-state UI work **≤ 6.94 ms** (144 Hz target); **p99 ≤ 10.0 ms** (100 Hz floor). No sustained hitch during scroll. (Emulator timings do **not** count for this check — real hardware only.)

**Pass / fail:** ☐ pass ☐ fail — steady-state ms: ______  p99 ms: ______  notes: ____________

---

## VC-16 — Mobile explicit-commit: no stray PUT on discard (item 16)

The PR-6 contract (`kMobileInlineEditBuild`): an inline single-line text edit commits **only** on explicit submit (Enter / IME "Done"); **every** focus-loss (Back, tap-away, IME dismiss) cancels with **no PUT**.

**Precondition:** EMULATOR ONLY (`-s emulator-5554`); a real backend configured with a **disposable scratch ticket**; logcat capturing (secret-filtered); a way to observe outbound PUTs (proxy log, backend audit, or the `BackendAuditTrail` entries).

**Discard matrix — each row must issue ZERO PUT:**
| # | Action | Expected |
|---|---|---|
| 1 | Open a text-cell edit, type a change, press **Back** | edit discarded, **no PUT** |
| 2 | Open a text-cell edit, type a change, **tap away** (tap another cell / empty area) | edit discarded, **no PUT** |
| 3 | Open a text-cell edit, type a change, **dismiss the IME** (swipe-down keyboard) | edit discarded, **no PUT** |

**Commit path (external-service mutation — CONFIRM BEFORE RUNNING; scratch ticket only):**
| # | Action | Expected |
|---|---|---|
| 4 | Open a text-cell edit, type a change, press Enter / IME **Done** | **exactly one PUT**; the scratch ticket field updates |

**Steps:**
1. `adb devices` → confirm `emulator-5554` is the target; never the physical device.
2. Run rows 1–3 (safe — no mutation). Confirm zero PUTs in the audit/proxy log for each.
3. Confirm the commit path (row 4) **only after** explicit confirmation to mutate the scratch ticket.

**Expected:** rows 1–3 produce **no** outbound PUT (the stray-PUT bug is absent); row 4 produces exactly one. Per the broad "no implicit commit on mobile" decision, the four combo/modal editors (Labels / Cascading / MultiSelect / DateTime) already require an explicit in-popup select/Apply and are **not** part of the PR-6 text-editor policy — their touch Save/Cancel affordance is Phase-1 P1.3, not verified here.

**Pass / fail:** ☐ row1 ☐ row2 ☐ row3 ☐ row4 — notes: ____________________

---

## Sign-off

| Check | Result | Date | Notes |
|---|---|---|---|
| VC-12 EGL recreate | ☐ | | |
| VC-14 save/restore | ☐ | | |
| VC-15 density rebuild < 100 ms | ☐ | | |
| VC-PERF real-hw ≤ 6.94 ms / p99 ≤ 10 ms | ☐ | | |
| VC-16 no stray PUT (rows 1–4) | ☐ | | |

When all five pass on real hardware + emulator as specified, Phase-0 manual acceptance is complete. Anything that fails routes back to the owning subsystem specialist (render → `unreal-bridge`/`ui-host`; perf → `perf-detective`; commit policy → `grid-engine`).
