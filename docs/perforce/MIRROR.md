# Perforce — GitHub → p4d one-way mirror (Helix4Git Git Connector)

> **Plan**: [`docs/plans/shipped/p4-git-connector-github-mirror.md`](../plans/shipped/p4-git-connector-github-mirror.md).
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

> **Scripted**: [`scripts/dev/p4-mirror-bootstrap.sh`](../../scripts/dev/p4-mirror-bootstrap.sh) codifies §§ 1–2 idempotently (verify-then-create the depot / repo spec / `gconn` user + grant, apt-install `git`/`git-lfs`, reachability pre-flight) and prints §§ 3–5's manual half as guided prompts. `--server-only` / `--host-only` split the two sides. Tests: [`tests/bats/p4_mirror_bootstrap.bats`](../../tests/bats/p4_mirror_bootstrap.bats). The sections below remain the reference for what the script does (and for hand-running any single step).

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

Grant `gconn` admin on the **graph depot** `repo`. Graph-depot ACLs are managed by `p4 grant-permission` — **not** the classic `p4 protect` table (that governs classic depots like `//smatchet`; a `protect` entry does nothing for a graph depot):

```bash
# Depot-wide grant: gconn admin on every repo under //repo/... .
p4 -p Mainbot:1666 -u alexk grant-permission -d repo -u gconn -p admin
# Verify (the //repo/smatchet owner grant is implicit from the repo spec's Owner: gconn):
p4 -p Mainbot:1666 -u alexk show-permission -d repo
#   //repo/...      * user  gconn admin
#   //repo/smatchet * owner gconn admin
```

`gconn` needs **no** entry in the classic `p4 protect` table — it only ever touches the graph depot. `//smatchet` (classic stream depot, agentic-WIP) and `//repo` (graph mirror) are two depots, two purposes, zero interference.

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

## 3. GitHub authentication — anonymous HTTPS (repo is public)

**As wired (this standup): anonymous HTTPS, no credential.** `alexandrosk0/Smatchet` is a **public** repo, so the connector reads it over the bare HTTPS URL `https://github.com/alexandrosk0/Smatchet.git` — no Deploy Key, no PAT, no SSH key, no GitHub-settings change. This is the simplest least-privilege path while the repo is public (read-only by construction — anonymous HTTPS cannot push), and it is what `gconn --mirrorhooks add` (§ 4) was pointed at.

### Optional — read-only Deploy Key (only if the repo flips to private)

If `alexandrosk0/Smatchet` is ever made private, anonymous HTTPS stops working and the mirror needs an explicit **read** credential. Use a least-privilege, single-repo **read-only Deploy Key** (not a broad PAT):

```bash
# In WSL2, generate a dedicated keypair OUTSIDE the repo tree:
ssh-keygen -t ed25519 -C "smatchet-mirror-deploy-key" -f ~/.ssh/smatchet_mirror -N ""
cat ~/.ssh/smatchet_mirror.pub
```

Add the **public** key at `https://github.com/alexandrosk0/Smatchet/settings/keys` → *Add deploy key* → **leave "Allow write access" UNCHECKED** (read-only). Then pin it for the mirror remote:

```sshconfig
# ~/.ssh/config (as the connector OS user `git`)
Host github-smatchet-mirror
    HostName github.com
    User git
    IdentityFile ~/.ssh/smatchet_mirror
    IdentitiesOnly yes
```

The connector's upstream URL would then become `git@github-smatchet-mirror:alexandrosk0/Smatchet.git` (re-point with `gconn --mirrorhooks setremote`, § 4). The private key stays **outside** the repo tree; revoke the Deploy Key on teardown (§ 7).

## 4. Configure the one-way mirror

The mirror is driven by the connector's **mirrorhooks** subsystem (`gconn --mirrorhooks`), pointed at the GitHub upstream. This standup uses a **minimal hand-rolled `gconn.conf`** rather than the package's `configure-git-connector.sh` — that script also stands up Apache + SSH to *serve* git over smart-http, which a pull-only backup mirror does not need (the connector only has to *fetch* from GitHub and *write* into `p4d`). Skipping it keeps the surface small; the cost is no smart-http endpoint, so the health-check uses its `MIRROR_RESOLVE=p4` path (§ 6).

> **Run as the connector OS user `git` — never root.** `gconn` refuses to run as root and trips git's dubious-ownership guard otherwise. The OS user is `git` (uid 1001, group `gconn-auth`); drive every `gconn` call via `runuser -u git -- env GCONN_CONFIG=… gconn …`.

**(a) Connector config** — `/opt/perforce/git-connector/gconn.conf` (owned `git:gconn-auth`). Minimal working shape:

```json
{
  "gconn": {
    "reposDir":      "/opt/perforce/git-connector/repos",
    "p4User":        "gconn",
    "p4Port":        "Mainbot:1666",
    "p4TicketsFile": "/opt/perforce/git-connector/.p4tickets",
    "p4TrustFile":   "/opt/perforce/git-connector/.p4trust",
    "authKeysFile":  "none",
    "gitExecPath":   "/usr/bin",
    "envPath":       "/usr/bin:/usr/local/bin:/opt/perforce/git-connector/bin",
    "authGroup":     "gconn-auth",
    "serverId":      "gconn-Brick"
  }
}
```

The connector authenticates to `p4d` as the **`gconn` p4 user** via the ticket in `p4TicketsFile` — ensure that ticket is valid before the first fetch (`p4 -p Mainbot:1666 -u gconn login`, writing to `/opt/perforce/git-connector/.p4tickets`; the free-tier server here uses a non-expiring ticket).

**(b) Register the mirror + initial population** — one command does both (registers the repo spec's `GconnMirror*` fields and performs the first fetch):

```bash
# As the connector OS user `git`. Upstream = public HTTPS (§ 3).
runuser -u git -- env GCONN_CONFIG=/opt/perforce/git-connector/gconn.conf \
  gconn --mirrorhooks add repo/smatchet https://github.com/alexandrosk0/Smatchet.git

# Confirm it registered:
runuser -u git -- env GCONN_CONFIG=/opt/perforce/git-connector/gconn.conf \
  gconn --mirrorhooks list
#   //repo/smatchet <<< https://github.com/alexandrosk0/Smatchet.git
```

**(c) Verify server-side** (from any `p4` client, e.g. Windows):

```bash
p4 -p Mainbot:1666 -u alexk repos                       # //repo/smatchet listed, "Mirror of …"
p4 -p Mainbot:1666 -u gconn graph log -n //repo/smatchet -m 1 develop   # tip commit SHA appears
```

> **Ref scope is the WHOLE repo, not just `develop` + tags.** `mirrorhooks` has **no ref-filter / exclude option** — it mirrors *every* ref GitHub serves (all `refs/heads/*`, `refs/tags/*`, and `refs/pull/*/head`). The plan's original "`develop` + tags only" scope is **not achievable** through the connector; the full-mirror consequence (ephemeral `refs/pull/*/merge` refs make the inner `git push --mirror` log a `256`, though the `--mirrorhooks fetch` verb itself still exits 0) is covered in § Known issues. `develop` — the only ref the health-check asserts — mirrors correctly regardless.

## 5. Cadence — pull-based fetch + WSL2 lifecycle

GitHub Actions **cannot** reach a local `p4d` ([`AGENT_FLOWS.md`](AGENT_FLOWS.md) § When NOT to use Perforce), so freshness is **pull-based**: a WSL2 cron triggers the connector fetch, then runs the health-check. **Wired live this standup** — cron daemon `active`, Windows logon task `SmatchetMirrorKeepWSL` `Running`.

The `cron` daemon must be running in WSL2 (systemd-in-WSL is simplest, already in place on this host): `[boot] systemd=true` in `/etc/wsl.conf`, `wsl --shutdown`, then `sudo systemctl enable --now cron`. The health-check script is **installed beside the connector** — `/opt/perforce/git-connector/p4-mirror-healthcheck.sh` — so the mirror infra is **self-contained** and does not depend on a developer checkout (the integration tree at `/mnt/c/Dev/Smatchet` legitimately floats across commits/branches and may not even contain the script on an older HEAD). Re-install after a script change with `install -m0755 <repo>/scripts/dev/p4-mirror-healthcheck.sh /opt/perforce/git-connector/`.

The cron lives in **root's** crontab (it uses `runuser -u git` for the fetch; absolute binaries + an explicit `PATH` because cron's default env omits `/usr/sbin`):

```cron
# root crontab inside WSL2 (`sudo crontab -e`) — fetch every 15 min, then health-check.
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
*/15 * * * * /usr/sbin/runuser -u git -- env GCONN_CONFIG=/opt/perforce/git-connector/gconn.conf /usr/bin/gconn --mirrorhooks fetch repo/smatchet >/dev/null 2>&1 || true ; MIRROR_RESOLVE=p4 P4PORT=Mainbot:1666 P4USER=gconn P4TICKETS=/opt/perforce/git-connector/.p4tickets /opt/perforce/git-connector/p4-mirror-healthcheck.sh >> /var/log/smatchet-mirror-health.log 2>&1
```

`gconn --mirrorhooks fetch` **exits 0** even though its inner `git push --mirror` logs `Return value: 256` for the ephemeral `refs/pull/*/merge` refs (§ Known issues) — the `|| true` is belt-and-suspenders, not load-bearing. `MIRROR_RESOLVE=p4` is used (not the smart-http `git` path) because this standup does not serve smart-http (§ 4). The health-check's `p4 graph log` reads the graph depot, which the `//repo/...` ACL grants the **`gconn`** p4 user — hence `P4USER=gconn` + the connector's ticket file.

**WSL2 auto-start** (else the cron never fires — WSL2 does not auto-start after a reboot, and its lightweight VM idle-shuts-down when no session is held, taking `cron` with it): a Windows Task-Scheduler **logon** task `SmatchetMirrorKeepWSL` runs

```text
conhost.exe --headless  wsl.exe -d Ubuntu -u root --exec /bin/sh -c "exec sleep infinity"
```

`--headless` keeps it windowless (Win11), and the `sleep infinity` holds one WSL session open so systemd — and thus `cron` — stays up continuously. The task restarts on failure (3×, 1-min interval) and runs only when the user is logged on (no stored password — `wsl -u root` is passwordless on this host). Without it the mirror silently staledates, which the health-check then makes loud.

## 6. Health-check — `scripts/dev/p4-mirror-healthcheck.sh`

Asserts the graph-depot `develop` SHA == GitHub's `develop` SHA. Exit 0 = in sync; non-zero + diagnostic = drift / staleness / unreachable.

```bash
# This standup (no smart-http served) — query the graph depot directly via p4.
# P4USER=gconn because the //repo/... graph ACL grants gconn (not the OS/login user).
MIRROR_RESOLVE=p4 P4PORT=Mainbot:1666 P4USER=gconn \
  P4TICKETS=/opt/perforce/git-connector/.p4tickets \
  scripts/dev/p4-mirror-healthcheck.sh

# Alternative, ONLY if you ran configure-git-connector.sh to serve smart-http:
MIRROR_REMOTE=http://localhost:1680 scripts/dev/p4-mirror-healthcheck.sh
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

- **The inner `git push --mirror` logs `Return value: 256` (ephemeral `refs/pull/*/merge`) — but `gconn --mirrorhooks fetch` itself exits 0.** GitHub auto-computes a `refs/pull/N/merge` ref for each open mergeable PR and prunes/recomputes them constantly. A `gconn --mirrorhooks fetch` grabs the current set, then `git push --mirror`-es into `p4d`; by the time the push runs, a handful of those `merge` refs point at commits GitHub has already pruned, so `p4d` rejects them (`Reference refs/pull/N/merge specifies a non-existent commit …`) and the **sub-push** returns **256** — surfaced only as a `remote:gconn: … Return value: 256` log line. **`git push --mirror` is per-ref, not atomic**, so every *stable* ref still updates — `develop`, all `refs/heads/*`, `refs/tags/*`, and the stable `refs/pull/*/head` refs sync correctly; only the ~4 volatile `*/merge` refs fail. Crucially the **`--mirrorhooks fetch` client verb wraps this and exits 0** (the 256 is the sub-push's return value, not the verb's — verified live: the run ends `Fetched content for repo 'repo/smatchet.git' …`, `$? == 0`). `mirrorhooks` has **no ref-filter / exclude option** (verified: verbs are add/remove/list/setremote/fetch only; the repo spec has no `ExcludedBranches` field), so the inner-push 256 is permanent while mirroring a repo with open PRs — but it is **cosmetic**: the cron's authoritative signal is the health-check's `develop`-SHA match. The cron's `|| true` (§ 5) is belt-and-suspenders should a future `gconn` ever propagate the sub-push code to the verb's exit.
- **Graph-depot delete hits a server lock-order abort.** `p4 depot -d <graph-depot>` on this `p4d` 2025.2 fails reproducibly with `Locking failure: db.counters locked after db.group!` (independent of `-f`). Root cause is the server's lockless-read lock-order check (`db.peeking`) on the depot-delete path. Workaround: set `p4 configure set db.peeking=0`, **restart `p4d`**, delete, then restore `db.peeking=2` + restart. Do this only in a maintenance window — it is disproportionate for routine teardown, so prefer leaving an empty graph depot in place (zero repos = zero data). The connector's steady-state writes never touch this delete path, so the mirror is unaffected.
- **Stray probe depot `testgraphprobe`.** A `p4 depot -t graph` capability probe (2026-06-08) left an **empty** graph depot `testgraphprobe` that the lock-order bug above blocks deleting remotely. Harmless (0 repos, 0 data, no effect on `//repo` or `//smatchet`). Clear it during the next `db.peeking` maintenance window, or from the server host.
- **WSL2 lifecycle** is the most likely staleness source (no auto-start after reboot). The health-check + its cron log are the detection net.
