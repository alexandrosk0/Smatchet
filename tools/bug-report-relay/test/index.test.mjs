// Smoke tests for the two abuse/privacy gates on POST /report: the rate limiters and
// the "minidumps only into a private repo" rule. Run with `npm test` (node:test, no deps).
//
// The Worker's only outbound dependency is global `fetch`, so each test installs a stub
// that records calls and replies from a per-test route table. Nothing here touches the
// network or GitHub.
//
// NOTE: `knownPrivateRepos` in src/index.js is module-level and survives across tests in
// one import, so every test uses a DISTINCT repo slug — otherwise a later case would read
// an earlier one's cached visibility.

import assert from "node:assert/strict";
import test from "node:test";

import worker from "../src/index.js";

const ISSUE = { number: 7, html_url: "https://github.com/o/r/issues/7" };

// Route table → fetch stub. Keys are matched as substrings of the request URL, longest
// first, so a specific path wins over a generic prefix. Unmatched URLs fail the test
// rather than silently 404-ing, which would hide a wrong endpoint.
function stubFetch(routes) {
  const calls = [];
  const keys = Object.keys(routes).sort((a, b) => b.length - a.length);
  globalThis.fetch = async (url, init = {}) => {
    const href = String(url);
    calls.push({ url: href, method: init.method || "GET" });
    const key = keys.find((k) => href.includes(k));
    assert.ok(key, `unstubbed fetch: ${init.method || "GET"} ${href}`);
    const { status = 200, body = {} } = routes[key];
    return new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
  };
  return calls;
}

function reportRequest(body, headers = {}) {
  return new Request("https://relay.test/report", {
    method: "POST",
    headers: { "Content-Type": "application/json", "CF-Connecting-IP": "203.0.113.9", ...headers },
    body: JSON.stringify(body),
  });
}

// A ratelimit binding that always allows / always rejects / always throws.
const allowLimiter = { limit: async () => ({ success: true }) };
const denyLimiter = { limit: async () => ({ success: false }) };
const brokenLimiter = {
  limit: async () => {
    throw new Error("limiter unavailable");
  },
};

test("over the per-IP budget returns 429 with Retry-After and never calls GitHub", async () => {
  const calls = stubFetch({});
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b" }), {
    REPO: "o/r",
    REPORT_RATE_LIMITER: denyLimiter,
  });
  assert.equal(resp.status, 429);
  assert.equal(resp.headers.get("Retry-After"), "60");
  assert.match((await resp.json()).error, /per-IP/);
  assert.deepEqual(calls, [], "a rate-limited request must not reach GitHub");
});

test("the global ceiling rejects even when the per-IP budget has room", async () => {
  stubFetch({});
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b" }), {
    REPO: "o/r",
    REPORT_RATE_LIMITER: allowLimiter,
    GLOBAL_RATE_LIMITER: denyLimiter,
  });
  assert.equal(resp.status, 429);
  assert.match((await resp.json()).error, /global/);
});

test("rate limiting runs BEFORE the relay-key check, so key guessing is throttled", async () => {
  stubFetch({});
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b" }, { "x-relay-key": "wrong" }), {
    REPO: "o/r",
    RELAY_KEY: "right",
    REPORT_RATE_LIMITER: denyLimiter,
  });
  assert.equal(resp.status, 429, "a bad key over budget must be rate-limited, not merely 401'd");
});

test("a broken limiter fails OPEN — reporting must not go down with it", async () => {
  stubFetch({ "/repos/o/r/issues": { status: 201, body: ISSUE } });
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b" }), {
    REPO: "o/r",
    REPORT_RATE_LIMITER: brokenLimiter,
  });
  assert.equal(resp.status, 200);
});

test("missing limiter bindings (older deploy / wrangler dev) let the request through", async () => {
  stubFetch({ "/repos/o/r/issues": { status: 201, body: ISSUE } });
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b" }), { REPO: "o/r" });
  assert.equal(resp.status, 200);
});

test("a minidump bound for a PUBLIC repo is discarded; the issue still files", async () => {
  let posted;
  const calls = stubFetch({
    "/repos/pub/dumps": { status: 200, body: { private: false } },
    "/repos/pub/dumps/issues": { status: 201, body: ISSUE },
  });
  globalThis.fetch = new Proxy(globalThis.fetch, {
    apply(target, thisArg, args) {
      if (String(args[0]).endsWith("/issues")) posted = JSON.parse(args[1].body);
      return Reflect.apply(target, thisArg, args);
    },
  });

  const resp = await worker.fetch(reportRequest({ title: "t", body: "b", dumpBase64: "AAAA", dumpName: "c.dmp" }), {
    REPO: "pub/dumps",
    GITHUB_TOKEN: "x",
  });

  assert.equal(resp.status, 200, "dropping the dump must not fail the report");
  assert.match(posted.body, /discarded/, "the issue must say the dump was dropped");
  assert.match(posted.body, /not private/);
  assert.ok(
    !calls.some((c) => c.url.includes("/releases")),
    "no release may be created or uploaded to for a public repo",
  );
});

test("unreadable visibility fails CLOSED — the dump is dropped", async () => {
  const calls = stubFetch({
    "/repos/err/dumps": { status: 500, body: {} },
    "/repos/err/dumps/issues": { status: 201, body: ISSUE },
  });
  const resp = await worker.fetch(reportRequest({ title: "t", body: "b", dumpBase64: "AAAA" }), {
    REPO: "err/dumps",
    GITHUB_TOKEN: "x",
  });
  assert.equal(resp.status, 200);
  assert.ok(!calls.some((c) => c.url.includes("/releases")));
});

test("a PRIVATE repo takes the dump, and DUMPS_REPO wins over ASSETS_REPO", async () => {
  const calls = stubFetch({
    "/repos/priv/dumps": { status: 200, body: { private: true } },
    "/repos/priv/dumps/releases/tags/crash-dumps": {
      status: 200,
      body: { upload_url: "https://uploads.test/assets{?name,label}" },
    },
    "https://uploads.test/assets": { status: 201, body: { browser_download_url: "https://dl.test/c.dmp" } },
    "/repos/o/r/issues": { status: 201, body: ISSUE },
  });

  const resp = await worker.fetch(reportRequest({ title: "t", body: "b", dumpBase64: "AAAA", dumpName: "c.dmp" }), {
    REPO: "o/r",
    ASSETS_REPO: "pub/assets", // must NOT be used for the dump
    DUMPS_REPO: "priv/dumps",
    GITHUB_TOKEN: "x",
  });

  assert.equal(resp.status, 200);
  assert.ok(calls.some((c) => c.url === "https://uploads.test/assets?name=c.dmp"), "the dump must be uploaded");
  assert.ok(!calls.some((c) => c.url.includes("pub/assets")), "ASSETS_REPO must not receive the dump");
});

test("a 'not private' answer is NOT cached — the operator's fix takes effect immediately", async () => {
  // First report sees a public repo; the operator then flips it private. Without
  // re-checking, the isolate would keep dropping dumps until it recycled.
  let isPrivate = false;
  const uploaded = [];
  globalThis.fetch = async (url, init = {}) => {
    const href = String(url);
    if (href.endsWith("/repos/flip/dumps")) return Response.json({ private: isPrivate });
    if (href.includes("/releases/tags/crash-dumps")) {
      return Response.json({ upload_url: "https://uploads.test/assets{?name,label}" });
    }
    if (href.startsWith("https://uploads.test/assets")) {
      uploaded.push(href);
      return Response.json({ browser_download_url: "https://dl.test/c.dmp" }, { status: 201 });
    }
    if (href.endsWith("/issues")) return Response.json(ISSUE, { status: 201 });
    throw new Error(`unstubbed fetch: ${init.method || "GET"} ${href}`);
  };

  const env = { REPO: "flip/dumps", GITHUB_TOKEN: "x" };
  const send = () => worker.fetch(reportRequest({ title: "t", body: "b", dumpBase64: "AAAA" }), env);

  await send();
  assert.deepEqual(uploaded, [], "public repo: dump dropped");

  isPrivate = true;
  await send();
  assert.equal(uploaded.length, 1, "once private, the very next report uploads the dump");
});
