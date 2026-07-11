# Smatchet Evaluation Set — Independent Verification Pass

**Reviewer role:** Final QA / triple-check content audit of the 27-report evaluation set.
**Date:** 2026-06-30
**Scope:** `evaluation/README.md` + 9 expert folders (`01`–`09`), each with `without-agents.md`, `with-agents.md`, `comparison.md`. Method: direct Read/Grep/Glob/Bash against the live tree at `/home/user/Smatchet`; no sub-agents.

---

## 1. Verdict

**PASS-WITH-NOTES.**

The report set is internally consistent on every score, complete (all 27 reports present, none truncated, all carry a scorecard and persona-appropriate sections), and its recurring factual claims hold up against the codebase. Of six concrete spot-checks, **five are CONFIRMED outright** and **one (tracker JSON DoS) is PARTIALLY-CONFIRMED** — the gap is real but more remediated than a casual reader might infer, and the reports themselves flag it honestly rather than overclaim. The only items worth a reader's attention are framing/wording nuances in the README (the "~98%" figure and the deliberately-acknowledged postmortem-count variance), not substantive errors. Nothing in the set materially overclaims or is contradicted by the code.

---

## 2. Score-consistency check

Every blended figure, and every without/with pair, quoted in the README master scorecard was cross-checked against the number actually stated in that expert's `comparison.md` (and, where stated, the `without-/with-agents.md` overalls). **All nine rows match exactly.**

| # | Expert | README without/with/blended | Report states | Match |
|---|--------|:--:|:--:|:--:|
| 01 | White-hat hacker | 8.0 / 7.5 / 7.5 | "8/10 (without) → 7.5/10 (with)"; "Blended overall: 7.5/10" | ✅ |
| 02 | Agentic-infra | 7.5 / 8.5 / 8.0 | "7.5/10 (without) → 8.5/10 (with)"; "Blended overall: 8/10" | ✅ |
| 03 | Game-dev buyer | 6.0 / 6.5 / 6.25 | scorecard 6.0→6.5; "Blended score: 6.25 / 10" | ✅ |
| 04 | Architect | 8.0 / 8.0 / 8.0 | "8/10 (A) → 8/10 (B)"; "Final blended score: 8/10" | ✅ |
| 05 | Technical director | 6.0 / 7.0 / 6.5 | "Overall 6.0/10 / 7.0/10 / 6.5/10"; "Blended score: 6.5/10" | ✅ |
| 06 | QA director | 8.0 / 8.4 / 8.2 | "Pass A 8.0 … Pass B 8.4 … Blended QA score: 8.2/10" | ✅ |
| 07 | Indie CEO | 4.0 / 6.5 / 5.5 | "Overall 4/10 / 6.5/10"; "Blended score: 5.5/10" | ✅ |
| 08 | AAA tools lead | 7.0 / 7.5 / 7.5 | "Overall 7/10 / 7.5/10"; "Final blended score: 7.5/10" | ✅ |
| 09 | UX expert | 6.0 / 7.4 / 7.0 | "Pass A (6.0) … Pass B (7.4) … Blended UX maturity: 7.0/10" | ✅ |

Stated average "~7.2/10" and the range "5.5 (indie CEO) → 8.2 (QA director)" are arithmetically correct for the nine blended values. **No score mismatches found.**

---

## 3. Factual spot-checks against the codebase

**(a) Absolute perf budget ships DISABLED/null — CONFIRMED.**
`docs/perf/regression-policy.json` sets `default.mean_abs_ceiling_ms: null`, and the schema `description` says the Pillar-1 absolute mean budget (target 6.94 = 1000/144) "ships null/DISABLED until the perf-gate-revival step-5 calibration pass." Only relative-regression detection (`mean_delta_pct`, `mean_min_abs_delta_ms: 0.05`) is armed. Exactly as reports 03/06/08/09 claim.

**(b) `governance.auto_merge` is "on" — CONFIRMED.**
`project.config.json:157` → `"auto_merge": "on"`, under a `governance` block (`:153`) whose `_doc` describes it as "a STANDING, revocable grant" (set 2026-06-23). Matches the white-hat headline (report 01) and the README's framing.

**(c) Single-author / ~98% framing — CONFIRMED with a wording caveat.**
`git shortlog -sn --all`: Alexandros Konstantonis 49, Claude 11, dependabot[bot] 1 (61 total). By commit count the human is **80.3%**, not 98%. The README's "~98% one human author + AI agents + dependabot" only holds if "~98%" means *human + AI agents combined* (60/61 = 98.4% non-dependabot) — which is the most defensible reading of the sentence. The substance (one human + an AI fleet, dependabot a rounding error, bus-factor-of-1) is correct; the phrasing invites a reader to misattribute 98% to the human alone. Minor, flagged in §5.

**(d) Tracker HTTP clients still use bare `nlohmann::json::parse` on responses — PARTIALLY-CONFIRMED.**
A security sweep has migrated many hot paths to the bounded parser `smatchet::json_safe::ParseBounded` (Jira/Plane/GitHub/Linear issue-search and activity-feed clients, e.g. `JiraIssueSearch.cpp`, `GitHubActivityFeed.cpp:79`, `PlaneActivityFeed.cpp:71`, `LinearIssueSearch.cpp:180`). **But residual bare parses remain**: ~48 `json::parse` call-sites under `Source/Core/src/Tracker/`, of which ~21 are the *throwing* form on response `.text` — most concentrated in `TrackerFieldCatalog.cpp` (9: lines 87/102/122/145/180/226/257/303 + others), plus `GitHubClient.cpp:137` and `JiraActivityFeed.cpp:223`. These are genuinely DoS-relevant (a depth-bomb on an attacker-controlled tracker response can crash the client). So the gap the reports describe is real but **not universal** — it is a residual, not the whole estate. Crucially, the white-hat `without-agents.md` closes on exactly this caution ("the residual tracker-response `json::parse` calls…"), so the report set does **not** overclaim; it states the nuance correctly.

**(e) Lua sandbox blocks io/package/load/require/debug — CONFIRMED.**
`Source/Core/src/AppController_LuaBindings.cpp:241` `CreateSandboxEnvironment` explicitly blocks `dofile`, `loadfile`, `load`, `loadstring`, `require`, `collectgarbage`, `io`, `package`, `debug` (set to `false`, not nil — with a correct comment on why nil would leak via `__index`), strips `string.dump`, and blocks the raw-table/metatable escape hatches. `os` is intentionally *not* nil'd but replaced upstream (`AppController_LuaBindingsCore.cpp:238-247`, `InitLuaCore`) with a whitelisted safe time/date subset (`time/clock/difftime/date`), deliberately omitting `os.execute/remove/exit/getenv`. The "io"/"require"/"os" strings the naive grep surfaced in `LuaConsolePlugin.cpp:136-203` are *autocomplete candidate lists*, not the enforcement surface. Matches report 01's whitelist-sandbox claim.

**(f) MCP server loopback/token-gated by default — CONFIRMED.**
`Source/Core/include/Config/ConfigManager.h`: `McpEnabled = false` (`:182`) and `McpRequireTokenOnLoopback = true` (`:195`) by default; bind host defaults to `127.0.0.1` (`SmatchetDefaults.h:49`, `kBindLocalhost`). `McpPlugin.cpp` enforces a constant-time token compare (`ConstantTimeStringEquals`, `:198`), denies tokenless loopback when the secure default is on (`:185-192`), and adds a DNS-rebind defense accepting only loopback-literal `Host`/`Origin` (`McpJsonRpcPure.*`, `McpPlugin.cpp:155-161`). Matches report 01's "token-required loopback MCP with DNS-rebind defense + constant-time compare."

---

## 4. Completeness check

- **All 27 reports present** (9 folders × {without, with, comparison}) plus `README.md`. Verified by directory listing.
- **No truncation.** Word counts range 2,112–4,130 per report (README 1,162). Every report's final line is a conclusive sentence ("Bottom line:…", "Blended score…", a closing recommendation), not a cut-off mid-section. The `with-agents` passes are consistently the longest (more surface to cover), as expected.
- **Scorecards present in every file** (grep for scorecard/dimension-table/score-heading returned ≥2 hits in all 27).
- **Persona-appropriate sections present** on spot-read: hacker reports carry finding IDs (F-#/P-#) and an auto-merge threat model; buyer/CEO/AAA reports carry pass/pilot/lift verdicts and adoption blockers; QA carries a test-estate inventory; UX carries P0 a11y findings; architect carries layering/coupling analysis.
- **README index accurate:** exactly 9 table rows (`^| 0N `), and all nine markdown folder links (`](0N-…/)`) resolve 1:1 to the nine real directories. Bottom-line blurbs in the index align with each report's actual conclusion.

---

## 5. Quality red flags / discrepancies worth a reader's awareness

1. **"~98%" framing (README:11).** As noted in §3c, by commit count the human author is 80%, AI ~18%, dependabot ~2%. The "~98%" only parses as "human + AI agents combined." The README is not *wrong*, but the sentence "~98% one human author + AI agents + dependabot" is easy to misread as 98% human. A one-word clarification ("~98% one human author *plus* AI agents") would remove the ambiguity. Does not affect any score.
2. **Postmortem-ledger count variance — disclosed, not a defect.** Different reports quote "43", "51", "255 entries", "2,013 lines." The README explicitly pre-empts this (line 7), telling readers to treat per-report numbers as that reporter's own measurement of different things. This is honest scoping, not an uncaught contradiction — but a reader skimming a single report should heed the README's caveat.
3. **Cross-report score divergence is by design.** The same artifact (e.g., the auto-merge pipeline) is scored very differently across personas (a 5/10 dominant *risk* for the hacker; a top *asset* for the agentic-infra builder). Each `comparison.md` owns and explains its number; none silently contradicts a sibling. The README's "AGENTS.md effect" section reconciles the divergence rather than papering over it. No persona drift detected on spot-reads — each report stays in-lens (the hacker doesn't moonlight as a UX critic, etc.).
4. **One mildly generous self-flagged score.** Report 01's own `comparison.md` notes Pass B's Network/TLS and Secrets dimension bumps are "arguably under-evidenced" relative to Pass A — i.e., the report critiques its own optimism. That is a feature (self-auditing), but a reader should know the per-dimension deltas in the with-pass are softer than the headline.

No report was found to drift off-persona, to contradict a sibling without acknowledgement, or to assert a severity/score the code contradicts.

---

## 6. Net assessment of trustworthiness

**High.** The set is unusually disciplined for an LLM-authored evaluation corpus: every quoted score reconciles to its source report, every folder/link/row in the index is correct, and the recurring load-bearing technical claims — disabled absolute perf budget, `auto_merge: on`, the Lua sandbox blocklist, the MCP loopback/token defaults — are verifiable verbatim in the tree. The one claim with real nuance (residual bare `json::parse` in the tracker layer) is *also* the one the reports handle most honestly, stating it as a residual caution rather than a blanket failure, and my own grep confirms both that the gap exists (≈21 throwing parses on response bodies, concentrated in `TrackerFieldCatalog.cpp`) and that it has been substantially mitigated elsewhere. The only blemishes are wording-level (the "~98%" compression) and a deliberately-disclosed measurement variance (postmortem counts). A reader can rely on the scores, the verdicts, and the cited evidence; the two notes above are worth a glance but change no conclusion. **PASS-WITH-NOTES.**
