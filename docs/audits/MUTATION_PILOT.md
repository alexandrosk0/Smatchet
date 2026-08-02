# Smatchet — Mutation-Testing Pilot

**Date:** 2026-07-05
**Branch:** `claude/mutation-testing-pilot-0s8ez2`
**Companions:** [`TEST_COVERAGE_GAP_MAP.md`](TEST_COVERAGE_GAP_MAP.md) (2026-07-02), [`CPP_CODE_AUDIT.md`](CPP_CODE_AUDIT.md) (2026-07-01), [`docs/plans/active/testing-surface-roadmap.md`](../plans/active/testing-surface-roadmap.md) (Slice **F**).

## Why this pilot

`TEST_COVERAGE_GAP_MAP.md` measures which TUs a test can **reach** (137/297 compiled into a test target). It says nothing about whether the tests that reach a TU would **catch a bug** in it — the map's own caveat: `ConfigManager.cpp` is compiled into `SmatchetTests`, yet `CPP_CODE_AUDIT.md` finding #2 sits in an untested branch of it. Reachability is necessary but not sufficient. This pilot measures the missing half: **assertion strength**, by planting plausible single-point bugs and checking whether the existing tests kill them.

Mutation testing flips a line of production logic (invert a comparison, drop a guard, off-by-one a bound), rebuilds, and reruns the suite. **Killed** = some assertion failed (good — the tests caught the bug). **Survived** = the suite still passed (a bug this shape would ship undetected — a weak assertion, an equivalent mutant, or an out-of-oracle blind spot).

## Phase 0 — feasibility (what runs here, and the scope it fixes)

This container is Linux; the product's primary test presets are MSVC/Windows or need ImGui/GLFW/X11/GL. Enumerated candidates:

| Target | Preset | Builds here | Runs here | Assertion oracle | Reach |
|---|---|---|---|---|---|
| **`SmatchetTsanTests`** | `ninja-tsan-linux` | **yes**¹ | **yes** — 209 cases / 2064 assertions / **0.59 s** | **yes (doctest)** | **52 Core TUs** |
| `fuzz_*` drivers | `ninja-fuzzer-linux` | (buildable) | crash-only | no — libFuzzer/ASan crash oracle, no correctness assertions | 6 parsers |
| `SmatchetCore_PosixCheck` | `posix-core-check` | compile-only (`STATIC EXCLUDE_FROM_ALL`) | **no** (never links/runs) | no — compile gate only | n/a |
| `SmatchetTests`, `SmatchetLuaTests`, bucket-E | `ninja-test-*` / ui | **no** — clang-cl/MSVC ABI or ImGui/GLFW/X11/GL | — | — | — |

¹ The TSan runtime archive (`libclang_rt.tsan`) was absent from the image; `apt-get install libclang-rt-18-dev` was needed to link. Filed as an infra backlog note — `ninja-tsan-linux` should link out-of-the-box.

**Only `SmatchetTsanTests` is an assertion-based mutation oracle on this host.** Its 52 compiled production TUs are the pilot's universe (well above the ~10-TU floor, so no CI-based alternative was needed). Of those 52, **17 carry their own dedicated `*.test.cpp` inside this rig** — the clean-signal set, where a surviving mutant is a genuine weak assertion. The other 35 are closure-only link deps whose dedicated tests live in the Windows-only main rig; a survivor there would be a *false* gap, so the pilot targeted TUs from the clean-signal set and, for every survivor, read the main-rig test too before ruling.

**Reconciliation with `testing-surface-roadmap.md`:** roadmap Slice **F** ("Mutation-smoke / coverage-delta gate", Gap 4, status `[new] — unbacklogged`, plan-required, unstarted) is exactly this territory. This pilot is the manual precursor that Slice F would automate; findings + the reusable harness (below) feed it. No scope conflict.

## Method

One mutation applied at a time via a harness (`git`-clean-asserted before and after each mutant): apply an exact single-point edit → `cmake --build` (incremental: 1 TU recompile + relink, ~2–8 s) → run `SmatchetTsanTests` → classify → `git checkout` revert → assert clean. **No mutation ever left the tree** (`git status` verified clean between every mutant and after every batch). Mutations that fail to compile are discarded (not counted). 12 TUs × ~5–7 mutants each.

## Headline numbers

| Metric | Value |
|---|---|
| TUs mutated | **12** |
| Valid mutants planted | **68** |
| Killed | **52** |
| Survived | **16** |
| Raw kill rate | **76.5 %** |
| Equivalent-mutant-adjusted kill rate² | **82.5 %** (52 / 63) |
| Genuine weak assertions found | **10** (3 fixed in this PR, 7 backlogged) |
| Equivalent mutants (unkillable by any test) | **5** |
| Out-of-oracle (killed only by a non-Linux-runnable test) | **1** |
| Build failures / spec errors | **0** |

² Equivalent mutants change no observable behaviour, so no test can kill them; excluding the 5 from the denominator gives the honest assertion-strength number.

## Per-TU kill rates

| TU | Killed | Survived | Kill % | Survivors (classification) |
|---|---|---|---|---|
| `TrackerGridFieldDisplayPure.cpp` | 3 | 3 | **50 %** | GR3 **weak→fixed**, GR5 weak, GR6 weak |
| `TrackerDateTimePure.cpp` | 3 | 2 | 60 % | DT2 equivalent, DT5 equivalent |
| `LinearQueryFromJql.cpp` | 4 | 2 | 67 % | JQL-01 equivalent, JQL-05 weak |
| `JqlSuggestEnginePure.cpp` | 4 | 2 | 67 % | JQL-03 weak, JQL-05 **weak→fixed** |
| `PlaneQuerySuggestEnginePure.cpp` | 4 | 2 | 67 % | PLANE-03 weak, PLANE-04 **weak→fixed** |
| `LinearIssueMappingPure.cpp` | 4 | 1 | 80 % | MAP-05 equivalent |
| `TrackerLabelsPure.cpp` | 4 | 1 | 80 % | m3 equivalent |
| `MergeWatchNotifyPure.cpp` | 4 | 1 | 80 % | m3 weak |
| `LocalCacheManager.cpp` | 5 | 1 | 83 % | LCM-06 out-of-oracle |
| `LinearClientHelpers.cpp` | 6 | 1 | 86 % | m5 weak |
| `TicketSyncService.cpp` | 6 | 0 | **100 %** | — |
| `AiPrefsTestConnectionPure.cpp` | 5 | 0 | **100 %** | — |

The two 100 % TUs (`TicketSyncService`, `AiPrefsTestConnectionPure`) and the ≥80 % pure-mappers demonstrate the `*Pure`/service-extraction test discipline works when the extracted seam is directly asserted. The weakness concentrates in **display-formatter and keystroke-suggest TUs** whose tests assert the *common* path richly but skip *edge branches* (tooltips, boundary counts, alternate match keys) — exactly the untrusted-input surface `CPP_CODE_AUDIT.md` and the gap map flag as under-tested.

## Every surviving mutant — exact diff + why it survived

### Genuine weak assertions (a real bug this shape would ship uncaught)

**① `TrackerGridFieldDisplayPure.cpp` — GR3 — worklog partial-page marker · FIXED in this PR**
```diff
- if (s.WorklogsOnPage < s.Total) {   // append "*" partial-load marker to model.line
+ if (s.WorklogsOnPage <= s.Total) {
```
On a *full* page (`WorklogsOnPage == Total`) the mutant appends the `*` "more work logs exist in Tracker" marker spuriously. The full-page test subcase (`total:20`, 20 entries) asserted only tooltip content, never `model.line`. **Fix:** the full-page subcase now asserts `model.line.back() != '*'`. Verified: mutant now KILLED.

**② `JqlSuggestEnginePure.cpp` — JQL-05 — email-prefix user match · FIXED in this PR**
```diff
- !nameMatch && !user.EmailAddress.empty() && AsciiStartsWithIgnoreCase(user.EmailAddress, pre);
+ nameMatch && !user.EmailAddress.empty() && AsciiStartsWithIgnoreCase(user.EmailAddress, pre);
```
Breaks the email-prefix fallback so a user found *only* by email prefix (display name misses) never surfaces. Every prior user test searched by display-name prefix (`ali`, `a`, `eve`), leaving the `emailMatch` branch unasserted. **Fix:** added a subcase with a user whose display name misses but whose email (`zoe@…`) prefix-hits; asserts the suggestion + label surface. Verified: mutant now KILLED.

**③ `PlaneQuerySuggestEnginePure.cpp` — PLANE-04 — display-name match offers account-id insert · FIXED in this PR**
```diff
- return AsciiStartsWithIgnoreCase(raw, pre) || AsciiStartsWithIgnoreCase(label, pre);
+ return AsciiStartsWithIgnoreCase(raw, pre) && AsciiStartsWithIgnoreCase(label, pre);
```
The near-twin gap to ②, in the *other* suggest engine: typing a display-name prefix (`Ali`) that does not prefix-match the account id (`u-123`) must still offer the account-id insert. The user-field test asserted only the `" (display) -> "` label (which fires regardless), never the id-insert suggestion. **Fix:** the user-field test now asserts `HasInsert(out, "u-123")`. Verified: mutant now KILLED. *(Finding the identical class in both hand-cloned engines is the strongest signal in the pilot — the `SMATCHET_DEVIATION(rule=duplication)` near-twins share a test blind spot.)*

**④ `TrackerGridFieldDisplayPure.cpp` — GR5 — maxResults tooltip line** *(backlog)*
```diff
- if (s.MaxResults > 0) {   // tooltip += "Page size (maxResults): N"
+ if (s.MaxResults >= 0) {
```
Emits the "Page size" tooltip line even when `maxResults == 0`. No worklog subcase asserts that tooltip line. Low severity (tooltip text), but a real assertion gap.

**⑤ `TrackerGridFieldDisplayPure.cpp` — GR6 — "This page:" tooltip branch** *(backlog)*
```diff
- if (s.Total > 0 && s.WorklogsOnPage > 0) {   // tooltip += "This page: a–b of N"
+ if (s.Total > 0 || s.WorklogsOnPage > 0) {
```
`&&`→`||` diverges only when exactly one operand is zero (e.g. `total:0` with a non-empty page array). Every subcase has both flags equal-signed, so the "This page:" tooltip branch is never exercised at that shape.

**⑥ `LinearQueryFromJql.cpp` — JQL-05 — 2-char quoted operand bound** *(backlog)*
```diff
- if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
+ if (s.size() > 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
```
Off-by-one in the unquote helper; diverges only for a 2-char quoted operand (`""`, `''`). No test uses an empty quoted value, so the boundary is unasserted. Low severity.

**⑦ `JqlSuggestEnginePure.cpp` — JQL-03 — 50-user suggestion cap** *(backlog)*
```diff
- if (++added >= kMaxUsers) {   // kMaxUsers = 50
+ if (++added > kMaxUsers) {
```
Off-by-one on the user-suggestion cap; no test builds a catalog anywhere near 50 matching users, so the cap boundary (50 vs 51 emitted) is never observed. Low severity but a boundary the code deliberately enforces.

**⑧ `PlaneQuerySuggestEnginePure.cpp` — PLANE-03 — empty-option guard** *(backlog)*
```diff
- if (raw.empty()) {   // tryAdd: skip empty option values
+ if (raw.empty() && false) {
```
Neutralises the empty-value skip in `tryAdd`. No test field carries an empty `AllowedValues`/option value, so the guard is never reached with empty input. A hostile/degenerate catalog with an empty option would emit an empty suggestion.

**⑨ `MergeWatchNotifyPure.cpp` — m3 — exact-cap message truncation** *(backlog)*
```diff
- if (out.size() > kMaxMessageBytes) {   // truncate notify payload
+ if (out.size() >= kMaxMessageBytes) {
```
Off-by-one on the truncation bound of a **network-listener** payload (SECURITY_AUDIT-flagged localhost listener, gap-map Tier-1 #6); diverges only at exactly `kMaxMessageBytes`. The sanitization test uses a 600-byte message (truncated identically either way), never an exactly-at-cap one. Cosmetic (1 byte) but on a trust-boundary TU.

**⑩ `LinearClientHelpers.cpp` — m5 — negative rate-limit header** *(backlog)*
```diff
- negative = (s[0] == '-');
+ negative = false;
```
Drops the sign in `ParseLongOr`. The negative-magnitude path (including the deliberate `LONG_MIN`-reconstruction logic + its comment) is entirely unasserted — `ParseLinearRateLimitHeaders` is only tested with positive values. Low real-world severity (rate-limit headers aren't negative) but the code goes out of its way to handle it, so a test should pin it.

### Equivalent mutants (no observable behaviour change — unkillable, **not** a test gap)

- **`TrackerDateTimePure.cpp` DT2** — `month > 12` → `month >= 12` in `DaysInMonth`. December (12) then hits the out-of-range fallback `return 31`, which *equals* December's real length — behaviourally identical for the only differing input. (Verified: fallback is a literal `return 31;`.)
- **`TrackerDateTimePure.cpp` DT5** — `w.Day > dim` → `>= dim` in `ClampDayToMonth`. When `Day == dim` the clamp writes `dim` (the same value) — no observable change.
- **`LinearQueryFromJql.cpp` JQL-01** — `substr(p + 8)` → `substr(p + 7)` in the ` not in ` branch. `kNotIn` is detected but **every field handler rejects it** with "unsupported operator", so `valueRaw` is never consumed — the offset change is unobservable. *(Product observation, not a test gap: `not in` is silently downgraded to "unsupported". A test SHOULD pin that `not in` warns; that pin would not kill this equivalent mutant but would document the intended behaviour.)*
- **`LinearIssueMappingPure.cpp` MAP-05** — `reserve(size())` → `reserve(size()+1)`. `reserve` affects capacity only, never observable size/contents.
- **`TrackerLabelsPure.cpp` m3** — `if (filterLower.empty())` → `if (false)`. `std::string::find("")` returns `0` (`!= npos`), so `LabelMatchesFilter` still returns `true` for an empty filter — the fast path was pure optimisation.

### Out-of-oracle (a runnable test *could* catch it, but that test isn't in this rig)

- **`LocalCacheManager.cpp` LCM-06** — `code != SQLITE_NOTADB && code != SQLITE_CORRUPT` → `|| `, in the corrupt-DB quarantine-and-rebuild gate. The only test that exercises this path is `tests/Core/LocalCacheManagerCorruption.test.cpp` (18 corrupt/rebuild/quarantine assertions) — which is compiled into the **main** rig but **not** the TSan rig. Confirmed: it is referenced once in `tests/CMakeLists.txt` (main block) and zero times in the TSan block. So the mutant is caught by CI on Windows; it survives *here* only because the Linux oracle is a subset. Flagged as a Linux-oracle blind spot, not a global gap.

## Ranked weak-assertion list (most to least valuable to pin)

1. **PLANE-04 + JQL-05 (email) — suggest-engine alternate-match keys** *(both FIXED)*. Two hand-cloned untrusted-keystroke engines share the same blind spot: the tests assert the *primary* match key (display name) but not the *secondary* (email prefix / account-id insert). Highest value — user-facing autocomplete correctness on the highest-frequency untrusted-input path, and the duplication makes a single-engine fix insufficient.
2. **GR3 — worklog partial-page marker** *(FIXED)*. User-facing "more data exists" indicator on the worst-kill-rate TU (50 %), which carries 3 `SECURITY_AUDIT` findings.
3. **GR6 / GR5 — worklog tooltip branches** *(backlog)*. Same TU; tooltip content (`This page:`, `Page size:`) entirely unasserted.
4. **PLANE-03 — empty-option guard** *(backlog)*. Degenerate/hostile catalog robustness on an untrusted-input parser.
5. **JQL-03 — user-suggestion cap boundary** *(backlog)*. A deliberately-enforced limit with no boundary test.
6. **MergeWatch m3 — exact-cap truncation** *(backlog)*. Trust-boundary TU; cosmetic magnitude.
7. **LinearQueryFromJql JQL-05 — 2-char quoted operand** *(backlog)*. Narrow parser edge.
8. **LinearClientHelpers m5 — negative header magnitude** *(backlog)*. Deliberately-handled path, near-zero real-world trigger.

## What this pilot did NOT find

No mutation survived because the **production code is wrong** (the audit-finding escape hatch). Every survivor is a missing/weak *test assertion* or an equivalent mutant, not a latent product bug. The one product-shaped observation — `LinearQueryFromJql` silently treats `not in` as "unsupported" — is an unimplemented-operator gap that already emits a warning, not a correctness defect; noted, not filed as an audit finding.

## Reproducing / extending

The harness (`scripts/dev/` candidate for Slice F) drives a JSON spec of `{file, search, replace}` mutants, rebuilds `SmatchetTsanTests` incrementally, runs it, records killed/survived, and reverts — asserting a clean tree throughout. At ~0.6 s/run + ~2–8 s incremental build, a 68-mutant sweep is a few minutes. Promoting it to a CI smoke (Slice F) over the hottest pure TUs would catch assertion rot the coverage-delta gate can't see.

## Deliverables

- **(a)** This report.
- **(b)** One PR strengthening the 3 worst survivors — each new assertion proven to kill its mutant (SURVIVED → KILLED confirmed): `tests/Core/JqlSuggestEnginePure.test.cpp`, `tests/Core/PlaneQuerySuggestEnginePure.test.cpp`, `tests/Core/TrackerGridFieldDisplayPure.test.cpp`.
- **(c)** Backlog entries for the remaining 7 weak assertions + the TSan-runtime infra gap + the reusable harness → `docs/self-improvement/categories/{test,infra,tooling}/2026-07-05-*`.

## Addendum — Phase 3 corpus expansion (2026-07-13)

Everything above shipped and was followed through: the 7 backlogged weak assertions were pinned post-pilot (all 7 re-verified KILLED), and the harness was productionised as `scripts/dev/mutation-smoke.sh` + `scripts/dev/mutation-smoke-corpus.json` with an advisory nightly gate (plan `docs/plans/mutation-smoke-gate.md`, Phases 1–2). Phase 3 (2026-07-13) expanded the corpus from the 10-mutant seed to **38 mutants covering all 20 dedicated-test TUs** now in the TSan rig — every entry live-validated in-container against `SmatchetTsanTests`:

- The 3 fixed-gap seeds (GR5, GR6, MergeWatch-m3) graduated `survived` → `killed` guards; the pilot's 4 other fixed survivors + the 5th equivalent (JQL-01) joined the corpus, completing the pilot's vetted set.
- 23 new mutants covered the 13 TUs the pilot didn't reach. **One survived** — a new genuine weak assertion, and a subtle one: `JiraErrorMessagePure.test.cpp`'s "cap never splits a multi-byte UTF-8 sequence" built a 200×'é' = exactly-400-byte message, which fits `<= kMaxJoinedErrorLen` (400) and appends whole — the truncation backoff the test documents never executed, so the test asserted nothing about it. Fixed (300×'é' + an ellipsis-marker assertion proving truncation engaged); SURVIVED → KILLED verified. The same at-the-boundary-but-not-past-it shape as the pilot's MergeWatch-m3 finding.
- One candidate was rejected as **flaky** (a `ConfigSaveWorker` drain-loop mutant whose verdict depended on worker-thread timing — 3 SURVIVED / 2 KILLED over 5 runs; a nightly gate needs deterministic oracles) and one skipped as **out-of-oracle** (`GridLiveContext.h` `everVisible`, consumed only by Windows-rig-tested AppController code — the LCM-06 class).
- Final gated sweep: **33/33 guards killed, 5/5 equivalents correctly surviving, 0 mis-ruled, 100% adjusted kill rate**, tree clean throughout.
