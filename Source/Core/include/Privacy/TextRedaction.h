#ifndef SMATCHET_PRIVACY_TEXT_REDACTION_H
#define SMATCHET_PRIVACY_TEXT_REDACTION_H

#include <string>

// TextRedaction — keyless, best-effort scrubber for free-text log lines bound
// for an outbound bug report. Unlike BackendAuditTrail::RedactText (which needs
// a key to redact), this scrubs by *shape*: auth headers, secret URL params,
// userinfo in URLs, emails, and long opaque tokens. Pure (std::regex only, no
// live state) so the phase-2 crash path can reuse it against an on-disk log.
//
// This is a safety net, NOT a guarantee — the bug-report modal's egress preview
// + explicit consent gate is the real safeguard (see log-a-bug-github plan).

namespace smatchet {
namespace privacy {

/// Redact secret-shaped substrings from a single log line:
///  - CR / LF / ESC / ANSI CSI+OSC / C0 control bytes -> space (anti-injection)
///  - `Authorization: Bearer/Basic <tok>`        -> `Authorization: <redacted>`
///  - URL params `token|api_key|access_token|password|secret=<v>` -> `=<redacted>`
///  - URL userinfo `scheme://user:pass@host`      -> `scheme://user:<redacted>@host`
///  - emails                                      -> `<redacted-email>`
///  - UUID/GUID shape (8-4-4-4-12 hex)            -> `<redacted>`  (Plane tokens)
///  - long opaque tokens (>=40 base64url/hex)     -> `<redacted>`
/// The control/ANSI strip runs first so a server-controlled string cannot forge a
/// fake log line (CR/LF) or drive terminal escapes (ANSI), and cannot hide a secret
/// byte mid-token to evade the shape matchers. A 40-char lowercase-hex run (git
/// SHA-1) is deliberately preserved so commit hashes survive in the report.
/// Idempotent on already-redacted text.
std::string RedactLogLine(const std::string& line);

/// Apply RedactLogLine to every `\n`-separated line of `text` (line endings
/// preserved). Convenience for whole-blob redaction.
std::string RedactLogText(const std::string& text);

} // namespace privacy
} // namespace smatchet

#endif // SMATCHET_PRIVACY_TEXT_REDACTION_H
