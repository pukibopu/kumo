#pragma once

namespace kumo::detail {
[[noreturn]] void assertFail(const char* condition, const char* file, int line);
} // namespace kumo::detail

#ifndef NDEBUG
#define KUMO_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::kumo::detail::assertFail(#cond, __FILE__, __LINE__);                                 \
        }                                                                                          \
    } while (false)
#else
// Keeps cond referenced (unevaluated) so release builds do not trip -Wunused.
#define KUMO_ASSERT(cond) ((void)(false && (cond)))
#endif
