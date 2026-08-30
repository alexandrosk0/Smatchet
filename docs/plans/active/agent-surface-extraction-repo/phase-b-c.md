# Companion — Phase B (seed) + Phase C (flip) detail

> Companion detail doc for the plan [`agent-surface-extraction-repo.md`](../agent-surface-extraction-repo.md). This file carries the 2x-depth expansion of Phase B (rows 8–10: the seed of `the-unwilling-agentic-bunch`, its CI, its root files) and Phase C (rows 11–16: the flip PR). The main plan stays the authoritative scope + status document; at archive time this companion directory is `git mv`'d together with the plan file.

### Phase B — seed `the-unwilling-agentic-bunch` (new public repo; user creates it — pause exception 3, cross-repo mutation)

**Preconditions (all human, all pause-exception 3).** (a) `gh repo create alexandrosk0/the-unwilling-agentic-bunch --public` with **no** auto-init (`--add-readme`/`--license`/`--gitignore` all omitted) — a non-empty target makes the seed push a non-fast-forward; (b) `git-filter-repo` on `PATH` (`pip install git-filter-repo`, needs git ≥ 2.24 + python ≥ 3.6 — `git filter-repo --version` is the probe); (c) CodeRabbit app installed on the new repo. Everything else in this phase is scripted (row 8f).

8. New repo seeded by `git filter-repo` from a fresh Smatchet clone: `--path agents/ --path docs/agent-rules/ --path docs/harness/ --path docs/self-improvement/AGENT_SELF_IMPROVEMENT.md --path AGENTS.md --path scripts/dev/project-config.sh` + one `--path tests/bats/<file>` per layer-coupled suite (the 61-file list generated at seed time via `grep -l 'agents/' tests/bats/*.bats`, committed alongside the seed script as the audit trail). Entries (`docs/self-improvement/categories/`, `postmortems.md`, `applied.md`, `.jsonl` ledgers) are **excluded** — they stay host-side. History preserved; layout unchanged.

8a. **Fresh clone, single branch, no tags.** `filter-repo` refuses a repo that is not a fresh clone (it probes for a stash, extra remotes, extra worktrees, unpushed refs, loose objects) and aborts with `Refusing to destructively overwrite repo history`. The fix is always a new clone directory — **never** `--force`, which also suppresses the not-empty-target check. Clone with `--no-local` so the rewrite cannot touch the source repo's objects through hardlinks, and prune the product's release tags at clone time (they version the Smatchet exe and are meaningless in the layer):

```bash
rm -rf /tmp/agent-layer-seed
git clone --no-local --no-tags --single-branch --branch develop \
    https://github.com/alexandrosk0/Smatchet.git /tmp/agent-layer-seed
cd /tmp/agent-layer-seed
```

8b. **Keep the branch named `develop`.** Not cosmetic: ~60 of the moved `agents/scripts/**` gates compute a merge-base delta against the literal `origin/develop` (`dup_audit.py --diff`, `agent_size_audit.py --diff`, `test-lint-rules.sh --diff origin/develop`, `test-markdown-links.sh`, `is-pure-docs-diff.sh`'s `base_ref` default). Renaming the layer's default branch to `main` would silently turn every one of those into a no-op or a hard error in the layer's own CI.

8c. **Path list by file, never on argv; keep-list form, no `--invert-paths`.** `filter-repo`'s `--path` is additive-keep by default, so the keep list *is* the audit trail the plan wants committed; an invert list would have to enumerate the larger, faster-drifting set that stays. Pass it via `--paths-from-file` rather than ~70 `--path` argv entries — the same argv-length discipline that forced the GraphQL-document-by-file fix on Windows (commit `52500a9bd`), and the file is the artefact reviewers read. Matching is a **path prefix rooted at the repo root**, not a basename glob, so a bare `AGENTS.md` line takes only the root rulebook and leaves every subsystem leaf doc (`Source/Core/src/<ctx>/AGENTS.md`) host-side — assert that rather than assume it.

```bash
# seed-paths.txt — committed to the layer repo as docs/seed-paths.txt (audit trail)
agents/
docs/agent-rules/
docs/harness/
docs/self-improvement/AGENT_SELF_IMPROVEMENT.md
docs/high-integrity/portable-purity-baseline.txt
docs/high-integrity/agent-size-baseline.md
AGENTS.md
scripts/dev/project-config.sh
# ... then one line per layer-coupled bats suite, appended by:
#   grep -l 'agents/' tests/bats/*.bats | sort >> seed-paths.txt   # expect 61 lines

git filter-repo --paths-from-file seed-paths.txt \
    --replace-refs delete-no-add --prune-empty auto
```

`--replace-refs delete-no-add` keeps `refs/replace/*` out of the seeded repo (they otherwise ship a confusing dual view of every rewritten commit); `--prune-empty auto` (the default, stated explicitly so a future edit does not flip it) drops commits that the path filter emptied but that were non-empty before — without it the layer's history is mostly product commits with no diff. Expect on the order of ~500 surviving commits (the measured size of the surface's history vs the 525 entry-only commits that stay behind).

8d. **`filter-repo` deletes `origin` by design.** Re-point and push:

```bash
git remote add origin https://github.com/alexandrosk0/the-unwilling-agentic-bunch.git
git push -u origin develop
```

**Accept (row 8):** `git ls-files | grep -cE '^(Source/|docs/plans/|docs/self-improvement/categories/|CMakeLists.txt)'` returns 0; `git ls-files '*AGENTS.md'` returns exactly `AGENTS.md`; `git ls-files 'tests/bats/*' | wc -l` matches the seed list length; `git log --follow --oneline -- agents/core/code-review.md | wc -l` is > 1 (history really survived, not a single squashed import commit); `git count-objects -v` shows a packed repo.

8e. **Suite/wrapper co-location is a hard post-condition of the 61/33 split.** `scripts/dev/test-all.sh` discovers tests by the `test-*.sh` glob and never runs a `.bats` directly, and `test-orphan-bats.sh` fails any suite no wrapper names. Two of the 61 measured suites have their wrapper on the *other* side of the split — verified: `tests/bats/android_openssl_failfast.bats` ← `scripts/dev/test-android-openssl-failfast-bats.sh`, and `tests/bats/safe_merge.bats` ← `scripts/dev/test-safe-merge-bats.sh`. Left alone this reds **both** repos (layer: orphan suite; host: wrapper pointing at a deleted file). The split rule is unchanged — the reconciliation is mechanical and happens **in Phase A, before the seed**: `git mv` those two wrappers into `agents/scripts/core/` so `--path agents/` carries them, and register both suites in the layer CI's `CI_SKIP_RE` with the reason "subject-under-test is host-side" (the same clean-SKIP contract `test-all.sh --ci` already uses for a missing `Smatchet.exe`). The seed script asserts the invariant (row 8f phase 2) so a suite added between now and the flip cannot re-open the hole silently.

8f. **Seed script shape** — `agents/scripts/core/seed-agent-layer-repo.sh`, the scripted part of the § Verification Manual-residue bullet. **Do not name it `test-*.sh`**: `test-all.sh`'s glob auto-enrols anything matching, which would run a destructive cross-repo seed in CI. Contract:

```text
seed-agent-layer-repo.sh --target <owner/repo> [--work-dir DIR] [--dry-run] [--help]

Exit: 0 seeded · 1 assertion failed · 2 usage / tooling missing · 3 target not empty

phase 1 preflight  git-filter-repo on PATH; gh auth status; target repo exists AND has
                   zero commits; work-dir absent (refuse to reuse — see 8a)
phase 2 manifest   regenerate seed-paths.txt; assert 61 bats lines; assert suite/wrapper
                   co-location (8e); assert no path under docs/self-improvement/categories/,
                   docs/plans/, Source/; print the manifest for review; stop here on --dry-run
phase 3 rewrite    clone (8a) + filter-repo (8c)
phase 4 scaffold   write the row 9 workflow files, row 10 root files, .coderabbit.yaml;
                   commit as `chore(seed): layer CI + consumption contract`
phase 5 publish    remote add + push develop; gh label create (the override labels from
                   the layer's own project.config.json § merge_gates.override_labels);
                   setup-branch-protection.sh against the layer's required_contexts
phase 6 report     print the row-8 Accept checks with PASS/FAIL and the seed SHA
```

It must pass the host `shell-lint` lane it will be committed under — `test-lint-bash.sh` (SC2086 / SC2046 / SC2155) plus `test-shell-lint.sh`'s 5-rule gate (deps preflight, shellcheck clean, no bare `curl`, sha256-pinned downloads, `--help`/flag parity), so the `--help` block and a `command -v` deps preflight are not optional.

8g. **`scripts/dev/project-config.sh` is a deliberate two-copy mirror, not an accident — and it gets a drift gate.** The host copy is **not** removed in row 11 and cannot be: `docs/plans/split-scripts-build-vs-agentic.md` fixed the rule that "the code repo must build with **no** dependency on the `agents/` tree", and the file is sourced by build-path callers (`scripts/dev/with-msvc-env.sh`) and by host-side agentic callers on the literal path (`agents/scripts/core/postmortem-owed.sh` does `. scripts/dev/project-config.sh`). Rules for the two copies:
   - The **layer copy is canonical**; the host copy is a byte-identical mirror. Both work unmodified because the file derives everything from its own location (`_pc_root` = two dirs up from `$BASH_SOURCE`) and the Phase A three-rung resolution order finds the host `project.config.json` from inside the submodule via `--show-superproject-working-tree`.
   - **Drift gate**: `cmp -s "$PROJECT_ROOT/scripts/dev/project-config.sh" "$AGENT_LAYER_ROOT/scripts/dev/project-config.sh"` — a new step in `agent-layer-integration.yml` (row 12b) and a new `test-docs.sh` step host-side. Edit direction is layer-first: a layer PR changes the canonical copy, the bump PR carries the mirrored host copy in the same diff, and the gate reds until it does.
   - **Rejected alternative** (record it so it is not re-proposed): making the host copy a 5-line shim that sources the layer copy. It inverts the dependency the split-scripts decision established, and it turns a missing/empty `agent-layer/` from one loud `check-harness-provisioned.sh` diagnostic into a hard failure of every host script's config load.

9. New repo CI: `agentic-selftests.yml` (over the 61 moved bats suites), `shell-lint.yml`, and a doc-validation subset (anchors / agent-contract / portable-purity / markdown-links scoped to the moved tree) — all reusing the moved scripts on themselves; branch protection + `merge-gates.sh` self-hosted as its own gate-poller; a **fresh** `.coderabbit.yaml` (no self-improvement auto-exemption — those paths stay in Smatchet, whose `.coderabbit.yaml` keeps the path_filter unchanged).

9a. **File inventory + what each lane runs** (all `runs-on: ubuntu-latest`, all `actions/checkout` with `fetch-depth: 0` — every lane below is a merge-base delta vs `origin/develop` — and `persist-credentials: false`; every lane exports `PROJECT_ROOT=$AGENT_LAYER_ROOT=$GITHUB_WORKSPACE`):

| File | Job / required context | Runs |
|---|---|---|
| `agentic-selftests.yml` | `Agentic self-tests (bats)` | `setup-harness.sh claude-code` then `scripts/dev/test-all.sh --ci` over the layer's `TEST_ROOTS`; `CI_SKIP_RE` extended with the two host-subject suites from 8e |
| `shell-lint.yml` | `Shell lint (shellcheck)` | `test-lint-bash.sh` + `test-shell-lint.sh` — copied verbatim, both scripts move |
| `doc-validation.yml` | `Doc anchors + agent contract` | the layer-runnable subset of `test-docs.sh` (9b) |
| `merge-gates` (no workflow) | — | `merge-gates.sh` invoked by the layer's own janitor/watcher; self-hosted per grill decision 2 |
| `auto-bump.yml` | — | **Phase D row 18**, not seeded here; branch protection must exist first |

`scripts/dev/test-all.sh` and `scripts/dev/test-docs.sh` are **not** in row 8's path list but the lanes above invoke them, so the seed list gains `--path scripts/dev/test-all.sh --path scripts/dev/test-docs.sh` (both are pure runners over the `test-*.sh` glob; they stay host-side too, as a second sanctioned mirror covered by the same 8g `cmp` gate).

9b. **Doc-validation subset — the include/exclude split is not a judgement call, it is host-content reachability.** Included (layer content only): `test-doc-anchors.sh`, `test-agent-contract.sh` (with the 9c fix), `test-portable-purity.sh`, `test-markdown-links.sh`, `md_lint.py --all`, `test-agent-discovery-fixture.sh`, `test-gate-selftests.sh --check`, `test-oob-label-impl.sh`, `test-portable-agent-vexp.sh`, `test-orphan-bats.sh`. **Excluded, with the reason each would hard-fail standalone**: `test-plan-index.sh` / `test-plan-ref-integrity.sh` / `test-plan-claim-anchors.sh` / `test-plan-naming.sh` (read `docs/plans/`, host-side); `test-agent-build-facts.sh` (opens `CMakePresets.json` and resolves every `ninja-*` token in it — host-only, verified); `test-config-globs.sh` (asserts every `project.config.json` glob matches ≥ 1 tracked file — the layer's globs point at `Source/`); `work_item_lint.py` (reads `docs/work/`); `check-pr-intent.sh --check-workflow-sync` (diffs a verdict regex against `.github/workflows/`, which is a *different* file set in the layer — re-point it or drop it, do not run it blind).

9c. **`test-agent-contract.sh` must be made standalone-safe before it can be the layer's own gate.** Check `[12/15]` (V3.3) does an unconditional `check_fail` when `Source/Core/src/P4Annotate.cpp` is absent, so it fails 100 % of layer runs. Fix in Phase A (host-side, no behaviour change while `Source/` is present): wrap the check in a host-content probe — `if [[ ! -d "$PROJECT_ROOT/Source" ]]; then check_skip "V3.3: host content absent (layer standalone)"; elif [[ ! -f ... ]]; then check_fail ...`. **Audit the other 14 checks the same way in the same pass** and record the verdict per check in the PR body; anything that reads outside `agents/`, `docs/agent-rules/`, `docs/harness/` is a candidate. **Accept:** `PROJECT_ROOT=$PWD bash agents/scripts/core/test-agent-contract.sh` exits 0 in a tree with `Source/` deleted, and still exits 0 host-side with the full tree.

9d. **`test-orphan-bats.sh` and `test-doc-anchors.sh` self-locate through hardcoded `agents/scripts/...` paths.** `test-orphan-bats.sh` hardcodes `WRAPPER_ROOTS=(scripts/dev agents/scripts/core agents/scripts/project)`; `test-doc-anchors.sh` ends with `exec "$PY" agents/scripts/core/test_doc_anchors.py`; `test-all.sh` has the same `TEST_ROOTS` triple guarded by `[ -d "$root" ]` — which means post-flip the host run **silently drops two of three roots and reports green over a fraction of the suite** rather than erroring. All three take `AGENT_LAYER_ROOT` in Phase A (they are layer-content readers), and the `[ -d ]` guard in `test-all.sh` gains an explicit "expected root missing" hard failure so a mis-set root can never degrade to a quiet pass.

9e. **`merge-gates.sh` self-hosts correctly but only standalone.** Its config derivation is `local config_file="${MERGE_GATES_CONFIG_FILE:-$SCRIPT_DIR/../../../project.config.json}"` — three levels up from `agents/scripts/core/` is the repo root, which is right in the layer repo and **wrong** in Smatchet post-flip (it resolves to `agent-layer/project.config.json`, silently gating every Smatchet PR against the *layer's* required-contexts list and rendering the required-absent detector inert). The layer side needs no change; the host side is row 12a.

9f. **Branch protection + labels.** `setup-branch-protection.sh` moves with the layer and reads `project.config.json § branch_protection.required_contexts`; seed that array with exactly the two job names the lanes above emit (`Agentic self-tests (bats)`, `Shell lint (shellcheck)`, `Doc anchors + agent contract`) — `test-required-context-parity.sh` asserts every required context is emitted unconditionally, so a context named here that a lane emits behind an `if:` wedges every layer PR. Labels to create: the layer's own `merge_gates.override_labels` set (`cr-out-of-band`, `bugbot-out-of-band`, `plan-lock-out-of-band` is **not** needed — no `docs/plans/` in the layer).

**Accept (row 9):** all three lanes green on the seed commit itself; `gh api repos/alexandrosk0/the-unwilling-agentic-bunch/branches/develop/protection` lists exactly the three contexts; a deliberately-broken throwaway PR in the layer (e.g. an unresolvable `AGENTS.md § Nope` reference) reds `Doc anchors + agent contract` and is blocked by `merge-gates.sh`.

10. New repo root additions: `README.md` (consumption contract: mount at `agent-layer/`, dual-root semantics, host provides `project.config.json` at superproject root), `LICENSE`, and the layer's **own real `project.config.json`** — the repo is its own first consumer (self-hosting: its CI runs the framework's gates on the framework itself with `PROJECT_ROOT=$AGENT_LAYER_ROOT`), and that file doubles as the reference example for new consumers.

10a. **The schema blocks a layer-only config today — decide it here, not at implementation time.** `project.config.schema.json` has top-level `required: ["project","build","perf","lint","vcs","ci","merge_gates","harness","agents"]`, `build.required: ["presets","targets","exe_path"]` with `minItems: 1` on both arrays, and `perf.required` listing all five numeric fields; `project-config.sh`'s no-deps gate exits 2 on any missing top-level required key. A repo with no CMake presets, no build targets and no frame budget cannot satisfy that honestly. **Decision: loosen the schema once, in the Phase A/C Smatchet PR** (the schema is root-pinned and never moves — plan § Context point 1):
   - add `"profile": { "enum": ["product", "agent-layer"], "default": "product" }` to top-level `properties` (top level is `additionalProperties: false`, so the key must be declared, not just used);
   - move `"build"` and `"perf"` out of the unconditional top-level `required` array into an `allOf` / `if-then` that requires them when `profile` is absent or `"product"`;
   - leave `build`/`perf`'s own inner `required` + `minItems` untouched, so a product consumer that *declares* the blocks still has to fill them.

10b. **Consequence for the no-deps gate.** `project-config.sh` reads only `schema.required`, so once `build`/`perf` leave that array the gate stops hard-failing the layer config; the conditional requirement is then enforced by the real jsonschema step in host `doc-validation.yml` and in `agent-layer-integration.yml` (row 12b), which are the only places both files coexist. Note the gate is *already* silently inert when the schema file is absent (`except FileNotFoundError: pass`) — the layer must therefore **not** rely on schema-absence as its escape hatch; `PC_SCHEMA_FILE` is left unset in the layer and validation is a host-side responsibility, stated in the README.

10c. **Layer `project.config.json` shape**: `project.name = "the-unwilling-agentic-bunch"`, `project.env_prefix = "AGENTLAYER"`, `project.literals` = the layer's own denylist seed (it must **not** contain `Smatchet` — `test-portable-purity.sh` reads this list, and seeding it with the host's literals would make the layer fail its own purity gate on the prose that has not been de-Smatchet-ified yet, which is an explicit non-goal); `profile: "agent-layer"`; no `build`/`perf`; `lint.zones` scoped to the moved tree; `branch_protection.required_contexts` per 9f; `merge_gates.cr_bot_logins` copied.

10d. **README must not over-claim.** The layer's config is the reference example **for a no-product-code consumer**; a consumer with product code additionally fills `build` + `perf` (and drops `profile`, or sets it to `"product"`). Say that explicitly — the fully-worked product example stays `Smatchet/project.config.json`, linked from the README. README also carries: the mount point (`agent-layer/`), the dual-root contract (`PROJECT_ROOT` = superproject, `AGENT_LAYER_ROOT` = this repo), the three-rung config resolution order, the "submodule working tree is not a write target" warning, and the `git submodule update --init --recursive` bootstrap.

**Accept (row 10):** `PC_CONFIG_FILE=<layer>/project.config.json bash scripts/dev/project-config.sh` exits 0 and prints `PC_BUILD_PRESETS=''`; the host jsonschema step validates **both** configs; host `Smatchet/project.config.json` still validates unchanged (the loosening is additive); `test-portable-purity.sh` green in the layer.

### Phase C — the flip (Smatchet PR)

11. `git rm -r agents/ docs/agent-rules/ docs/harness/` + `git rm docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` + `git rm` the 61 layer-coupled bats suites + `git submodule add ../the-unwilling-agentic-bunch.git agent-layer` + `.gitmodules` (relative URL — both repos public, resolves tokenless). `docs/self-improvement/categories/` + ledgers + the 33 product-coupled bats suites stay.

11a. **Ordered checklist.** Two commits, deliberately — a delete commit and an add commit — so the rollback in 11d has clean granularity and `git log --follow` in the host still traces the deletions:

```bash
# 0. branch + pre-flip inventory (the acceptance oracle)
git switch -c feat/agent-layer-flip
git ls-files agents docs/agent-rules docs/harness > /tmp/pre-flip-inventory.txt

# 1. remove the in-tree surface
git rm -r -q agents docs/agent-rules docs/harness
git rm -q docs/self-improvement/AGENT_SELF_IMPROVEMENT.md
git rm -q docs/high-integrity/portable-purity-baseline.txt docs/high-integrity/agent-size-baseline.md
xargs -a seed-bats-list.txt git rm -q      # the 61, same list the seed consumed
git commit -m "refactor(agent-layer)!: remove the in-tree agent surface"

# 2. mount the layer, pinned at the seed SHA
git submodule add ../the-unwilling-agentic-bunch.git agent-layer
git config -f .gitmodules submodule.agent-layer.branch develop
git -C agent-layer checkout <layer-sha-with-the-row-14-tmpl-fix>
git add .gitmodules agent-layer
git commit -m "feat(agent-layer): mount the-unwilling-agentic-bunch as agent-layer"

# 3. relink the harness — .claude/agents is per-file HARDLINKS into the now-deleted
#    agents/*.md inodes; nothing re-materializes it on its own
bash agent-layer/agents/scripts/core/setup-harness.sh claude-code
bash agent-layer/agents/scripts/core/check-harness-provisioned.sh
```

11b. **`.gitmodules` content** (tabs are git's own format; do not hand-normalize to spaces):

```gitconfig
[submodule "agent-layer"]
	path = agent-layer
	url = ../the-unwilling-agentic-bunch.git
	branch = develop
```

Do **not** add `shallow = true`: the host lanes run the layer's merge-base-delta gates through the submodule and a depth-1 checkout has no `origin/develop` to diff against (the exact class the `feat/pillar2-fetch-depth` fix already burned once). Relative-URL semantics: the URL resolves against the **superproject's** `origin`, i.e. `https://github.com/alexandrosk0/Smatchet.git` → `https://github.com/alexandrosk0/the-unwilling-agentic-bunch.git`. **Failure mode:** a fork at `github.com/<other>/Smatchet` resolves to `<other>/the-unwilling-agentic-bunch`, which does not exist — the documented escape is a local, uncommitted override `git config submodule.agent-layer.url https://github.com/alexandrosk0/the-unwilling-agentic-bunch.git && git submodule sync`. Put that line in the root `AGENTS.md` stub (row 13) and the layer README.

11c. **Verification battery, run locally before push** (each of these is a known post-flip break, not a formality): `bash agent-layer/agents/scripts/core/plan-lock-gate.sh` — it resolves `lib="$root/agents/scripts/core/lock-table-cache.sh"` from `git rev-parse --show-toplevel` and hard-exits 1 when absent, so the CI-invoked fail-CLOSED net reds on the very first post-flip PR unless Phase A rewired it; `bash scripts/dev/test-all.sh` — count the suites and compare against the pre-flip count (a drop means the `[ -d "$root" ]` root guard silently skipped a root, per 9d); `bash scripts/dev/test-docs.sh`; `bash agent-layer/agents/scripts/core/test-agent-discovery-fixture.sh`; `bash agent-layer/agents/scripts/core/test-orphan-bats.sh`.

11d. **Rollback recipe.** `git revert` restores the *tracked* state and nothing else — empirically it leaves the `[submodule "agent-layer"]` section in `.git/config`, an orphaned `.git/modules/agent-layer`, and `agent-layer/` on disk as an untracked non-empty directory (git itself warns `unable to rmdir agent-layer: Directory not empty`). Full recipe, for a flip that reached `develop` as a squash-merge:

```bash
git revert --no-edit <squash-sha>          # squash-merge = an ordinary commit; NOT -m 1
# git revert cannot touch these — they are local state, per worktree:
git submodule deinit -f agent-layer
git config --remove-section submodule.agent-layer 2>/dev/null || true
rm -rf .git/modules/agent-layer agent-layer
bash agents/scripts/core/setup-harness.sh claude-code   # relink to the restored in-tree files
```

Pre-merge (two commits still on the branch) the equivalent is `git revert --no-commit <add-sha> <rm-sha> && git commit` followed by the same four local-state lines. **Every sibling worktree needs the local-state lines run independently** — a worktree keeps its own submodule clone under `<main>/.git/worktrees/<wt>/modules/agent-layer`, so clearing the main repo's copy leaves siblings stale.

**Accept (row 11):** `git ls-files | grep -c '^agents/'` is 0; `git submodule status` shows a clean pin with no `+`/`-` prefix; `diff <(sed 's|^|agent-layer/|' /tmp/pre-flip-inventory.txt | sort) <(git -C agent-layer ls-files | sed 's|^|agent-layer/|' | sort)` shows only the seed-time additions; `ls .claude/agents/*.md | wc -l` matches the pre-flip count; every command in 11c exits 0.

12. `scripts/dev/project-config.sh` (or `project.config.json` § paths) — `AGENT_LAYER_ROOT=agent-layer`; `PROJECT_ROOT` stays the Smatchet root.

Prefer the **config** form over a literal in the script — a new `"paths": { "agent_layer_root": "agent-layer" }` block, default `"."`, read by `project-config.sh` and emitted as `PC_AGENT_LAYER_ROOT`. That keeps the mount point data rather than code, which is what makes the mirrored copy in 8g byte-identical across both repos (the layer's own config sets `"."`). Top-level `additionalProperties: false` means `paths` must be added to `project.config.schema.json § properties` in the same PR, and `test-config-globs.sh --check` must be taught that `paths.*` is a directory, not a glob.

12a. **Host callers of `merge-gates.sh` must pin the config explicitly.** Per 9e the script's default resolves to the layer's config once it lives inside the submodule. Export `MERGE_GATES_CONFIG_FILE="$PROJECT_ROOT/project.config.json"` at every host invocation site — `git-janitor` / the `git-cleanup-procedures` skill, `smatchet-merge-watcher`, `merge-gates-prompt.sh`, and the orchestrator's documented one-liner — and add a regression assertion to `tests/bats/merge_gates.bats` that an unset `MERGE_GATES_CONFIG_FILE` in a submodule-shaped layout resolves to the *superproject* config. Silent-degradation class: without this the required-absent detector is inert on every Smatchet PR and nothing reds.

12b. New `.github/workflows/agent-layer-integration.yml` (host) — path-filtered on `agent-layer` + `.gitmodules`: checkout with `submodules: recursive`, run `check-harness-provisioned.sh` + `setup-harness.sh` + the host-side gate scripts through the submodule. This is the binding check for pointer-bump PRs (grill decision 3 — without it a bump PR changes only the gitlink and merges on docs-tier CI alone).

Add `workflow_dispatch` with a `layer_ref` input (default `develop`) so a layer change can be validated against real Smatchet content **before** the layer PR merges: the lane checks out the host, then `git -C agent-layer fetch origin <layer_ref> && git -C agent-layer checkout FETCH_HEAD` (uncommitted — the gitlink is never touched), then runs the same steps. Without it there is no pre-merge host validation path at all and the first signal on a layer change arrives after it is already merged. Steps: `check-harness-provisioned.sh`, `setup-harness.sh claude-code`, the 8g `cmp` drift gate, `test-agent-discovery-fixture.sh`, `test-docs.sh`, `test-all.sh --ci`, `plan-lock-gate.sh`. `test-required-context-parity.sh` applies — if this becomes a required context it must emit unconditionally, so gate the *work* on the path filter, never the job's existence.

12c. Pointer-bump docs-tier classification — the `is-pure-docs-diff.sh` allow-list must learn `agent-layer` + `.gitmodules`; the edit lands in the **layer repo** (the script moves in Phase B), so the full spec + acceptance live in Phase D row 17b. Noted here because the flip creates the need — the first post-flip bump PR rides the full build gate until 17b lands.

13. `AGENTS.md` (root) — becomes a thin stub: harness auto-load entry point, `@agent-layer/AGENTS.md`-style import for Claude Code via the regenerated `.claude/CLAUDE.md`, prose pointer for Codex/others. Root stub stays under the 150-line cap trivially.

13a. **The stub breaks every `AGENTS.md § <section>` cross-reference unless `test-doc-anchors.sh` is re-pointed.** That gate resolves each `AGENTS.md § X` reference against the root file's headings; once the headings live in `agent-layer/AGENTS.md` the whole corpus of references in agent prompts, rule-docs and plan files dangles at once. Phase A must teach `test_doc_anchors.py` to resolve the anchor set from `$AGENT_LAYER_ROOT/AGENTS.md` (falling back to the root file when the two are the same path, which is the pre-flip no-op). **Accept:** `bash agent-layer/agents/scripts/core/test-doc-anchors.sh` exits 0 post-flip with zero baseline additions.

13b. Stub contents (≈ 30 lines): what the repo is; the one-line mount contract; the `git submodule update --init --recursive` bootstrap; the fork URL-override line from 11b; "the canonical rules live in `agent-layer/AGENTS.md` — every `AGENTS.md § <section>` reference resolves there"; the host-only pointers that must stay reachable from the root (`AI_POLICY.md`, `CONTEXT-MAP.md`, `project.config.json`, `docs/plans/`). It carries no rule detail — `agent_size_audit.py` still gates it at 150 lines and the `SMATCHET_DEVIATION(rule=agent-too-long; …)` marker must **not** be carried over.

14. `.claude/CLAUDE.md` template — import path `../agent-layer/AGENTS.md`.

The template is **not** inline in `setup-harness.sh`; that script only does `copy_template "docs/harness/claude-code/CLAUDE.md.tmpl" ".claude/CLAUDE.md"`. The file to edit is the tracked `docs/harness/claude-code/CLAUDE.md.tmpl`, whose line 7 is the literal `@../AGENTS.md`. It needs **both** imports — the root stub (host-specific pointers) and the real rulebook, because Claude Code expands `@` imports inside `CLAUDE.md` only, so a stub that merely *mentions* `agent-layer/AGENTS.md` in prose is never loaded:

```markdown
@../AGENTS.md
@../agent-layer/AGENTS.md
```

14a. **Sequencing constraint — this edit lands in the layer repo, not in the flip PR.** `docs/harness/` moves in Phase B, so the tmpl is layer content by the time row 14 applies. It must therefore be committed in the layer (either folded into the seed scaffold, row 8f phase 4, or as the layer's first PR), and the flip PR's `git -C agent-layer checkout <sha>` in 11a step 2 must pin a SHA that **contains it**. Pinning the bare seed SHA ships a `.claude/CLAUDE.md` importing a stub with no rulebook behind it — a silent, total loss of the rules for every Claude Code session, with no gate that reds. **Accept:** post-flip, `grep -c 'agent-layer/AGENTS.md' .claude/CLAUDE.md` is 1 and `bash agent-layer/agents/scripts/core/test-agent-discovery-fixture.sh` exits 0.

14b. **Hardlink staleness is a standing hazard, not a one-time flip step.** `link_agents()` materializes `.claude/agents` as per-file hardlinks (`mklink //H`), and `git submodule update` writes fresh inodes — so an existing long-lived worktree that pulls and updates keeps hardlinks to the *old* content until `setup-harness.sh` runs again, and `check-harness-provisioned.sh` cannot see it (it probes for absence, not content drift). The flip PR's 11a step 3 covers the flip itself; the durable fix is Phase D row 20's process rule plus a content check (`cmp` per linked file) added to `check-harness-provisioned.sh` (Phase A row 5e).

15. Cross-boundary markdown-link sweep — run `test-markdown-links.sh` from both repos; fix host→layer links to `agent-layer/…` and layer→host links via the documented superproject convention (`../` from submodule root); baseline the residue that must wait for full de-Smatchet-ification.

Use the checker as the **enumerator**, never grep: `bash agent-layer/agents/scripts/core/test-markdown-links.sh --all` host-side and the layer's own copy layer-side. Its default mode is diff-scoped (it grandfathers by scope and only checks markdown the change touched), which is exactly wrong for a move of this size — `--all` is the census. Then rebaseline: `docs/high-integrity/markdown-link-baseline.md` is keyed `source::href` (not by line), so a re-key is only needed for links whose *href* changed, and `--all` reports any already-fixed entry as stale so the residue list stays honest. The layer repo gets its own baseline file, seeded from its first `--all` run.

**Accept (row 15):** `--all` in both repos reports zero non-baselined dangling links; the host baseline's diff in this PR contains no entry whose href still starts with `agents/`, `docs/agent-rules/` or `docs/harness/` without the `agent-layer/` prefix.

16. Delta-gate baselines that key on paths (`portable-purity-baseline.txt`, agent-size grandfather list `docs/high-integrity/agent-size-baseline.md`, include-cycle baseline untouched) — regenerate where the move re-keys entries; one-time, in the flip PR (layer-side copies live in the new repo after Phase B).

16a. **Two of the ten baselines move; the other eight stay but carry a stale regen command.** `docs/high-integrity/` holds ten auto-generated files and **every one** hardcodes its regeneration command as a literal `agents/scripts/...` path in its header, all of which 404 post-flip (there is no host-root `agents/` compat shim). Class A — data rows key on layer paths, so the file itself moves to the layer (already in row 8c's seed list, removed host-side in 11a step 1):

| File | Regen command (rewrite target: prefix `agent-layer/`) |
|---|---|
| `portable-purity-baseline.txt` | `bash agents/scripts/core/test-portable-purity.sh --baseline` |
| `agent-size-baseline.md` | `bash agents/scripts/project/test-lint-rules.sh --agentsize-baseline` |

Class B — data stays host-side (C++, plans, links); only the header prose is stale. Rewrite each line-3 command to the `agent-layer/`-prefixed form in this PR:

| File | Stale command in its header |
|---|---|
| `baseline.md` | `bash agents/scripts/project/test-lint-rules.sh --catalog --refresh` |
| `dup-baseline.md` | `bash agents/scripts/project/test-lint-rules.sh --dup-baseline` |
| `function-size-baseline.md` | `bash agents/scripts/project/test-lint-rules.sh --funcsize-baseline` |
| `include-cycle-baseline.md` | `bash agents/scripts/project/test-lint-rules.sh --include-cycle-baseline` |
| `dead-export-baseline.md` | `bash agents/scripts/core/test-dead-export-audit.sh --baseline` |
| `small-helper-baseline.md` | `bash agents/scripts/core/test-small-helper-audit.sh --baseline` |
| `markdown-link-baseline.md` | `bash agents/scripts/core/test-markdown-links.sh --baseline` |
| `plan-claim-anchor-baseline.md` | `agents/scripts/core/test-plan-claim-anchors.sh --baseline` |

16b. **Sweep for the same staleness class outside `docs/high-integrity/`.** The header-comment defect is "a doc hardcodes an `agents/scripts/...` invocation as copy-paste instructions"; the baselines are the densest cluster but not the only one. Run `grep -rn 'bash agents/scripts/' --include='*.md' .` after 11a step 1 and rewrite or `$AGENT_LAYER_ROOT`-parameterize each hit; this is a subset of the Phase A exhaustive sweep, re-run here because the flip is what makes the paths wrong.

**Accept (row 16):** every command printed in a `docs/high-integrity/*` header executes from a fresh post-flip checkout; re-running each regen command produces a zero diff (proves the baselines are actually in sync, not merely path-corrected); `test-portable-purity.sh` and `agent_size_audit.py --diff origin/develop` both green in the layer repo, and neither is referenced from a host workflow any more.
