# Agent self-improvement — security

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-17 · code-review · [security] · P2 — `tests/support/GoldenImage.h` `std::strtol` parses PPM `w`, `h`, `maxv` without overflow / negative checks (now resolved by PNG migration but the new stb-based reader still warrants a cap)
  Details: Original PPM-P6 parser had no dim caps. The PNG migration (2026-05-17) replaced that reader with stb_image, which has its own `STBI_MAX_DIMENSIONS` (1<<24) plus an additional `kMaxGoldenImageDim = 16384` cap in `GoldenImage.h`. Entry kept open because (a) the cap should also be propagated to the Standalone screenshot writer (currently bounded only by GPU framebuffer size) and (b) a fuzz test against crafted PNG dims is still missing.
  Concrete next action: add a fuzz test in `tests/Source_Core/GoldenImage.test.cpp` (new file) covering crafted PNG dims at / above 16384 and verify the cap rejects them. Estimated cost 30 min once a synthetic crafted-PNG fixture is in place.
  Status: open
  Last-reviewed: 2026-05-17
