#include "retry_after.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace kumo::agent::detail {

std::optional<double> parseRetryAfterSeconds(std::string_view headerValue) {
    const auto begin = headerValue.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return std::nullopt;
    }
    const auto end = headerValue.find_last_not_of(" \t");
    const std::string trimmed(headerValue.substr(begin, end - begin + 1));

    // strtod instead of std::from_chars: the floating-point overload is still
    // missing from the CI toolchain's libc++. A full match only: the HTTP-date
    // form ("Wed, 21 Oct 2015 07:28:00 GMT") stops at the comma and is
    // rejected; strtod sets the result to +-HUGE_VAL on overflow, which the
    // finiteness check below rejects.
    char* parseEnd = nullptr;
    const double seconds = std::strtod(trimmed.c_str(), &parseEnd);
    if (parseEnd != trimmed.c_str() + trimmed.size() || trimmed.empty()) {
        return std::nullopt;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return std::nullopt;
    }
    return seconds;
}

} // namespace kumo::agent::detail
