#pragma once

#include <optional>
#include <string_view>

namespace kumo::agent::detail {

// Parses the delta-seconds form of a Retry-After header value ("2.5", "7").
// nullopt for anything else, including the HTTP-date form and garbage, so the
// caller falls back to its own backoff schedule.
std::optional<double> parseRetryAfterSeconds(std::string_view headerValue);

} // namespace kumo::agent::detail
