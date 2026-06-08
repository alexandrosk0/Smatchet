// OllamaStreamError.h — pure formatting of the AiStreamError::Message strings
// OllamaClient::SendStreaming surfaces on a transport / non-2xx HTTP failure.
// cpr-free sibling TU so the doctest rig links it without the HTTP layer.
// Pure assembly only: the HTTP-error body MUST already be sanitised via
// RedactProviderErrorBody by the caller (which redacts once, for both the log
// line and this message) — this helper never touches secrets.

#ifndef SMATCHET_OLLAMA_STREAM_ERROR_H
#define SMATCHET_OLLAMA_STREAM_ERROR_H

#include <string>

namespace smatchet {
namespace ai {
namespace pure {

// AiStreamError::Message for an Ollama transport failure (cpr error code != OK).
// Shape: "transport: <cprMessage>".
std::string FormatOllamaTransportError(const std::string& cprMessage);

// AiStreamError::Message for a non-2xx Ollama HTTP response. Shape:
// "HTTP <statusCode>" with ": <sanitisedBody>" appended only when the body is
// non-empty. sanitisedBody is assumed already passed through
// RedactProviderErrorBody by the caller; this helper performs no redaction.
std::string FormatOllamaHttpError(long statusCode, const std::string& sanitisedBody);

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_OLLAMA_STREAM_ERROR_H
