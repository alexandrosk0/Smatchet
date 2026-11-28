# Field-icon bucket-E test (priority_icon_bad_bytes) deferred; LaunchIconResolve has no UI-side test

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-28
- **Observed on**: the FI (file, icon and attachment caches) workstream of `docs/plans/active/cache-hardening.md`, FI-5 slice.
- **Status**: open

## What happened

The plan's Verification section lists a bucket-E test `tests/ui/priority_icon_bad_bytes.test.cpp` (one launch, a bad cached icon file removed, the fallback drawn, the negative memo expiring after a faked 5 minutes). It was not written: the Linux build environment has no GLFW/X11 and no UI test engine, so it could not be run or even linked. The disk-cache half (`SmatchetFieldIconDiskCache`) and the pure rules are covered by doctest cases that do run.

`SmatchetFieldIconRender::SetIconFetchFnForTest` was added for that test and has no caller yet. The UI-side logic in `LaunchIconResolve` (mark before launch, release exactly once, a throwing launch, one completion post, memo expiry) therefore has no automated test.

## Concrete next action

Write the bucket-E test (or a rig doctest that drives `LaunchIconResolve` through the hook with a fake fetch and a fake clock), run it where the full rig builds, and give `SetIconFetchFnForTest` its first caller.
