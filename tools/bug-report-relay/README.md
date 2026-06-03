# Smatchet bug-report relay

A tiny [Cloudflare Worker](https://developers.cloudflare.com/workers/) that lets
**external users** file Smatchet bug reports **without the app ever shipping a
GitHub token**. The desktop app POSTs a redacted report to this relay; the relay
re-files it as a GitHub issue under a token held server-side.

This is the secure alternative to embedding a token in the binary — an embedded
token is extractable by anyone with the binary and is a published credential the
moment you distribute. See `docs/plans/shipped/log-a-bug-github.md` and the P1
entry in `docs/self-improvement/categories/security.md`.

```text
Smatchet app ──(redacted report JSON, optional x-relay-key)──▶ Worker ──(server PAT)──▶ GitHub
```

## What the token needs

The server-held token (a dedicated **bot account**, not a personal one) needs on
the destination repo:

- **Issues: write** — required (create the issue).
- **Contents: write** — only if you want inline screenshots (the Worker creates a
  `bug-report-assets` branch and uploads PNGs). Without it, screenshots are
  dropped with a note; the issue still files.

Because the token lives only on Cloudflare, you can **rotate it without
re-shipping the app**, and a leak can't happen via the binary.

## Deploy

Prereqs: a (free) Cloudflare account + Node.

```bash
cd tools/bug-report-relay
npm install
npx wrangler login

# 1. Point the relay at your dev repo (edit wrangler.toml [vars].REPO,
#    optionally ASSETS_REPO).

# 2. Set the GitHub token as a SECRET (never in wrangler.toml / git):
npx wrangler secret put GITHUB_TOKEN
#    paste the bot PAT when prompted.

# 3. (Recommended) set a shared access key so the relay isn't fully open:
npx wrangler secret put RELAY_KEY
#    pick any random string.

# 4. Ship it:
npx wrangler deploy
#    → prints https://smatchet-bug-report-relay.<your-subdomain>.workers.dev
```

> **Two secrets — do not confuse them.**
>
> | Secret | What it is | Lives where | Sent by clients? |
> |---|---|---|---|
> | `GITHUB_TOKEN` | the GitHub PAT | **only on Cloudflare** (`wrangler secret`) | **never** |
> | `RELAY_KEY` | a random access key you invent | Cloudflare **and** the app/config | **yes** (`x-relay-key` header / `bugreport_relay_key`) |
>
> The `RELAY_KEY` is the one that goes in the `x-relay-key` header and the app
> config. **Never** put the GitHub PAT in that header — the whole point is that the
> PAT stays on Cloudflare. Secrets are write-only; if you forget `RELAY_KEY`, just
> `wrangler secret put RELAY_KEY` again with a new value.
>
> **The Worker bundles `wrangler.toml` `[vars]` at deploy time.** After editing
> `REPO` / `ASSETS_REPO`, you **must** `npx wrangler deploy` again for the change to
> take effect. Same for any `src/index.js` edit.

## Point the app at it

Set these in `smatchet_config.json` (in the app's user-data dir) — and ship that
config (or a default) with your distributed build:

```json
"bugreport_relay_url": "https://smatchet-bug-report-relay.<sub>.workers.dev/report",
"bugreport_relay_key": "<the RELAY_KEY you set, or empty if none>"
```

When `bugreport_relay_url` is set, the app uses the relay and **ignores the local
owner/repo/PAT path entirely** — no GitHub token is needed on the client. The
`bugreport_relay_key` is *not* a GitHub credential; it only gates access to your
relay and is rate-limited + rotatable server-side, so bundling it is acceptable.

## Endpoints

- `POST /report` — body `{ title, body, screenshotBase64?, censored? }`; returns
  `{ ok, issueKey: "owner/repo#N", url }`. Requires `x-relay-key` when `RELAY_KEY`
  is set. Payload hard-capped at 2 MB (base64 screenshots inflate ~4/3; the app downscales captures to 1280px first).
- `GET /health` — `{ ok: true }`.

## Abuse protection

- Set `RELAY_KEY` so the endpoint isn't open to the world.
- Add a [Cloudflare Rate Limiting rule](https://developers.cloudflare.com/waf/rate-limiting-rules/)
  on the Worker route (e.g. N requests/min/IP) — the free tier covers basic rules.
- The 2 MB payload cap bounds screenshot size (the app downscales to 1280px before upload).
- Revoke/rotate the bot token any time from GitHub without touching the app.

## Local test

```bash
npx wrangler dev   # serves on http://localhost:8787
curl -s localhost:8787/health
curl -s -X POST localhost:8787/report -H 'content-type: application/json' \
  -H 'x-relay-key: <key>' \
  -d '{"title":"[Bug] test","body":"hello from curl"}'
```

## Verify a live deployment

Run against the deployed URL (note the host is
`<worker-name>.<your-subdomain>.workers.dev`, **not** a bare `<name>.workers.dev`).

**bash / `curl.exe`:**

```bash
# 1. Reachability (no side effect)
curl -s https://<worker>.<sub>.workers.dev/health        # → {"ok":true}

# 2. End-to-end — files a real issue (safe to close afterwards)
curl -s -X POST https://<worker>.<sub>.workers.dev/report \
  -H 'content-type: application/json' -H 'x-relay-key: <RELAY_KEY>' \
  -d '{"title":"[Bug] relay smoke test","body":"safe to close"}'
# → {"ok":true,"issueKey":"owner/repo#N","url":"https://github.com/..."}
```

**PowerShell** — `curl` is an alias for `Invoke-WebRequest`, so its `-s`/`-w`/`-d`
flags break. Use `curl.exe` (note the `.exe`) or `Invoke-RestMethod`:

```powershell
$b = '{"title":"[Bug] relay smoke test","body":"safe to close"}'
Invoke-RestMethod -Method Post `
  -Uri "https://<worker>.<sub>.workers.dev/report" `
  -Headers @{ "x-relay-key" = "<RELAY_KEY>" } `
  -ContentType "application/json" -Body $b | ConvertTo-Json
```

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `{"ok":false,"error":"bad or missing relay key"}` (401) | `RELAY_KEY` is set but the request's `x-relay-key` is missing/wrong | Send the matching key. This 401 *confirms* the auth gate works. |
| `{"ok":false,"error":"GitHub create failed: Not Found"}` (the relay returns 502) | `REPO` points at a repo that **doesn't exist**, or the `GITHUB_TOKEN` lacks access (GitHub returns 404, not 403, for no-access to hide existence) | Verify `REPO` in `wrangler.toml` is a real `owner/repo`; ensure the bot token has **Issues: write** on it (a private repo needs the bot added as a collaborator). Redeploy after editing `REPO`. |
| `{"ok":false,"error":"payload too large"}` (413) | base64 screenshot exceeds the cap | The app downscales captures to 1280px (≈ well under the 2 MB cap). If you see this, redeploy a Worker built from current `src/index.js` (the cap was raised from 256 KB → 2 MB). |
| `relay REPO var not configured as owner/repo` (500) | `REPO` unset or malformed | Set `[vars].REPO = "owner/repo"` in `wrangler.toml`, redeploy. |
| Screenshot doesn't render inline; issue still files | Token lacks **Contents: write**, so the asset upload is skipped | Grant Contents: write on the assets repo, or accept text-only reports. |
| `Could not resolve host` | Wrong URL (bare `<name>.workers.dev`) | Use the full `<worker-name>.<your-subdomain>.workers.dev` that `wrangler deploy` printed. |
| Edited `REPO` / `index.js` but behaviour unchanged | Vars + code are bundled at deploy time | `npx wrangler deploy` again. |

Verified working 2026-05-30 against a live Worker (filed `alexandrosk0/Smatchet#585`).
