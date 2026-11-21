- 2026-11-17 · input-ux-w2-text-field · [tooling] · P2 — The bucket-E lane has ~2 s of wall-clock headroom, folds a timeout into "transient collapse", and retries on the killed attempt's profile

Details:
`.github/workflows/build-and-test.yml` (Bucket-E UI tests) runs `ui_test.run --all --frames=5400 --timeout=<ms> --spawn`. Three things made PR #2438 (14 new UI tests) red for a reason none of its tests showed:

1. **Headroom.** Develop's last green run took 157.9 s of the 160 s limit (243 tests, ~33 ms per frame). Any PR that adds ~60 frames of UI test crosses it. Nothing reports elapsed time against the limit on green runs, so the erosion was invisible until a PR tripped it.
2. **A timeout is read as a collapse.** On `{"code":"timeout"}` the parent prints no `data.passed` / `data.failed`; the lane's parse-miss path sets `status=broken passed=0 failed=1` and retries, the same way as the first-frame render collapse it was written for. The deadline's quit aborts whichever test is running, so each attempt shows a different "failing" test.
3. **Retries are not independent.** The spawned app reads and writes `%LOCALAPPDATA%\Smatchet\imgui.ini` and `smatchet_config.json`; the loop does not reset them, so attempt 2 boots on the layout saved by the killed attempt 1. Attempts 2 and 3 of PR #2438 failed `PrefsSearchFilter/*` and `WindowExpand/*` (windows not docked), which pass on develop. Unproven: the saved layout is the best candidate.

PR #2438 raised `--timeout` to 230000 (the documented knob). The deeper fixes are not in that PR. It recurred on 2026-11-21 (develop run 44917896468: 338 tests, ~233 s, all three attempts timed out) and #2470 raised it to 300000, the third raise; item (a), an elapsed-vs-deadline warning on green runs, would have flagged the 206-224 s PR runs first.

Concrete next action:
In the bucket-E step: (a) print elapsed time against `--timeout` on every attempt and warn above ~85 %; (b) report `error.code == "timeout"` as its own status instead of `broken`, and do not retry it blindly; (c) delete `imgui.ini` and the config file from the throwaway profile before each attempt (the local `scripts/dev/lib/ui-test-driver.sh` already uses a throwaway profile). Cover (b) and (c) in `tests/bats/` for the lane script if it is extracted, or with a workflow-lint check.

Status: open
Last-reviewed: 2026-11-21
