#include <kumo/core/log.h>
#include <kumo/rhi/types.h>

#include <cstdlib>
#include <string_view>

#if defined(__APPLE__) || defined(KUMO_HAS_VULKAN)
#define KUMO_HAS_RUNAPP 1
int runApp(kumo::rhi::Backend backend, int maxFrames);
#endif

int main(int argc, char** argv) {
    int maxFrames = -1;
#if defined(__APPLE__)
    kumo::rhi::Backend backend = kumo::rhi::Backend::Metal;
#else
    kumo::rhi::Backend backend = kumo::rhi::Backend::Vulkan;
#endif

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--frames" && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        } else if (arg == "--rhi" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            if (value == "metal") {
                backend = kumo::rhi::Backend::Metal;
            } else if (value == "vulkan") {
                backend = kumo::rhi::Backend::Vulkan;
            } else {
                kumo::logError("unknown --rhi '{}' (expected metal|vulkan)", value);
                return 1;
            }
        }
    }

    kumo::logInfo("kumo viewer {}", KUMO_VERSION_STRING);

#if defined(KUMO_HAS_RUNAPP)
    return runApp(backend, maxFrames);
#else
    (void)maxFrames;
    (void)backend;
    kumo::logInfo("no rendering backend for this platform yet");
    return 0;
#endif
}
