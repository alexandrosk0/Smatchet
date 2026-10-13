# Tunable limits (`config.json` → `"limits"`)

Caps, timeouts and cache sizes that used to be hardcoded `constexpr` literals are user-tunable. They live under
the top-level `"limits"` object in `config.json` and are written in full on every Save, so every key is
discoverable by opening the file. No Preferences UI — edit the file and restart (`Load()` caches the parsed config, so a hand edit is picked up on next launch).

- **Single source of truth:** [`Source/Core/include/Config/TunableLimits.h`](../../Source/Core/include/Config/TunableLimits.h).
  One `SMATCHET_LIMITS_TABLE` row per limit gives the struct field, JSON key, default, and clamp range.
- **Load behaviour:** missing key → default; non-integer value → default (siblings unaffected); out-of-range
  value → clamped to the row's `[min, max]`.
- **Publishing:** `ConfigManager::Load()` calls `smatchet::limits::Set(cfg.Limits)`. Code reads values through
  `smatchet::limits::Get().<Field>` (thread-safe by-value snapshot; read once before a loop, not per iteration).
- **Units:** `*Ms` milliseconds, `*MiB` mebibytes (`limits::MiB()` converts to bytes), everything else a count.

## Adding a limit

1. Append one `X(Field, "json_key", default, min, max)` row to `SMATCHET_LIMITS_TABLE`.
2. Replace the hardcoded constant with `smatchet::limits::Get().Field`.

The defaults/clamp unit test (`tests/Core/ConfigManager.test.cpp`) covers the new row automatically. Keys are
persisted: never rename or renumber an existing one.

## What is deliberately not a limit

Recursion-depth guards, parser buffer ceilings, log-string truncation, wire-protocol page sizes (`kPageSize`),
and audio sample rates are safety bounds or protocol facts, not preferences, and stay `constexpr`.

## Behaviour notes

- Search result-count caps (`jira_search_max_results`, `plane_search_max_results`, `github_search_max_results`)
  default to **500** (previously 10000 / 10000 / 5000). The cap is a soft bound rounded up to a whole page
  (100 rows): a search with more rows stops after the page that crosses the cap and the fetch summary carries
  the truncation warning, while a walk that finishes its last page is never reported as truncated. Raise the
  key to fetch more.
- `github_search_max_pages` is clamped to 10: GitHub's search API returns at most 1000 results.
