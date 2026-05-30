# Smatchet bug-report relay

A tiny [Cloudflare Worker](https://developers.cloudflare.com/workers/) that lets
**external users** file Smatchet bug reports **without the app ever shipping a
GitHub token**. The desktop app POSTs a redacted report to this relay; the relay
re-files it as a GitHub issue under a token held server-side.

This is the secure alternative to embedding a token in the binary — an embedded
token is extractable by anyone with the binary and is a published credential the
moment you distribute. See `docs/plans/active/log-a-bug-github.md` and the P1
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
  is set. Payload hard-capped at 256 KB.
- `GET /health` — `{ ok: true }`.

## Abuse protection

- Set `RELAY_KEY` so the endpoint isn't open to the world.
- Add a [Cloudflare Rate Limiting rule](https://developers.cloudflare.com/waf/rate-limiting-rules/)
  on the Worker route (e.g. N requests/min/IP) — the free tier covers basic rules.
- The 256 KB payload cap bounds screenshot size.
- Revoke/rotate the bot token any time from GitHub without touching the app.

## Local test

```bash
npx wrangler dev   # serves on http://localhost:8787
curl -s localhost:8787/health
curl -s -X POST localhost:8787/report -H 'content-type: application/json' \
  -H 'x-relay-key: <key>' \
  -d '{"title":"[Bug] test","body":"hello from curl"}'
```
