#!/usr/bin/env python3
# scripts/dev/verifier-endpoint.py — a dependency-free, OpenAI-compatible
# /chat/completions endpoint that serves DETERMINISTIC canned responses, so the
# full verifier loop (produce -> aggregate -> calibrate) can be smoke-tested over
# real HTTP with no model, no API key, and no GPU. Pure Python 3 stdlib
# (http.server + hashlib), offline-testable via chat_completion() + --selftest.
# Docs: docs/agent-rules/verifier-sidecar.md § Calibration.
#
# It is NOT a model. Each answer is a hash of the request (criterion + candidate),
# shaped into a plausible single-token A-T distribution peaked on one letter — so
# scores vary reproducibly across candidates/criteria (enough spread to exercise
# the sidecar's variance and the calibration report), but they carry no real
# judgement. Use it to prove the wiring, then point VERIFIER_BASE_URL at a real
# logprob-capable backend (vLLM / SGLang) for the fine-grained path.
#
# It answers BOTH producer modes:
#   * logprobs mode — choices[0].logprobs.content[0].top_logprobs is a full A-T
#     distribution the producer reads and the sidecar takes an expectation over;
#   * scalar mode  — choices[0].message.content is the single chosen letter.
#
# Usage:
#   python scripts/dev/verifier-endpoint.py                 # bind 127.0.0.1:<ephemeral>
#   python scripts/dev/verifier-endpoint.py --port 8900
#   python scripts/dev/verifier-endpoint.py --fixed A       # every answer is 'A' (demo)
#   python scripts/dev/verifier-endpoint.py --selftest
#
# On startup it prints one line so a caller can capture the bound URL:
#   endpoint: http://127.0.0.1:<port>
#
# Drive the loop against it:
#   python scripts/dev/verifier-endpoint.py --port 8900 &
#   VERIFIER_BASE_URL=http://127.0.0.1:8900 \
#     python scripts/dev/verifier-produce.py job.json --model stub \
#     | python scripts/dev/verifier-sidecar.py aggregate -
#
# selftest: asserts-failure — --selftest exercises malformed-request rejection.
#
# Exit codes:
#   0 — --selftest passed, or the server shut down cleanly (Ctrl-C).
#   2 — usage error (e.g. a bad --fixed letter or an in-use port).

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple

GRANULARITY = 20
LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[:GRANULARITY]  # A(best) .. T(worst)
_CREATED = 1700000000  # constant so a given request is byte-for-byte reproducible.


class EndpointError(ValueError):
    """Malformed /chat/completions request."""


# --- deterministic scoring -------------------------------------------------

def _seed(text: str) -> int:
    """Stable cross-process seed (Python's built-in hash() is salted, so unusable
    for reproducibility)."""
    return int.from_bytes(hashlib.sha256(text.encode("utf-8")).digest()[:8], "big")


def _messages_key(body: Dict[str, Any]) -> str:
    """A stable key over the prompt: the criterion + candidate live in the message
    text, so hashing it makes each (criterion, candidate) score differently while
    a repeated identical request scores identically."""
    parts = []
    for m in body.get("messages", []):
        if isinstance(m, dict):
            parts.append(f"{m.get('role', '')}:{m.get('content', '')}")
    return "\n".join(parts)


def distribution(body: Dict[str, Any], fixed: Optional[str]) -> Tuple[int, List[float]]:
    """Return (chosen_letter_index, per-letter logprobs summing to 1 in prob space).
    Peaked on a hash-chosen centre; temperature widens the peak and (via the seed)
    moves it, so the producer's hotter resamples actually vary."""
    if fixed is not None:
        center, sigma = LETTERS.index(fixed), 0.6
    else:
        try:
            temperature = float(body.get("temperature", 0.0))
        except (TypeError, ValueError):
            temperature = 0.0
        seed = _seed(f"{_messages_key(body)}|T={temperature:.3f}")
        center = seed % GRANULARITY
        # width in [1, 4], widened by temperature so hot samples spread.
        sigma = (1.0 + (seed >> 8) % 3000 / 1000.0) * (1.0 + max(0.0, temperature))
    logits = [-((i - center) ** 2) / (2.0 * sigma * sigma) for i in range(GRANULARITY)]
    m = max(logits)
    logsumexp = m + math.log(sum(math.exp(x - m) for x in logits))
    logprobs = [x - logsumexp for x in logits]
    # The centre is the mode, so argmax == center; return it as the chosen token.
    return center, logprobs


def chat_completion(body: Dict[str, Any], fixed: Optional[str] = None) -> Dict[str, Any]:
    """Build one OpenAI-shaped chat.completion for the request `body`."""
    if not isinstance(body, dict):
        raise EndpointError("request body must be a JSON object")
    if not isinstance(body.get("messages"), list) or not body["messages"]:
        raise EndpointError("request needs a non-empty 'messages' array")

    center, logprobs = distribution(body, fixed)
    letter = LETTERS[center]
    message = {"role": "assistant", "content": letter}

    choice: Dict[str, Any] = {
        "index": 0,
        "message": message,
        "finish_reason": "stop",
    }
    # Only attach logprobs when the caller asked (mirrors real backends; the
    # producer sets logprobs=True in logprobs mode and omits it in scalar mode).
    if body.get("logprobs"):
        want = body.get("top_logprobs", GRANULARITY)
        try:
            want = max(1, min(GRANULARITY, int(want)))
        except (TypeError, ValueError):
            want = GRANULARITY
        ranked = sorted(range(GRANULARITY), key=lambda i: logprobs[i], reverse=True)[:want]
        top = [{"token": LETTERS[i], "logprob": round(logprobs[i], 6)} for i in ranked]
        choice["logprobs"] = {"content": [{
            "token": letter,
            "logprob": round(logprobs[center], 6),
            "top_logprobs": top,
        }]}

    return {
        "id": "chatcmpl-stub-" + format(_seed(_messages_key(body)) & 0xFFFFFFFF, "08x"),
        "object": "chat.completion",
        "created": _CREATED,
        "model": str(body.get("model", "verifier-stub")),
        "choices": [choice],
        "usage": {"prompt_tokens": 0, "completion_tokens": 1, "total_tokens": 1},
    }


def parse_request(raw: bytes) -> Dict[str, Any]:
    try:
        body = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EndpointError(f"invalid JSON body: {exc}") from exc
    if not isinstance(body, dict):
        raise EndpointError("request body must be a JSON object")
    return body


# --- HTTP server -----------------------------------------------------------

def _make_handler(fixed: Optional[str]) -> type:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def _send(self, code: int, payload: Dict[str, Any]) -> None:
            data = json.dumps(payload).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:  # noqa: N802 (stdlib naming)
            if self.path.rstrip("/") in ("/health", "/healthz"):
                self._send(200, {"status": "ok"})
            else:
                self._send(404, {"error": {"message": f"not found: {self.path}"}})

        def do_POST(self) -> None:  # noqa: N802
            if self.path.rstrip("/") not in ("/chat/completions", "/v1/chat/completions"):
                self._send(404, {"error": {"message": f"not found: {self.path}"}})
                return
            try:
                length = int(self.headers.get("Content-Length", 0))
            except (TypeError, ValueError):
                length = 0
            raw = self.rfile.read(length) if length > 0 else b""
            try:
                body = parse_request(raw)
                self._send(200, chat_completion(body, fixed))
            except EndpointError as exc:
                self._send(400, {"error": {"message": str(exc), "type": "invalid_request_error"}})

        def log_message(self, *_args: Any) -> None:  # silence per-request logging.
            pass

    return Handler


def serve(host: str, port: int, fixed: Optional[str]) -> None:
    httpd = ThreadingHTTPServer((host, port), _make_handler(fixed))
    bound_host, bound_port = httpd.server_address[0], httpd.server_address[1]
    # One machine-readable line so a caller can capture the ephemeral URL.
    print(f"endpoint: http://{bound_host}:{bound_port}", flush=True)
    if fixed is not None:
        print(f"fixed-letter mode: every answer is {fixed!r}", file=sys.stderr, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


# --- self-test -------------------------------------------------------------

def selftest() -> None:
    sys_msg = {"role": "system", "content": "Criterion: security — no injection?"}
    body = {"model": "stub", "logprobs": True, "top_logprobs": GRANULARITY, "temperature": 0.0,
            "messages": [sys_msg, {"role": "user", "content": "CANDIDATE: parameterized query"}]}

    resp = chat_completion(body)
    # Well-formed, single A-T letter content.
    content = resp["choices"][0]["message"]["content"]
    assert content in LETTERS, content
    lp = resp["choices"][0]["logprobs"]["content"]
    assert len(lp) == 1 and lp[0]["token"] == content, lp
    top = lp[0]["top_logprobs"]
    assert len(top) == GRANULARITY, len(top)
    assert all(t["token"] in LETTERS for t in top), top
    # The chosen token is the mode (max logprob).
    assert lp[0]["logprob"] == max(t["logprob"] for t in top), lp
    # Probabilities normalise to 1.
    total = sum(math.exp(t["logprob"]) for t in top)
    assert abs(total - 1.0) < 1e-6, total

    # top_logprobs count is honoured (and clamped).
    few = chat_completion({**body, "top_logprobs": 3})
    assert len(few["choices"][0]["logprobs"]["content"][0]["top_logprobs"]) == 3

    # Determinism: identical request → byte-identical response.
    assert chat_completion(body) == resp, "response is not deterministic"

    # Different criteria score differently (variety for the sidecar/calibrate).
    other = {**body, "messages": [{"role": "system", "content": "Criterion: correctness — sound?"},
                                  {"role": "user", "content": "CANDIDATE: parameterized query"}]}
    centers = {chat_completion(b)["choices"][0]["message"]["content"]
               for b in (body, other,
                         {**body, "messages": [sys_msg, {"role": "user", "content": "CANDIDATE: raw sql"}]})}
    assert len(centers) >= 2, f"stub gives no spread across requests: {centers}"

    # Scalar mode: no logprobs requested → content only, no logprobs object.
    scalar = chat_completion({"model": "stub", "messages": body["messages"]})
    assert scalar["choices"][0]["message"]["content"] in LETTERS
    assert "logprobs" not in scalar["choices"][0], scalar["choices"][0]

    # Fixed mode pins the letter.
    fixed = chat_completion(body, fixed="A")
    assert fixed["choices"][0]["message"]["content"] == "A", fixed
    assert fixed["choices"][0]["logprobs"]["content"][0]["token"] == "A"

    # Malformed requests FAIL (selftest: asserts-failure).
    for raw in (b"{not json", b"[]", b'{"messages": []}', b'{"messages": "x"}', b'{}'):
        try:
            chat_completion(parse_request(raw))
        except EndpointError:
            pass
        else:
            raise AssertionError(f"expected EndpointError for {raw!r}")


# --- CLI -------------------------------------------------------------------

def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Deterministic OpenAI-compatible verifier stub endpoint.")
    parser.add_argument("--host", default="127.0.0.1", help="Bind host (default 127.0.0.1).")
    parser.add_argument("--port", type=int, default=0, help="Bind port (default 0 = ephemeral).")
    parser.add_argument("--fixed", default=None, help="Force every answer to this A-T letter (demos).")
    parser.add_argument("--selftest", action="store_true", help="Run the deterministic self-test.")
    args = parser.parse_args(argv)

    try:
        if args.selftest:
            selftest()
            print("verifier-endpoint selftest: PASS")
            return 0
        fixed = None
        if args.fixed is not None:
            fixed = args.fixed.strip().upper()
            if fixed not in LETTERS:
                raise EndpointError(f"--fixed must be one of A-T, got {args.fixed!r}")
        serve(args.host, args.port, fixed)
        return 0
    except EndpointError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"ERROR: cannot bind {args.host}:{args.port}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
