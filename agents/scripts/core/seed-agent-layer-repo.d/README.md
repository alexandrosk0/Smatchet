# The Unwilling Agentic Bunch

A portable **agent layer**: agent prompts, shared skills, rule-docs, harness adapters, the
gate scripts that enforce them, and the self-improvement framework spec. It is consumed as
a **git submodule** mounted at `agent-layer/` in a host project, and it runs its own gates
on itself — this repo is its own first consumer.

There is no product code here, by design. What lives here is the machinery that reviews,
gates and ships product code somewhere else.

## Bootstrap

```bash
git submodule add https://github.com/alexandrosk0/the-unwilling-agentic-bunch.git agent-layer
git submodule update --init --recursive
bash agent-layer/agents/scripts/core/setup-harness.sh claude-code
bash agent-layer/agents/scripts/core/check-harness-provisioned.sh
```

Every clone and every CI checkout needs `--recursive` (or a `submodules: recursive` step).
A checkout that forgets it leaves `agent-layer/` empty; `check-harness-provisioned.sh`
exists to fail loudly on exactly that rather than letting gates silently no-op.

Each **git worktree** needs its own `git submodule update --init` — worktrees share
`.git/modules` storage but not the checkout.

## The dual-root contract

Two roots, because the layer reads two different trees and must keep them straight:

| Variable | Points at | Holds |
|---|---|---|
| `PROJECT_ROOT` | the **superproject** (host) working tree | `docs/plans/`, self-improvement entries, backlog, product source |
| `AGENT_LAYER_ROOT` | **this** repo's working tree | agent prompts, skills, rule-docs, harness adapters, gate scripts |

Both are exported by `scripts/dev/project-config.sh`. A script that reads *layer* content
resolves through `AGENT_LAYER_ROOT`; a script that reads *host* content resolves through
`PROJECT_ROOT`. That split is the whole reason this repo can also run **standalone** — its
own CI just sets `PROJECT_ROOT=$AGENT_LAYER_ROOT` and points the framework at itself.

### Config resolution order

The host provides `project.config.json` at the **superproject root**. `project-config.sh`
resolves it in this order, first hit wins:

1. `$PC_CONFIG_FILE` — an explicit file path. Strictly more specific than a root, so it
   wins outright, *including* its "not found" failure.
2. `$SMATCHET_PROJECT_CONFIG` — environment override.
3. The **superproject** root, via `git rev-parse --show-superproject-working-tree` — this
   is the rung that works from inside the mounted submodule.
4. This repo's own root — standalone mode, using the `project.config.json` shipped here.

## `project.config.json` — what the copy in this repo is, and is not

The file at this repo's root is a **real, working config**, not a sample: the standalone CI
lanes run against it. It therefore doubles as the reference example — but specifically as
the reference for a **consumer with no product code**.

It sets `"profile": "agent-layer"` and omits `build` and `perf` entirely, because a repo
with no CMake presets, no build targets and no frame budget cannot fill them honestly.
A consumer **with** product code does the opposite: it drops `profile` (or sets it to
`"product"`) and fills both blocks — they are required in that case, and each block's own
inner `required` and `minItems` still apply once declared.

For a fully-worked product config, read the host's own
[`Smatchet/project.config.json`](https://github.com/alexandrosk0/Smatchet/blob/develop/project.config.json),
not the one here.

### Schema validation is the host's job

No `project.config.schema.json` ships beside the config in this repo. `project-config.sh`'s
no-deps required-key gate skips silently when the schema file is absent, so **standalone
validation of this file is inert by construction** — and that is a known gap, not a
feature. Do not set `PC_SCHEMA_FILE` here to paper over it.

Real JSON-Schema validation of both configs runs **host-side**, in the host's
`doc-validation.yml` and `agent-layer-integration.yml` — the only lanes where both trees
are checked out at once.

## The submodule working tree is not a write target

Anything written under `agent-layer/` from a host session is **discarded** by the next
`git submodule update`. Edits to the layer are made in a clone of *this* repo, shipped as a
PR here, and reach the host as a pointer bump.

The loop, per edit:

1. PR in this repo, gated by this repo's own CI.
2. A `chore(agent-layer): bump to <sha>` PR in the host, opened automatically on merge.
3. The host's `agent-layer-integration.yml` lane is the bump PR's binding check — a bump
   changes only the gitlink, so no path-filtered host lane would otherwise fire.

Some files are deliberately **dual-homed** — `scripts/dev/project-config.sh`,
`scripts/dev/test-all.sh`, `scripts/dev/test-docs.sh`. The host must build with no
dependency on this tree, so it keeps its own copies. **The copy here is canonical**; the
host copy is a byte-identical mirror, listed in `docs/mirrored-paths.txt` and held by a
`cmp -s` drift gate. Edit direction is layer-first: change the canonical copy here, and the
bump PR carries every mirrored host copy in the same diff. The gate reds until it does.

## Provenance

Seeded from the Smatchet repository with `git filter-repo`, history preserved. The exact
allowlist is committed as [`docs/seed-paths.txt`](docs/seed-paths.txt) and the per-path
publication verdict as [`docs/seed-audit.md`](docs/seed-audit.md).

The prose here is not yet fully project-neutral — de-Smatchet-ification of portable prose
is a tracked follow-up, not a precondition of the extraction. Residual host literals are
reported by a host-side scan rather than by this repo's own purity gate; see
`docs/seed-audit.md` for why that check lives where it does.

## Licence

MIT — see [`LICENSE`](LICENSE).
