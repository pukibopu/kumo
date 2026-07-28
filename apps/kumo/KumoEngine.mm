#import "KumoEngine.h"

#include <kumo/facade/engine_runtime.h>
#include <kumo/rhi/rhi.h>
#include <kumo/rhi_metal/rhi_metal.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
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

NSString* toNSString(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}

std::string toStdString(NSString* s) {
    return s.UTF8String != nullptr ? std::string(s.UTF8String) : std::string();
}

} // namespace

@interface KumoEntityInfo ()
@property(nonatomic, readwrite) NSString* entityId;
@property(nonatomic, readwrite) NSString* name;
@property(nonatomic, readwrite) NSString* primitive;
@end

@implementation KumoEntityInfo
@end

@interface KumoEntityDetail ()
@property(nonatomic, readwrite) BOOL found;
@property(nonatomic, readwrite) NSString* entityId;
@property(nonatomic, readwrite) NSString* name;
@property(nonatomic, readwrite) NSString* primitive;
@property(nonatomic, readwrite) float positionX, positionY, positionZ;
@property(nonatomic, readwrite) float eulerX, eulerY, eulerZ;
@property(nonatomic, readwrite) float scaleX, scaleY, scaleZ;
@property(nonatomic, readwrite) BOOL hasMaterial;
@property(nonatomic, readwrite) float baseColorR, baseColorG, baseColorB, baseColorA;
@property(nonatomic, readwrite) float metallic;
@property(nonatomic, readwrite) float roughness;
@property(nonatomic, readwrite) float emissiveR, emissiveG, emissiveB;
@property(nonatomic, readwrite) BOOL hasCustomShader;
@end

@implementation KumoEntityDetail
@end

namespace {

KumoEntityDetail* toKumoDetail(const facade::EngineRuntime::EntityDetail& detail) {
    KumoEntityDetail* out = [[KumoEntityDetail alloc] init];
    out.found = detail.found;
    out.entityId = toNSString(detail.id);
    out.name = toNSString(detail.name);
    out.primitive = toNSString(detail.primitive);
    out.positionX = detail.position.x;
    out.positionY = detail.position.y;
    out.positionZ = detail.position.z;
    out.eulerX = detail.eulerDeg.x;
    out.eulerY = detail.eulerDeg.y;
    out.eulerZ = detail.eulerDeg.z;
    out.scaleX = detail.scale.x;
    out.scaleY = detail.scale.y;
    out.scaleZ = detail.scale.z;
    out.hasMaterial = detail.hasMaterial;
    out.baseColorR = detail.material.baseColor[0];
    out.baseColorG = detail.material.baseColor[1];
    out.baseColorB = detail.material.baseColor[2];
    out.baseColorA = detail.material.baseColor[3];
    out.metallic = detail.material.metallic;
    out.roughness = detail.material.roughness;
    out.emissiveR = detail.material.emissive[0];
    out.emissiveG = detail.material.emissive[1];
    out.emissiveB = detail.material.emissive[2];
    out.hasCustomShader = detail.hasCustomShader;
    return out;
}

} // namespace

@interface KumoEngine () {
    // Declaration order is the destruction contract, mirroring EngineRuntime:
    // the runtime is torn down first (dealloc resets it explicitly below),
    // then the surface, then the device it was created from.
    rhi::Ptr<rhi::Device> _device;
    rhi::Ptr<rhi::Surface> _surface;
    std::unique_ptr<facade::EngineRuntime> _runtime;
    // Counts ticks, not rendered frames: render-on-demand skips rendering on
    // an idle scene, but the scripted-exit smoke test must still terminate.
    int _ticks;
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
    _ticks = 0;

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
    // Render-on-demand: nextDrawable blocks the main thread until vsync, so an
    // idle scene must skip drawable acquisition entirely rather than just
    // skipping the draw calls.
    if (_runtime->consumeRenderNeeded()) {
        rhi::Ptr<rhi::CommandEncoder> encoder = _device->queue().createCommandEncoder();
        rhi::Texture* target = _surface->acquireNextTexture();
        if (target != nullptr) {
            _runtime->render(*encoder, target);
        }
        encoder->finishAndSubmit(_surface.get());
    }

    if (_maxFrames > 0 && ++_ticks >= _maxFrames) {
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

- (NSArray<KumoEntityInfo*>*)listEntities {
    NSMutableArray<KumoEntityInfo*>* out = [NSMutableArray array];
    for (const facade::EngineRuntime::EntityInfo& entity : _runtime->listEntities()) {
        KumoEntityInfo* info = [[KumoEntityInfo alloc] init];
        info.entityId = toNSString(entity.id);
        info.name = toNSString(entity.name);
        info.primitive = toNSString(entity.primitive);
        [out addObject:info];
    }
    return out;
}

- (KumoEntityDetail*)entityDetail:(NSString*)entityId {
    return toKumoDetail(_runtime->entityDetail(toStdString(entityId)));
}

- (void)beginEdit:(NSString*)label {
    _runtime->beginEdit(toStdString(label));
}

- (BOOL)setEntityTransform:(NSString*)entityId
                        px:(float)px
                        py:(float)py
                        pz:(float)pz
                        rx:(float)rx
                        ry:(float)ry
                        rz:(float)rz
                        sx:(float)sx
                        sy:(float)sy
                        sz:(float)sz {
    return _runtime->setEntityTransform(toStdString(entityId), {px, py, pz}, {rx, ry, rz},
                                        {sx, sy, sz})
               ? YES
               : NO;
}

- (BOOL)setEntityMaterial:(NSString*)entityId
                        r:(float)r
                        g:(float)g
                        b:(float)b
                        a:(float)a
                 metallic:(float)metallic
                roughness:(float)roughness
                       er:(float)er
                       eg:(float)eg
                       eb:(float)eb {
    const renderer::ForwardRenderer::MaterialParams params{.baseColor = {r, g, b, a},
                                                           .metallic = metallic,
                                                           .roughness = roughness,
                                                           .emissive = {er, eg, eb}};
    return _runtime->setEntityMaterial(toStdString(entityId), params) ? YES : NO;
}

- (nullable NSString*)entityShaderSource:(NSString*)entityId {
    const std::optional<std::string> source = _runtime->entityShaderSource(toStdString(entityId));
    return source.has_value() ? toNSString(*source) : nil;
}

- (BOOL)clearEntityShader:(NSString*)entityId {
    return _runtime->clearEntityShader(toStdString(entityId)) ? YES : NO;
}

- (nullable NSString*)generatedShaderPath:(NSString*)entityId {
    const std::filesystem::path path = _runtime->generatedShaderPath(toStdString(entityId));
    return path.empty() ? nil : toNSString(path.string());
}

- (BOOL)undoAvailable {
    return _runtime->undoAvailable() ? YES : NO;
}

- (BOOL)redoAvailable {
    return _runtime->redoAvailable() ? YES : NO;
}

- (NSString*)undoLabel {
    return toNSString(_runtime->undoLabel());
}

- (NSString*)redoLabel {
    return toNSString(_runtime->redoLabel());
}

- (BOOL)undo {
    return _runtime->undo() ? YES : NO;
}

- (BOOL)redo {
    return _runtime->redo() ? YES : NO;
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
