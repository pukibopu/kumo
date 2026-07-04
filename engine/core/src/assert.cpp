#include "kumo/core/assert.h"

#include "kumo/core/log.h"

#include <cstdlib>

namespace kumo::detail {

[[noreturn]] void assertFail(const char* condition, const char* file, int line) {
    logError("assertion failed: {} ({}:{})", condition, file, line);
    std::abort();
}

} // namespace kumo::detail
