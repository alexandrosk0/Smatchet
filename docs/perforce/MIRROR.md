# Perforce — GitHub → p4d one-way mirror (Helix4Git Git Connector)

> **Plan**: [`docs/plans/active/p4-git-connector-github-mirror.md`](../plans/active/p4-git-connector-github-mirror.md).
> **Sibling docs**: [`SETUP.md`](SETUP.md) — Phase-0 server bring-up (prerequisite). [`AGENT_FLOWS.md`](AGENT_FLOWS.md) — topology + which verb when. [`RUNBOOK.md`](RUNBOOK.md) — day-to-day ops.
> **Audience**: operator standing up (or tearing down) the read-only GitHub mirror on the dev box. Agents do **not** read or write the mirror — it is non-authoritative and off the ship-line.

The mirror replicates `github.com/alexandrosk0/Smatchet` **into** the Windows `p4d` server (`Mainbot:1666`) as a **read-only, git-native** copy stored in a **graph depot** (`//repo/smatchet`) — real git objects + SHAs, not flattened to classic Perforce revisions.

**Invariant — one-way, non-authoritative.** GitHub stays canonical (PR review, CI, `smatchet-merge-watcher`). Data flows **GitHub → p4d only**; nothing is ever pushed `//repo/smatchet` → GitHub, and the graph depot is never a review/merge source. Losing it is annoyance, not data loss (mirrors [`RUNBOOK.md`](RUNBOOK.md) line 7 for the classic depot). This is purely additive to the dual-VCS topology in [`AGENT_FLOWS.md`](AGENT_FLOWS.md) § Topology.

## Why a graph depot (not a classic `p4 reconcile` mirror)

A graph depot is the **only** mechanism that stores the mirror as real git objects (commit SHAs + history preserved). A classic-depot `p4 reconcile` mirror would flatten history into Perforce revisions and lose SHA fidelity. The trade-off: the connector that populates a graph depot (`gconn`) is **Linux-only** (no Windows binary), so it runs on **WSL2 Ubuntu on the same Windows box** as `p4d`, reaching `p4d` over `P4PORT`. The graph depot itself lives inside the cross-platform Windows `p4d`.

## Prerequisites

1. **Phase-0 server up** — `p4 info` against `Mainbot:1666` succeeds per [`SETUP.md`](SETUP.md) § Smoke test.
2. **Graph depots available** — confirmed: this server's `p4d` 2025.2 supports graph depots on the **free (unlicensed) tier** — `p4 depot -t graph` succeeds with `Server license: none`. No paid Helix4Git license is required for a single mirror repo (free tier ≤ 5 users / ≤ 10 repos). Verify:
   ```bash
   p4 -p Mainbot:1666 -u alexk depots -t graph
   # Expect a row: Depot repo ... graph repo/... 'Default graph depot'
   ```
3. **`//repo` graph depot already exists** (created 2026-05-21, empty — 0 repos). The depot-create step below is therefore idempotent: verify-then-create-if-absent.

## 1. Server side (Windows `p4d`) — graph depot + repo spec + service user

Run from any `p4` client reaching `Mainbot:1666` (e.g. the dev box).

```bash
# (a) Graph depot — create ONLY if `p4 depots -t graph` did not already list `repo`.
p4 -p Mainbot:1666 -u alexk depots -t graph | grep -q '^Depot repo ' \
  || printf 'Depot:\trepo\nOwner:\talexk\nType:\tgraph\nMap:\trepo/...\n' \
       | p4 -p Mainbot:1666 -u alexk depot -i

# (b) Repo spec for the mirror target.
printf 'Repo:\t//repo/smatchet\nOwner:\tgconn\n' | p4 -p Mainbot:1666 -u alexk repo -i
p4 -p Mainbot:1666 -u alexk repos          # should now list //repo/smatchet

# (c) Low-privilege service user the connector authenticates as.
printf 'User:\tgconn\nEmail:\tgconn@localhost\nFullName:\tGit Connector service\n' \
  | p4 -p Mainbot:1666 -u alexk user -f -i
```

Grant `gconn` write **only** on `//repo/...` (it must never touch `//smatchet/...`, the classic agentic-WIP depot). Append to `p4 protect`:

```
write   user   gconn   *   //repo/...
list    user   gconn   *   //repo/...
```

`//smatchet` (classic stream depot) and `//repo` (graph mirror) are two depots, two purposes, zero interference.

## 2. WSL2 connector host

```bash
# In an elevated Windows PowerShell, once:
wsl --install -d Ubuntu-22.04        # reboot if first-ever WSL install
```

Inside the Ubuntu shell:

```bash
sudo apt-get update
sudo apt-get install -y git git-lfs        # git >= 1.8.5, git-lfs >= 1.1.0 (lfs REQUIRED for third-party mirroring)
git lfs install --skip-repo
```

Install the **Helix Git Connector** package matching the **2025.2** server line (version-match the connector to the `p4d` line per the Helix4Git support matrix — a connector newer/older than the server can refuse the graph protocol):

- Download: <https://www.perforce.com/downloads/helix-core-git-connector> (Linux `.deb`, x86_64).
- Install per the P4SAG: <https://help.perforce.com/helix-core/server-apps/p4sag/current/Content/P4SAG/install-config.system.html>.

**Reachability pre-flight** (WSL2 → Windows `p4d`) — WSL2's NAT must reach the Windows host's `:1666`:

```bash
# From inside WSL2; <windows-host> is the LAN name/IP of the box, e.g. Mainbot.
P4PORT=<windows-host>:1666 p4 -u gconn info     # must print Server address / version
```

The LAN firewall rule already exists ([`SETUP.md`](SETUP.md) § 7, Private/Domain profile). If WSL2 can't reach it, add the host's LAN IP explicitly (`cat /etc/resolv.conf` gives the Windows host IP under default WSL2 NAT).

## 3. GitHub authentication — read-only Deploy Key

The mirror needs **read** on GitHub. Use a least-privilege, single-repo **read-only Deploy Key** (operator's chosen auth):

```bash
# In WSL2, generate a dedicated keypair OUTSIDE the repo tree:
ssh-keygen -t ed25519 -C "smatchet-mirror-deploy-key" -f ~/.ssh/smatchet_mirror -N ""
cat ~/.ssh/smatchet_mirror.pub
```

Add the **public** key at `https://github.com/alexandrosk0/Smatchet/settings/keys` → *Add deploy key* → **leave "Allow write access" UNCHECKED** (read-only). Then pin it for the mirror remote:

```sshconfig
# ~/.ssh/config
Host github-smatchet-mirror
    HostName github.com
    User git
    IdentityFile ~/.ssh/smatchet_mirror
    IdentitiesOnly yes
```

The connector's upstream GitHub URL is then `git@github-smatchet-mirror:alexandrosk0/Smatchet.git`.

> **Note — the repo is currently PUBLIC.** Anonymous `https://github.com/alexandrosk0/Smatchet` read works today with no credential at all. The Deploy Key is kept anyway because it (a) future-proofs a flip to private with zero re-plumbing and (b) avoids unauthenticated API/clone rate limits. If you prefer the simplest path while public, point the connector at the public HTTPS URL and skip this section — the health-check works either way.

## 4. Configure the one-way mirror

Point the connector at the GitHub upstream in **mirror (read-only) mode**, targeting `//repo/smatchet`. The exact connector config keys are **version-sensitive** — follow the P4SAG "Mirror a repo from an external repository" for your connector build:
<https://help.perforce.com/helix-core/server-apps/p4sag/current/Content/P4SAG/overview.architecture.html> (architecture) + the connector package's `Mirroring` section.

Config shape (fill the version-specific keys from the P4SAG):
- **upstream URL** = `git@github-smatchet-mirror:alexandrosk0/Smatchet.git` (or the public HTTPS URL while public),
- **mirror** = on / read-only (never push back),
- **target graph repo** = `//repo/smatchet`,
- **refs** = `develop` + tags (per plan Non-goals: not every PR branch).

Initial population:

```bash
# Trigger the first fetch (exact verb per your connector build), then verify server-side:
p4 -p Mainbot:1666 -u alexk repos                       # //repo/smatchet listed
git ls-remote <connector-smart-http-base>/repo/smatchet refs/heads/develop   # SHA appears
```

## 5. Cadence — pull-based fetch + WSL2 lifecycle

GitHub Actions **cannot** reach a local `p4d` ([`AGENT_FLOWS.md`](AGENT_FLOWS.md) § When NOT to use Perforce), so freshness is **pull-based**: a WSL2 cron triggers the connector fetch.

```cron
# crontab -e inside WSL2 — fetch every 15 min, then health-check.
*/15 * * * * /opt/perforce/git-connector/bin/<fetch-verb> >/dev/null 2>&1 ; \
             MIRROR_REMOTE=http://localhost:1680 \
             /mnt/c/Dev/Smatchet/scripts/dev/p4-mirror-healthcheck.sh \
             >> ~/smatchet-mirror-health.log 2>&1
```

**WSL2 auto-start** (else the cron never fires after a reboot): register a Windows Task-Scheduler logon task running `wsl -d Ubuntu-22.04 -e true` (boots the distro), or enable systemd-in-WSL with the connector as a service. Without this, the mirror silently staledates — which the health-check below makes loud.

## 6. Health-check — `scripts/dev/p4-mirror-healthcheck.sh`

Asserts the graph-depot `develop` SHA == GitHub's `develop` SHA. Exit 0 = in sync; non-zero + diagnostic = drift / staleness / unreachable.

```bash
# Primary (connector serves smart-http):
MIRROR_REMOTE=http://localhost:1680 scripts/dev/p4-mirror-healthcheck.sh

# Fallback (smart-http not served) — query the graph depot directly via p4:
MIRROR_RESOLVE=p4 scripts/dev/p4-mirror-healthcheck.sh
```

Env knobs: `GITHUB_REMOTE`, `MIRROR_REMOTE`, `MIRROR_REPO_PATH` (default `repo/smatchet`), `MIRROR_REF` (default `develop`), `MIRROR_RESOLVE` (`git`|`p4`). Tests: [`tests/bats/p4_mirror_healthcheck.bats`](../../tests/bats/p4_mirror_healthcheck.bats).

## 7. Teardown

```bash
p4 -p Mainbot:1666 -u alexk repo -d //repo/smatchet     # drop the repo spec
# (optionally) drop the empty graph depot:
p4 -p Mainbot:1666 -u alexk depot -d repo
```

Then in WSL2: stop the cron + connector, `rm ~/.ssh/smatchet_mirror*`, and **revoke the Deploy Key** at `https://github.com/alexandrosk0/Smatchet/settings/keys`.

## Known issues

- **Graph-depot delete hits a server lock-order abort.** `p4 depot -d <graph-depot>` on this `p4d` 2025.2 fails reproducibly with `Locking failure: db.counters locked after db.group!` (independent of `-f`). Root cause is the server's lockless-read lock-order check (`db.peeking`) on the depot-delete path. Workaround: set `p4 configure set db.peeking=0`, **restart `p4d`**, delete, then restore `db.peeking=2` + restart. Do this only in a maintenance window — it is disproportionate for routine teardown, so prefer leaving an empty graph depot in place (zero repos = zero data). The connector's steady-state writes never touch this delete path, so the mirror is unaffected.
- **Stray probe depot `testgraphprobe`.** A `p4 depot -t graph` capability probe (2026-06-08) left an **empty** graph depot `testgraphprobe` that the lock-order bug above blocks deleting remotely. Harmless (0 repos, 0 data, no effect on `//repo` or `//smatchet`). Clear it during the next `db.peeking` maintenance window, or from the server host.
- **WSL2 lifecycle** is the most likely staleness source (no auto-start after reboot). The health-check + its cron log are the detection net.
