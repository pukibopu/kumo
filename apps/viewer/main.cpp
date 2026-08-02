#include <kumo/core/log.h>

#include <cstdlib>
#include <filesystem>
#include <string_view>

#if defined(__APPLE__)
#define KUMO_HAS_RUNAPP 1
int runApp(int maxFrames, const std::filesystem::path& modelPath,
           const std::filesystem::path& envPath, const std::filesystem::path& assetDir,
           bool demoPrimitives, bool offline, bool confirmDestructive, bool mcp);
// Headless batch mode (MA milestone): renders a thumbnail PNG per model/
// texture-set/env under `assetDir` and (re)writes assetDir/index.json; no
// GLFW window. `force` re-renders every thumbnail even when its PNG is newer
// than the source asset. Returns the process exit code.
int runThumbnails(const std::filesystem::path& assetDir, bool force);
#endif

int main(int argc, char** argv) {
    int maxFrames = -1;
    bool demoPrimitives = false;
    bool offline = false;
    bool confirmDestructive = false;
    bool mcp = false;
    bool thumbnails = false;
    bool force = false;
    std::filesystem::path modelPath =
        std::filesystem::path(KUMO_ASSET_DIR) / "models" / "DamagedHelmet.glb";
    std::filesystem::path envPath =
        std::filesystem::path(KUMO_ASSET_DIR) / "env" / "studio_small_09_2k.hdr";

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--frames" && i + 1 < argc) {
            maxFrames = std::atoi(argv[++i]);
        } else if (arg == "--demo-primitives") {
            demoPrimitives = true;
        } else if (arg == "--offline") {
            offline = true;
        } else if (arg == "--confirm-destructive") {
            confirmDestructive = true;
        } else if (arg == "--mcp") {
            mcp = true;
        } else if (arg == "--thumbnails") {
            thumbnails = true;
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--env") {
            if (i + 1 >= argc) {
                kumo::logError("--env requires a path to an .hdr file");
                return 1;
            }
            envPath = argv[++i];
        } else if (!arg.starts_with("--")) {
            modelPath = arg;
        } else {
            kumo::logError(
                "unknown option '{}' (usage: viewer [model.glb] [--env path.hdr] "
                "[--frames N] [--demo-primitives] [--offline] [--confirm-destructive] [--mcp] "
                "[--thumbnails [--force]])",
                arg);
            return 1;
        }
    }

    // MCP mode reserves stdout for the JSON-RPC wire, so every log line must
    // move to stderr before the very first one is printed.
    if (mcp) {
        kumo::setLogAllToStderr(true);
    }
    kumo::logInfo("kumo viewer {}", KUMO_VERSION_STRING);

#if defined(KUMO_HAS_RUNAPP)
    if (thumbnails) {
        return runThumbnails(KUMO_ASSET_DIR, force);
    }
    return runApp(maxFrames, modelPath, envPath, KUMO_ASSET_DIR, demoPrimitives, offline,
                  confirmDestructive, mcp);
#else
    (void)maxFrames;
    (void)demoPrimitives;
    (void)offline;
    (void)confirmDestructive;
    (void)mcp;
    (void)thumbnails;
    (void)force;
    kumo::logInfo("no rendering backend for this platform yet");
    return 0;
#endif
}
