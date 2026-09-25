# Recurring finding-classes — ranked mining record (gate campaign)

> Mining record for the recurring-findings gate campaign (PR #1605): the defect
> classes this repo fixed by hand two or more times, ranked by
> **recurrence × severity × mechanical detectability**, with the gate decision
> taken for each. Sources swept: `SECURITY_AUDIT.md`, `CPP_CODE_AUDIT.md`,
> `docs/self-improvement/postmortems.md`,
> `docs/self-improvement/historical-review-findings.md`,
> `docs/self-improvement/categories/{tooling,applied,security}.md`, and the
> merged-PR history around #1566/#1573/#1592/#1598. This doc pre-scopes the next
> gate batch — consume it top-down.

## Shipped this batch (PR #1605)

| Rank | Class | Recurrence evidence | Gate shipped | Tier |
|---|---|---|---|---|
| 1 | Bare `json::parse` on untrusted input outside `json_safe::ParseBounded` | 25/33 SECURITY_AUDIT findings; re-fixed in #1566, #1573, #1592, #1598 — the last three in TUs the old curated `BARE_JSON_INGRESS_TUS` allow-list did not watch; also recurred in a header (`JsonParseUtil.h`) and the `file >> j` operator form | `bare-json-parse-untrusted` widened to repo-wide default-deny over all first-party `.cpp/.h/.hpp` + the declare-then-slurp `>>` form; allow-list retired; tree burned down (fixes + `SMATCHET_DEVIATION`); whole-tree emptiness bats-asserted | **blocking** |
| 2 | Silent exception swallow — empty `catch (...) {}` | exception-handling-policy hard rule 1 (review CRITICAL); editor hook existed but no merge gate; CPP_CODE_AUDIT #6 is the missing-guard sibling | `catch-all-swallow` absolute-0 (tree was at 0) | **blocking** |
| 3 | Unbounded recursive walker over untrusted JSON/Lua (the campaign's DW class) | cpp-security-hardening § Approach ("bounding the parse does not bound a hand-written recursion"); CPP_CODE_AUDIT #20; recurrences #1220/#1237; 2 live residuals found on day one (`FieldCatalogCache::OptionFromJson`, `TrackerFieldValueParser::TrackerFieldOptionFromJson` — both transitively bounded by ParseBounded's depth-256 upstream) | `unbounded-recursive-json-walker` | WARN-first (heuristic text proxy; graduate per the duplication precedent) |
| 4 | Unbounded whole-file read (no byte cap) | SECURITY_AUDIT #33 (Win32 64 MiB cap vs uncapped POSIX sibling); CPP_CODE_AUDIT #11/#31; 11 residual sites inventoried | `unbounded-file-slurp` | WARN-first (residuals are dev-controlled fixtures/config) |

Blocking-vs-advisory rationale: exact-signal rules on a zero-hit tree block
(ranks 1–2); judgment-call heuristics start advisory (ranks 3–4), mirroring the
`duplication` WARN→block graduation (ADR-0015).

Wiring decision: **no new workflow lane** — all four rules ride the existing
REQUIRED `Comment-noise + high-integrity gate` context via
`test-lint-rules.sh --diff`. Postmortems #923/#1130 show non-required lanes are
bypassed by native merge paths, so extending the required lane is strictly
stronger than the pillar2-scan.yml/dup-scan.yml standalone-workflow template.
New-gate fail-open lessons honored: asserts-failure selftests per rule,
whole-tree clean invariants bats-asserted, campaign sweep modes
(`--scan-bare-json` / `--scan-catch-all` / `--scan-json-walkers` /
`--scan-slurps`), remediation printed on every failure.

## Shipped this batch (Slice S4 — Quality Pillar 6 offline-first gates)

| Rank | Class | Recurrence evidence | Gate shipped | Tier |
|---|---|---|---|---|
| 1 | Backend write outside offline queue | New pillar; ADR-0026 codifies offline-first as UX Quality Pillar 6; blocking exact rule to catch tracker mutations reaching the wire without queueing | `offline-write-bypasses-queue` exact-delta, blocking | **blocking** |
| 2 | Transport error kind collapsed to Unknown | ADR-0026; dead-letter + audit data loss on kind erasure; blocking exact rule to catch `TrackerErrorUnknown()` bare-call in tracker code | `tracker-error-kind-collapsed` exact-delta, blocking | **blocking** |
| 3 | Loading-only render while cache exists | UX regression (Pillar 4 / Pillar 6 contract); a view with cached data must show cache + freshness cue, never loading spinner | `offline-loading-only-render` heuristic, delta-warn | **WARN-first** (graduation per duplication precedent: calibrate 5 PRs before blocking) |
| 4 | Inflight-fetch latch unguarded on poll | Pillar 2 stall risk; `ResultIsCurrent` + `in_flight` must be kept in lock-step, never cached separately | `offline-inflight-latch-unguarded` heuristic, delta-warn | **WARN-first** |
| 5 | Failed fetch cached as loaded | Logic error (Pillar 6); a fetch failure keeps prior cache but must not mark as live/loaded; next poll must retry | `offline-failure-cached-as-loaded` heuristic, delta-warn | **WARN-first** |
| 6 | Cache cleared without audit | Data loss (Pillar 6); a wipe of `lookup_cache` / `pending_actions` must emit audit entry so user sees intent | `offline-cache-cleared` heuristic, delta-warn | **WARN-first** |
| 7 | Network read ungated by freshness cue | UX regression (Pillar 6); a cached view must gate fetch behind `DataFreshnessCue` so network off / failed fetch keeps rendering | `offline-network-read-ungated` heuristic, delta-warn | **WARN-first** |

Rationale: exact rules (1–2) block on zero-hit tree per the bare-json / catch-all precedent; heuristics (3–7) start WARN-first and graduate to blocking once a 5-PR calibration window closes with <10 % false-positive rate (per ADR-0015 duplication gate).

## Reconciliation vs `docs/plans/shipped/ci-falsepositive-hardening.md`

Checked before building (campaign charter requirement). No overlap to extend:
Cluster C hardens gates that **mis-fire** (false-RED on noise, false-green on
silent skips — unicode-bats, p99 noise, coverage-quarantine), while this
campaign adds gates for **product-code defect classes** that had none. The two
meet only at the shared fail-open lessons, which this batch consumed as design
constraints rather than re-implementing. The comment-noise-gate placement item
on the Cluster-C roadmap (`process.md:16`) was already resolved upstream by the
lane decoupling; the roadmap's remaining slices are untouched by this work.

## Deferred (pre-scoped next batch, in rank order)

1. **Pillar-2 reachability + pattern gaps** — `pillar2-scan.sh is_ui_reachable()`
   misses transitively-imgui helpers (ledgered `tooling.md` #327, MEDIUM) and
   `SYNC_IO_REGEX` misses `std::ofstream`/`fopen`/`std::filesystem::`/blocking
   sockets/`SQLite::Transaction`. Deferred: surgery on a live required lane
   deserves its own PR with its own burn-down; false-positive risk is real
   (worker-thread I/O in `*Ui*` files needs annotations).
2. **Cache/DB-tier missing try/catch** (CPP_CODE_AUDIT #6 shape — a bare
   `cache->`/`SQLite::` call outside any `try` in a Cache/DB-tier TU) plus the
   `catch (...)`-without-LOG tier (32 grandfathered sites, editor-hook WARN
   today). Deferred: needs lexical-scope analysis a bash proxy does poorly;
   start WARN-first when built.
3. **Unchecked fallible C-API null** (`std::localtime`/`std::gmtime` straight
   into `strftime`, JNI `NewGlobalRef`/`FindClass` deref) — recurred across
   audits (cpp-security-hardening #35, CPP_CODE_AUDIT #22/#32) but the sink set
   is tiny; cheap, high-precision grep when batched.
4. **Gate fail-open meta-class residuals** — historical-review-findings #329
   (stale-artifact grep), #80/#77 (zero-assertion guards), #1116/#789
   (pre-ship fail-opens). The meta-gate shipped in #1510 covers NEW gates only;
   the residuals are tracked backlog items, burn down opportunistically.
5. **WARN→blocking graduation** for ranks 3–4 above once the calibration window
   (~20 PRs, <10 % FP — the ADR-0015 bar) closes.

## Class-selection method (repeatable)

Fan out readers over the audit docs + postmortems + categories ledgers; keep
only classes with ≥2 independent remediation events AND a cheap mechanical
discriminator (a token/shape a grep-level scan can match with an explicit
escape hatch); rank by recurrence × severity × detectability; burn the tree to
zero (fix or deviation-annotate every live hit) before flipping a rule
blocking — never ship a red gate.
