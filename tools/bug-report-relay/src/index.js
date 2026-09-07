// Smatchet bug-report relay — Cloudflare Worker.
//
// Holds the GitHub token SERVER-SIDE so the desktop app never ships a credential
// (see docs/plans/shipped/log-a-bug-github.md + the P1 security backlog entry).
// The app POSTs a redacted report here; this Worker re-files it as a GitHub issue
// under a server-held PAT and returns { ok, issueKey, url } in the shape
// BugReportService::SubmitViaRelay expects.
//
// Deploy + secrets: see README.md. Required binding: secret GITHUB_TOKEN. Vars:
// REPO ("owner/repo"), optional ASSETS_REPO ("owner/repo"), optional RELAY_KEY
// (shared access key — when set, requests must send a matching x-relay-key header).
//
// The token here needs, on REPO/ASSETS_REPO: Issues:write (+ Contents:write only
// if screenshots are enabled). It is NEVER exposed to clients.

const GH_API = "https://api.github.com";
const ASSETS_BRANCH = "bug-report-assets";
const MAX_BODY_BYTES = 2 * 1024 * 1024; // hard cap on the whole request payload (base64 screenshot inflates ~4/3)
const UA = "Smatchet-BugReportRelay";
const RATE_LIMIT_RETRY_AFTER = "60"; // seconds; matches the bindings' `simple.period` in wrangler.toml

function ghHeaders(token) {
  return {
    Authorization: `Bearer ${token}`,
    Accept: "application/vnd.github+json",
    "X-GitHub-Api-Version": "2022-11-28",
    "User-Agent": UA,
    "Content-Type": "application/json",
  };
}

function json(status, obj) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

// One rate-limit bucket. `limiter` is a Cloudflare ratelimit binding (see the
// [[unsafe.bindings]] blocks in wrangler.toml); it is absent under `wrangler dev`
// on older versions and on a Worker deployed before those bindings existed, in
// which case there is nothing to enforce and the request passes.
//
// Fail-OPEN on a limiter error: the binding is abuse protection, not correctness,
// and a limiter outage must not take the bug reporter down with it. (The dump
// privacy check below is the opposite — it fails closed.)
async function withinRateLimit(limiter, key) {
  if (!limiter || typeof limiter.limit !== "function") return true;
  try {
    const { success } = await limiter.limit({ key });
    return success !== false;
  } catch {
    return true;
  }
}

// Per-IP + whole-relay budget on /report. Runs BEFORE the relay-key check so a
// key-guessing flood is throttled too. `CF-Connecting-IP` is set by Cloudflare's
// edge and cannot be spoofed by the client; the "unknown" fallback shares one
// bucket, which is the conservative side to err on.
async function rateLimitReport(request, env) {
  const ip = request.headers.get("CF-Connecting-IP") || "unknown";
  if (!(await withinRateLimit(env.REPORT_RATE_LIMITER, ip))) return "per-IP";
  if (!(await withinRateLimit(env.GLOBAL_RATE_LIMITER, "global"))) return "global";
  return "";
}

// Repo visibility, cached for the life of the isolate. A repo's public/private
// state changes ~never, and this sits on the crash-report path, so re-asking
// GitHub per request buys nothing and spends rate-limit budget.
const repoPrivateCache = new Map();

// A minidump carries the crashing thread's STACK MEMORY and the loaded-module
// list — file paths under the user's home directory, and whatever strings the
// crashing frames happened to be holding. A GitHub Release asset on a PUBLIC repo
// is world-downloadable with no auth, so uploading one there publishes a stranger's
// process state. Refuse unless the destination repo is private.
//
// Fails CLOSED: an unreadable/ambiguous visibility answer drops the dump. The
// issue itself still files (the caller degrades to a note in the body) — losing a
// dump costs a debugging session, publishing one cannot be undone.
async function assertDumpRepoPrivate(token, repo) {
  const cached = repoPrivateCache.get(repo);
  if (cached !== undefined) return cached;
  let isPrivate = false;
  try {
    const resp = await fetch(`${GH_API}/repos/${repo}`, { headers: ghHeaders(token) });
    if (resp.status === 200) isPrivate = (await resp.json()).private === true;
  } catch {
    isPrivate = false;
  }
  repoPrivateCache.set(repo, isPrivate);
  return isPrivate;
}

// Ensure a `crash-dumps` prerelease exists; return its upload_url template, or "".
async function ensureCrashRelease(token, repo) {
  const h = ghHeaders(token);
  const base = `${GH_API}/repos/${repo}/releases`;
  const existing = await fetch(`${base}/tags/crash-dumps`, { headers: h });
  if (existing.status === 200) {
    try {
      return (await existing.json()).upload_url || "";
    } catch {
      return "";
    }
  }
  if (existing.status !== 404) return "";
  const made = await fetch(base, {
    method: "POST",
    headers: h,
    body: JSON.stringify({
      tag_name: "crash-dumps",
      name: "Crash dumps",
      body: "Minidumps attached automatically by the Smatchet crash reporter.",
      prerelease: true,
    }),
  });
  if (made.status !== 201) return "";
  try {
    return (await made.json()).upload_url || "";
  } catch {
    return "";
  }
}

// Upload a base64 minidump as a Release asset; returns the browser download URL or "".
async function uploadCrashDump(token, repo, base64, name) {
  let uploadUrl = await ensureCrashRelease(token, repo);
  if (!uploadUrl) return "";
  const brace = uploadUrl.indexOf("{");
  if (brace !== -1) uploadUrl = uploadUrl.slice(0, brace);
  uploadUrl += `?name=${encodeURIComponent(name || "crash.dmp")}`;
  // base64 -> bytes (malformed input must drop the dump, NOT 500 the whole report)
  let bytes;
  try {
    const bin = atob(base64);
    bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  } catch {
    return "";
  }
  const resp = await fetch(uploadUrl, {
    method: "POST",
    headers: {
      Authorization: `Bearer ${token}`,
      Accept: "application/vnd.github+json",
      "X-GitHub-Api-Version": "2022-11-28",
      "User-Agent": UA,
      "Content-Type": "application/octet-stream",
    },
    body: bytes,
  });
  if (resp.status !== 201 && resp.status !== 200) return "";
  try {
    return (await resp.json()).browser_download_url || "";
  } catch {
    return "";
  }
}

function splitRepo(slug) {
  const i = slug.indexOf("/");
  if (i <= 0 || i + 1 >= slug.length) return null;
  return { owner: slug.slice(0, i), repo: slug.slice(i + 1) };
}

// Best-effort: ensure the dedicated assets branch exists. Returns true on
// exists/created, false on any failure (caller skips the screenshot, non-fatal).
async function ensureAssetsBranch(token, repo) {
  const base = `${GH_API}/repos/${repo}`;
  const h = ghHeaders(token);
  const existing = await fetch(`${base}/git/ref/heads/${ASSETS_BRANCH}`, { headers: h });
  if (existing.status === 200) return true;
  if (existing.status !== 404) return false;

  const meta = await fetch(base, { headers: h });
  if (meta.status !== 200) return false;
  const def = (await meta.json()).default_branch;
  if (!def) return false;

  const baseRef = await fetch(`${base}/git/ref/heads/${def}`, { headers: h });
  if (baseRef.status !== 200) return false;
  const sha = (await baseRef.json())?.object?.sha;
  if (!sha) return false;

  const created = await fetch(`${base}/git/refs`, {
    method: "POST",
    headers: h,
    body: JSON.stringify({ ref: `refs/heads/${ASSETS_BRANCH}`, sha }),
  });
  if (created.status === 201) return true;
  // Concurrent creator may have made the ref between our 404 probe and this POST
  // (GitHub reports 422, sometimes 409). Re-check rather than failing the upload.
  if (created.status === 422 || created.status === 409) {
    const recheck = await fetch(`${base}/git/ref/heads/${ASSETS_BRANCH}`, { headers: h });
    return recheck.status === 200;
  }
  return false;
}

// Upload a base64 PNG via the Contents API; returns the rendered download URL or "".
async function uploadScreenshot(token, assetsRepo, base64, stamp) {
  if (!(await ensureAssetsBranch(token, assetsRepo))) return "";
  const path = `bug-assets/${stamp}.png`;
  const url = `${GH_API}/repos/${assetsRepo}/contents/${path}`;
  const resp = await fetch(url, {
    method: "PUT",
    headers: ghHeaders(token),
    body: JSON.stringify({
      message: `bug-report screenshot ${stamp}`,
      content: base64,
      branch: ASSETS_BRANCH,
    }),
  });
  if (resp.status !== 201 && resp.status !== 200) return "";
  try {
    return (await resp.json())?.content?.download_url || "";
  } catch {
    return "";
  }
}

async function handleReport(request, env) {
  if (request.method !== "POST") return json(405, { ok: false, error: "POST only" });

  const limited = await rateLimitReport(request, env);
  if (limited) {
    return new Response(JSON.stringify({ ok: false, error: `rate limited (${limited})` }), {
      status: 429,
      headers: { "Content-Type": "application/json", "Retry-After": RATE_LIMIT_RETRY_AFTER },
    });
  }

  if (env.RELAY_KEY && request.headers.get("x-relay-key") !== env.RELAY_KEY) {
    return json(401, { ok: false, error: "bad or missing relay key" });
  }

  // Measure real bytes, not UTF-16 code units (multibyte input could otherwise
  // bypass the cap).
  const rawBuf = await request.arrayBuffer();
  if (rawBuf.byteLength > MAX_BODY_BYTES) return json(413, { ok: false, error: "payload too large" });
  const raw = new TextDecoder().decode(rawBuf);

  let payload;
  try {
    payload = JSON.parse(raw);
  } catch {
    return json(400, { ok: false, error: "invalid JSON" });
  }

  // Type-safe: a non-string title (number/object) must not throw a 500.
  const title = typeof payload.title === "string" ? payload.title.trim() : "";
  let body = typeof payload.body === "string" ? payload.body : "";
  if (!title) return json(400, { ok: false, error: "title required" });

  const repo = env.REPO;
  if (!repo || !splitRepo(repo)) {
    return json(500, { ok: false, error: "relay REPO var not configured as owner/repo" });
  }
  const assetsRepo = env.ASSETS_REPO && splitRepo(env.ASSETS_REPO) ? env.ASSETS_REPO : repo;

  // Screenshot first, so the inline image is already in the body when the issue is created.
  if (payload.screenshotBase64) {
    const stamp = Date.now().toString();
    const rawUrl = await uploadScreenshot(env.GITHUB_TOKEN, assetsRepo, payload.screenshotBase64, stamp);
    if (rawUrl) {
      body += `\n\n![screenshot](${rawUrl})`;
    } else {
      body += `\n\n_Screenshot received but could not be uploaded (relay token lacks contents:write or upload failed)._`;
    }
  }

  // Crash minidump → Release asset (binaries off the git tree), link in the body.
  // Only ever into a PRIVATE repo: see assertDumpRepoPrivate.
  if (payload.dumpBase64) {
    const dumpsRepo = env.DUMPS_REPO && splitRepo(env.DUMPS_REPO) ? env.DUMPS_REPO : assetsRepo;
    if (!(await assertDumpRepoPrivate(env.GITHUB_TOKEN, dumpsRepo))) {
      body +=
        `\n\n_Crash minidump received but **discarded**: the configured dump repo (\`${dumpsRepo}\`) is ` +
        `not private, and a minidump carries process stack memory. Set \`DUMPS_REPO\` to a private repo and redeploy._`;
    } else {
      const dumpUrl = await uploadCrashDump(env.GITHUB_TOKEN, dumpsRepo, payload.dumpBase64, payload.dumpName);
      if (dumpUrl) {
        body += `\n\n[Crash minidump](${dumpUrl})`;
      } else {
        body += `\n\n_Crash minidump received but could not be uploaded (relay token lacks release perms or upload failed)._`;
      }
    }
  }

  const create = await fetch(`${GH_API}/repos/${repo}/issues`, {
    method: "POST",
    headers: ghHeaders(env.GITHUB_TOKEN),
    body: JSON.stringify({ title, body }),
  });
  if (create.status !== 201 && create.status !== 200) {
    let msg = `HTTP ${create.status}`;
    try {
      msg = (await create.json())?.message || msg;
    } catch {}
    return json(502, { ok: false, error: `GitHub create failed: ${msg}` });
  }
  const issue = await create.json();
  const num = issue.number;
  return json(200, {
    ok: true,
    issueKey: `${repo}#${num}`,
    url: issue.html_url || "",
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/report") return handleReport(request, env);
    if (url.pathname === "/health") return json(200, { ok: true });
    return json(404, { ok: false, error: "not found" });
  },
};
