#import "KumoEngine.h"

#include <kumo/facade/engine_runtime.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

using namespace kumo;

namespace {

constexpr rhi::TextureFormat kSwapchainFormat = rhi::TextureFormat::BGRA8Unorm;

void configureSurface(rhi::Surface& surface, std::uint32_t width, std::uint32_t height) {
    surface.configure({.size = {width, height}, .format = kSwapchainFormat});
}

// KUMO_APP_FRAMES: scripted-exit hook for smoke tests (0/unset disables it).
int scriptedExitFrameCount() {
    const char* env = std::getenv("KUMO_APP_FRAMES");
    if (env == nullptr) {
        return 0;
    }
    const int n = std::atoi(env);
    return n > 0 ? n : 0;
}

} // namespace

@interface KumoEngine () {
    // Declaration order is the destruction contract, mirroring EngineRuntime:
    // the runtime is torn down first (dealloc resets it explicitly below),
    // then the surface, then the device it was created from.
    rhi::Ptr<rhi::Device> _device;
    rhi::Ptr<rhi::Surface> _surface;
    std::unique_ptr<facade::EngineRuntime> _runtime;
    int _framesRendered;
    int _maxFrames;
}
@end

@implementation KumoEngine

- (nullable instancetype)initWithLayer:(CAMetalLayer*)layer
                              assetDir:(NSString*)assetDir
                             shaderDir:(NSString*)shaderDir
                            configPath:(NSString*)configPath
                           envFilePath:(NSString*)envFilePath {
    self = [super init];
    if (self == nil) {
        return nil;
    }

    _maxFrames = scriptedExitFrameCount();
    _framesRendered = 0;

    _device = rhi::metal::createDevice({});
    if (!_device) {
        return nil;
    }
    _surface = _device->createSurface({.nativeLayer = (__bridge void*)layer});
    if (!_surface) {
        return nil;
    }
    const double scale = layer.contentsScale > 0 ? layer.contentsScale : 1.0;
    const auto width = static_cast<std::uint32_t>(std::max(1.0, layer.bounds.size.width * scale));
    const auto height = static_cast<std::uint32_t>(std::max(1.0, layer.bounds.size.height * scale));
    configureSurface(*_surface, width, height);

    const std::string assetDirStd = assetDir.UTF8String;
    const facade::EngineRuntime::Desc desc{
        .modelPath = assetDirStd + "/models/DamagedHelmet.glb",
        .envPath = assetDirStd + "/env/studio_small_09_2k.hdr",
        .configPath = configPath.UTF8String,
        .envFilePath = envFilePath.UTF8String,
        .shaderDir = shaderDir.UTF8String,
        .offline = false,
        .confirmDestructive = false,
        .demoPrimitives = false,
        .mcp = false,
        .appVersion = KUMO_VERSION_STRING,
    };
    _runtime = facade::EngineRuntime::create(*_device, desc);
    if (!_runtime) {
        return nil;
    }
    _runtime->resize({.width = width, .height = height});

    return self;
}

- (nullable instancetype)initWithLayerUsingDefaults:(CAMetalLayer*)layer {
    return [self initWithLayer:layer
                      assetDir:@(KUMO_ASSET_DIR)
                     shaderDir:@(KUMO_SHADER_DIR)
                    configPath:@(KUMO_REPO_DIR "/kumo.config.json")
                   envFilePath:@(KUMO_REPO_DIR "/.env")];
}

- (BOOL)tick {
    if (!_runtime->pump()) {
        // The MCP client (if any) hung up; ask the shell to quit.
        return NO;
    }
    rhi::Ptr<rhi::CommandEncoder> encoder = _device->queue().createCommandEncoder();
    rhi::Texture* target = _surface->acquireNextTexture();
    if (target != nullptr) {
        _runtime->render(*encoder, target);
    }
    encoder->finishAndSubmit(_surface.get());

    if (_maxFrames > 0 && ++_framesRendered >= _maxFrames) {
        return NO;
    }
    return YES;
}

- (void)resizeWidth:(uint32_t)width height:(uint32_t)height {
    if (width == 0 || height == 0) {
        return;
    }
    configureSurface(*_surface, width, height);
    _runtime->resize({.width = width, .height = height});
}

- (void)dealloc {
    // Explicit, in destruction-contract order: the runtime must stop touching
    // the surface/device before they go away (mirrors app.cpp's
    // runtime.reset()-before-teardown sequencing).
    _runtime.reset();
    _surface.reset();
    _device.reset();
}

@end
