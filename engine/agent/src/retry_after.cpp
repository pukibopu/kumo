#include "retry_after.h"

#include <charconv>
#include <cmath>

namespace kumo::agent::detail {

std::optional<double> parseRetryAfterSeconds(std::string_view headerValue) {
    const auto begin = headerValue.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto end = headerValue.find_last_not_of(" \t");
    const std::string_view trimmed = headerValue.substr(begin, end - begin + 1);

    double seconds = 0.0;
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), seconds);
    // A full match only: the HTTP-date form ("Wed, 21 Oct 2015 07:28:00 GMT")
    // fails to parse as a leading number and falls back to backoff; an
    // out-of-range value (e.g. an absurdly long digit string) is rejected too.
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return std::nullopt;
    }
    return seconds;
}

} // namespace kumo::agent::detail
