# Field-icon render keeps its own InFlightClaim next to develop's LaunchClaim

- **Category**: debt
- **Priority**: P3
- **Date**: 2026-11-28
- **Status**: open

## What happened

`Source/Core/src/Ui/SmatchetFieldIconRender.cpp` owns a file-local `InFlightClaim` (one atomic release on destruction or explicitly; covers a throwing launch, a dropped-unrun task and normal completion). develop later added `smatchet::LaunchClaim` (`Source/Core/include/LaunchClaim.h`) and `IAppThreading::LaunchBackgroundTask` now tells callers to own an in-flight latch through it. Both do the same job, so there are two primitives for one contract. The local one is correct (all four paths traced in review); it just predates the shared one.

## Concrete next action

Move `LaunchIconResolve` and the synchronous revalidate path to `LaunchClaim` (`TakeForRun` in the task body, `ReleaseIfUntaken` from the launch catch), keep the existing in-flight tests green, and delete `InFlightClaim`.
