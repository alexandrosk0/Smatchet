- 2026-10-03 · deviation renewal (markers expiring 2026-12-31) · [debt] · P3 — the AI provider clients each keep their own copy of the URL helpers and the chat messages build

Details:
`AnthropicClient.cpp`, `OllamaClient.cpp` and `OpenAiClient.cpp` each define the same anonymous-namespace
`JoinUrl` and a `ResolveBaseUrl` that differs only in its default base URL; OpenAI's also trims a
trailing `/v1`. Ollama and OpenAI also build the chat `messages` array the same way: an optional leading
`{role: system}` entry, then one entry per `History` item. `dup_audit.py` reports these as clones,
exempted by two markers in `OllamaClient.cpp` (`revisit=2027-05-31`). The same three files carry further
grandfathered, unmarked clones in their reachability probes and streaming loops.

The markers used to say that sharing these would couple independent provider adapters. It would not:
the three clients already share `AiErrorRedact.h` (`smatchet::ai::pure`) and `AiWireIntrospect.h`, and
the helpers are pure string and JSON work with no provider policy in them.

The per-provider wire-introspection wrappers (`<Provider>BuildChatBodyJson` / `<Provider>ResolveChatUrl`)
are not part of this. Each has to live in its own TU to reach that TU's anonymous-namespace builders, so
their markers are `revisit=never`.

Concrete next action:
- Add a pure header next to `AiErrorRedact.h` with `JoinUrl(base, path)`,
  `ResolveBaseUrlOr(cfg, defaultBase)` and `AppendChatMessages(json& messages, systemPrompt, history)`.
- Move the three clients onto it, keeping OpenAI's `/v1` trim at its call site.
- Cover the helpers with a bucket-A test, and keep the existing `ai.dump-request` wire tests green.
- Delete the two `OllamaClient.cpp` exemptions.

Status: open
Last-reviewed: 2026-10-03
