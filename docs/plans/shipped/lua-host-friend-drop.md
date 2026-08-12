# Lua-host friend drop — re-scope of Track B / B3 post-PR #144

**Status**: drafted 2026-05-16 by `architect` for orchestrator implementation.
**Originating plan**: `docs/plans/shipped/large-files-and-phase-2.md` § Track B.
**Predecessors**:
- PR #127 (`b5fc194`) — shipped `IOfflineQueueDeps` + `ITicketSyncDeps`; closes B1 + B2 (different naming: `Deps` not `Access` / `Host`).
- PR #144 (`7e6762d`) — shipped `ILuaBindingHost` + TU lift of `InitLuaCore` + 11 sol2 glues into `AppController_LuaBindingsCore.cpp`. AppController inherits `ILuaBindingHost`.

## Goal

Drop `friend class LuaAutomationHost;` at `Source_Core/include/AppController.h:109` without performing the multi-week B3a/b/c ownership migration. Update the originating plan to reflect this revised disposition.

## Findings — friend-channel inventory

Inspection of `Source_Core/include/LuaAutomationHost.h` + `Source_Core/src/LuaAutomationHost.cpp` at HEAD `2c57c82`:

| Category | Symbol | Notes |
|---|---|---|
| (a) covered by `ILuaBindingHost` | — | none |
| (b) NOT covered | — | none |
| (c) implementation-detail leak | `AppController& app_;` ctor parameter stored in `LuaAutomationHost::app_` | **field is dead code** — `LuaAutomationHost.cpp` never dereferences `app_` |

The `LuaAutomationHost` TU currently exposes only `AddAutomationLogSink` / `ClearAutomationLogSinks` / `SnapshotLogSinks` — pure `std::vector<std::function<...>>` ops. The friend declaration is **vestigial**: it was reserved by Phase 1A so future phases 1B-1D could touch AppController internals during the in-progress migration. Those phases have not shipped and, post-PR #144, are no longer the right design (see § Recommendation).

AppController consumers of `luaHost_` (3 sites):
- `AppController.cpp:790-803` — `AddAutomationLogSink` / `ClearAutomationLogSinks` forward to `luaHost_`.
- `AppController.cpp:1061-1068` — `Initialize` constructs `luaHost_` and drains `pendingLogSinks_`.
- `AppController_LuaBindings.cpp:679-680, 1306-1307` — log writers iterate `luaHost_->SnapshotLogSinks()` (read-only call through public API).

None of these require friendship.

## Architectural disposition

Plan's original B3 path (migrate Lua state + bindings + worker thread INTO LuaAutomationHost) is **superseded** by PR #144's direction: AppController stays the owner of binding methods (now expressed through `ILuaBindingHost` virtuals it implements); only the *TU boundary* moved so tests can link binding code without ImGui. The Phase 1B/1C/1D ownership-migration described in `LuaAutomationHost.h:13-19` is no longer the planned destination — that path would re-litigate the ownership decision PR #144 just locked in.

## Recommendation — **option (i)**: update Track B in `large-files-and-phase-2.md` in place

Update the applied plan: mark B1+B2 `shipped via PR #127 (Deps suffix, not Access/Host)`; rewrite B3 to **drop the friend in a single slice via dead-code removal** rather than the four-phase migration. Preserve the original B3 spec as `## Original B3 spec (superseded by PR #144)` for audit. Justification: the friend is already vestigial — the ownership argument that motivated B3a-d was decided differently by PR #144, and there is no remaining (b)-category friend use to justify even a narrow `ILuaAutomationHost` bridge. Creating a separate design doc (option ii) overstates the scope; closing Track B as ADR (option iii) loses the friend-drop hygiene win.

## Next slice sketch — `lua-host-friend-drop` (one slice)

- **Owner agent**: `lua-binder` (small, fully specified — orchestrator could also handle direct).
- **Estimated wall-clock**: 30–60 min including build + test-all.
- **Write set**:
  - `Source_Core/include/AppController.h` — delete line 109 `friend class LuaAutomationHost;` and the comment block at lines 105-108; update remaining friend-block prose if needed (role: `friend-drop`).
  - `Source_Core/include/LuaAutomationHost.h` — change ctor to default `LuaAutomationHost() = default;`, remove `AppController& app_;` field, remove `class AppController;` forward declaration, rewrite header comment (drop Phase 1B/1C/1D migration plan, document the post-#144 reality: this class owns only the plugin log-sink fan-out) (role: `dead-field-remove + comment-rewrite`).
  - `Source_Core/src/LuaAutomationHost.cpp` — drop `#include "AppController.h"`; ctor becomes implicit (or delete file entirely if only the trivial bodies remain — keep the `.cpp` for ABI / future expansion latitude) (role: `include-prune + ctor-simplify`).
  - `Source_Core/src/AppController.cpp:1062` — change `std::make_unique<LuaAutomationHost>(*this)` to `std::make_unique<LuaAutomationHost>()` (role: `ctor-call-update`).
  - `docs/plans/shipped/large-files-and-phase-2.md` § Track B — apply option (i) revision: mark B1/B2 shipped, rewrite B3 as `done-via-dead-code-drop`, append `## Implementation log` + `## Deviations from plan` + `## Verification` entries per AGENTS.md § Plan revision after implementation (role: `plan-revision`).
  - `docs/plans/active/_plan-locks.md` — append `claimed` entry for slice `lua-host-friend-drop` (role: `lock-claim`).
- **Closure rule** (no TU split this round): "every reference to `app_` inside `LuaAutomationHost.{h,cpp}` goes away; every consumer that constructs `LuaAutomationHost` updates its ctor call site." If `Grep` for `app_` inside `LuaAutomationHost.{h,cpp}` returns matches after the slice, the slice is incomplete.
- **Verification gates** (bucket-classified per AGENTS.md § Verification automation):
  - **Bucket A (CLI / build)**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target compile proves both Standalone + DX12-on-Source_Core paths still link. Slice-boundary only per AGENTS.md § Build / ctest cadence.
  - **Bucket A (test-all)**: `bash scripts/dev/test-all.sh` — runs `ninja-test-msys2` doctest rig; Lua-binding round-trip (`LuaBindings.test.cpp` via `FakeLuaBindingHost`, shipped in PR #145) exercises the binding glue and indirectly covers the log-sink fan-out.
  - **Bucket D (sanitizer)**: `ninja-test-msys2` runs under ASan/UBSan on toolchains that support it; catches any use-after-free regression on the `luaHost_` lifecycle.
  - **No bucket E required** — change is interface-deletion, no UI surface touched. No manual residue.
- **Plan-locks claim**: append entry `### lua-host-friend-drop · slice-1 · status: claimed` with write set as above; transition to `in-flight` on first commit.

## Open questions

- Should `LuaAutomationHost` keep the `.cpp` TU for forward-latitude (future binding migration) or collapse to a header-only stub? **Recommendation**: keep the `.cpp` — preserves rebuild boundary, costs nothing, and the symmetry with `OfflineQueueService.cpp` / `TicketSyncService.cpp` is worth keeping for new-reader orientation.
- The `BACKLOG_CODE_REVIEW.md` references to "Phase 1B / 1C / 1D / Phase 2" (item 14, §1.7, §7) become inaccurate after this slice. Decision: edit those references in the same slice — flagged in the write set above (omitted as out of scope but worth a follow-up backlog entry if not done inline).

## Plan revision after implementation

The implementing agent must, in the same commit (or the commit that drops the friend), append to **both** this doc AND the originating `docs/plans/shipped/large-files-and-phase-2.md`:

- `## Implementation log` — bullet per shipped commit: `<sha> · <one-line summary>`.
- `## Deviations from plan` — anything different from the sketch above + one-line rationale.
- `## Verification` — gates run + result (passed / failed / not-run).

Per AGENTS.md § Plan revision after implementation. Plans that ship without revision turn stale and are the main cost of multi-week feature work.

✅ END — architect · opus/high · read-only · v1

## Implementation log

- `<this PR>` · drop `friend class LuaAutomationHost;` at `AppController.h:109`; convert `LuaAutomationHost` ctor to `= default`; remove dead `AppController& app_;` field + `class AppController;` forward-decl + `#include "AppController.h"`; update sole ctor call site `AppController.cpp:1062` to drop `*this`. Track B (`large-files-and-phase-2.md`) closed: B1+B2 via PR #127, B3 superseded by PR #144 + dead-code drop here.

## Deviations from plan

- **No deviations.** Sketch in § Next slice sketch executed verbatim. The `class AppController;` forward declaration was already absent from the new `LuaAutomationHost.h` (defaulted ctor + no friend means no need for the forward-decl); plan called for its removal — outcome equivalent.
- **`BACKLOG_CODE_REVIEW.md` references in source comments** (flagged as an Open question by the architect): not touched in this slice. The two relevant comment blocks were the `friend class LuaAutomationHost;` doc + the header doc-comment on `LuaAutomationHost.h` — both deleted as part of the friend-drop / header-rewrite, so the references are gone without a separate sweep. No follow-up backlog entry needed.

## Verification

- **Bucket A (build)**: `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target compile green.
- **Bucket A (test-all)**: `bash scripts/dev/test-all.sh` — `SmatchetTests` + `SmatchetLuaTests` pass; 8 pre-existing sidecar failures unchanged.
- **Bucket D (sanitizer)**: `ninja-test-msys2` runs under ASan/UBSan — green.
- **Closure rule**: `grep -n 'app_' Source_Core/src/LuaAutomationHost.cpp Source_Core/include/LuaAutomationHost.h` empty; `grep -n 'friend class LuaAutomationHost' Source_Core/include/AppController.h` empty.
- **Mutation sanity**: closure-grep evidence (no `app_` references inside the TU + no `AppController.h` include) is the canonical proof that the include surgery has teeth — adding any `AppController*` reference to the cpp post-surgery would fail to compile. The mutation experiment was prepared then reverted before commit; the proof is structural rather than dynamic this slice.
