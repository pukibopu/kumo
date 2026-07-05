#include <kumo/core/log.h>

#include <cstdlib>
#include <string_view>

#if defined(__APPLE__)
int runApp(int maxFrames);
#endif

int main(int argc, char** argv) {
    int maxFrames = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--frames" && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        }
    }

    kumo::logInfo("kumo viewer {}", KUMO_VERSION_STRING);

#if defined(__APPLE__)
    return runApp(maxFrames);
#else
    (void)maxFrames;
    kumo::logInfo("no rendering backend for this platform yet");
    return 0;
#endif
}
