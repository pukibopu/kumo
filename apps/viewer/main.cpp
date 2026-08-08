#include <kumo/core/file.h>
#include <kumo/core/log.h>
#include <kumo/shaderc/compiler.h>

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace {

// One-shot compatibility proof for every agent-generated shader (MD): a
// template or ABI change that breaks an installed material must fail CI-style
// here, not at the next app launch.
int runCheckShaders() {
    const std::filesystem::path dir = std::filesystem::path(KUMO_SHADER_DIR) / "generated";
    const std::vector<std::string> includeDirs{
        (std::filesystem::path(KUMO_SHADER_DIR) / "include").string()};
    int checked = 0;
    int failures = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".frag") {
            continue;
        }
        ++checked;
        const auto text = kumo::readTextFile(entry.path());
        if (!text.has_value()) {
            kumo::logError("check-shaders: cannot read {}", entry.path().string());
            ++failures;
            continue;
        }
        const auto compiled = kumo::shaderc::compileGlsl(
            *text, kumo::shaderc::Stage::Fragment,
            {.sourceName = entry.path().filename().string(), .includeDirs = includeDirs});
        if (!compiled.has_value()) {
            ++failures;
            for (const kumo::shaderc::CompileError& error : compiled.error()) {
                kumo::logError("check-shaders: {}:{}: {}", entry.path().filename().string(),
                               error.line, error.message);
            }
        }
    }
    kumo::logInfo("check-shaders: {} generated shaders checked, {} failed", checked, failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

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
    bool checkShaders = false;
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
        } else if (arg == "--check-shaders") {
            checkShaders = true;
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
                "[--thumbnails [--force]] [--check-shaders])",
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

    if (checkShaders) {
        return runCheckShaders();
    }

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
