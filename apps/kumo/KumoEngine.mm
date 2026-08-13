#import "KumoEngine.h"

#include <kumo/agent/confirmation_gate.h>
#include <kumo/agent/session.h>
#include <kumo/facade/engine_runtime.h>
#include <kumo/gpu/gpu.h>
#include <kumo/gpu/metal_interop.h>

#import <Security/Security.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace kumo;

namespace {

constexpr gpu::TextureFormat kSwapchainFormat = gpu::TextureFormat::BGRA8Unorm;

void configureSurface(gpu::Surface& surface, std::uint32_t width, std::uint32_t height) {
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

// fileSystemRepresentation (not UTF8String) so non-ASCII paths round-trip
// through the filesystem's actual on-disk byte form.
std::filesystem::path toPath(NSString* s) {
    return std::filesystem::path(s.fileSystemRepresentation);
}

// Chinese hints mirroring apps/viewer/ui.cpp's drawAgentPanels (ADR 0028: UI
// copy is Chinese, engine strings stay English).
NSString* const kSceneUnavailableHint =
    @"未配置模型：在 kumo.config.json 填写 provider（参考 kumo.config.example.json），或用 "
    @"--offline 运行离线演示脚本。";
NSString* const kShaderUnavailableHint =
    @"未配置 shader 模型：在 kumo.config.json 的 agents.shader 填 model（云端如 OpenAI 需同时填 "
    @"base_url 与 api_key）。";

agent::AgentSession* sessionFor(facade::EngineRuntime& runtime, KumoAgentKind kind) {
    return kind == KumoAgentKindScene ? runtime.sceneSession() : runtime.shaderSession();
}

facade::EngineRuntime::Notice& noticeFor(facade::EngineRuntime& runtime, KumoAgentKind kind) {
    return kind == KumoAgentKindScene ? runtime.sceneRetryNotice() : runtime.shaderRetryNotice();
}

// Mirrors apps/viewer/ui.cpp's statusText.
const char* statusText(agent::AgentSession::Status status) {
    switch (status) {
    case agent::AgentSession::Status::WaitingForModel:
        return "思考中…";
    case agent::AgentSession::Status::RunningTool:
        return "执行工具中…";
    case agent::AgentSession::Status::WaitingForConfirmation:
        return "等待确认…";
    case agent::AgentSession::Status::Idle:
        break;
    }
    return "输入消息，回车发送";
}

KumoTranscriptKind toKumoTranscriptKind(agent::AgentSession::TranscriptEntry::Kind kind) {
    using Kind = agent::AgentSession::TranscriptEntry::Kind;
    switch (kind) {
    case Kind::User:
        return KumoTranscriptKindUser;
    case Kind::Assistant:
        return KumoTranscriptKindAssistant;
    case Kind::ToolCall:
        return KumoTranscriptKindToolCall;
    case Kind::ToolResult:
        return KumoTranscriptKindToolResult;
    case Kind::Error:
        return KumoTranscriptKindError;
    case Kind::Info:
        break;
    }
    return KumoTranscriptKindInfo;
}

// Reads a Settings-saved key from the Keychain (service "dev.kumo.app",
// account = env-var name) and seeds the process env with it. `overwrite=0`
// (launch time) means "do not overwrite a real env var" already set; the
// config loader's env-over-file precedence (ADR 0012) then applies it the
// same as if the user had exported it themselves. `overwrite=1` is the
// settings-hot-apply path (Item D): the user just changed the value in-app
// and explicitly wants it to take effect now.
void applyKeychainEnv(NSString* account, const char* envName, int overwrite) {
    NSDictionary* query = @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : @"dev.kumo.app",
        (__bridge id)kSecAttrAccount : account,
        (__bridge id)kSecReturnData : @YES,
        (__bridge id)kSecMatchLimit : (__bridge id)kSecMatchLimitOne,
    };
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status != errSecSuccess || result == nullptr) {
        return;
    }
    NSData* data = (__bridge_transfer NSData*)result;
    NSString* value = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (value.length > 0) {
        setenv(envName, value.UTF8String, overwrite);
    }
}

} // namespace

@interface KumoLightDetail ()
@property(nonatomic, readwrite) BOOL found;
@property(nonatomic, readwrite) float azimuthDeg;
@property(nonatomic, readwrite) float elevationDeg;
@property(nonatomic, readwrite) float intensity;
@property(nonatomic, readwrite) float colorR, colorG, colorB;
@end

@implementation KumoLightDetail
@end

namespace {

KumoLightDetail* toKumoLightDetail(const facade::EngineRuntime::LightState& state) {
    KumoLightDetail* out = [[KumoLightDetail alloc] init];
    out.found = state.found;
    out.azimuthDeg = state.azimuthDeg;
    out.elevationDeg = state.elevationDeg;
    out.intensity = state.intensity;
    out.colorR = state.color.x;
    out.colorG = state.color.y;
    out.colorB = state.color.z;
    return out;
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

@interface KumoSurfaceParam ()
@property(nonatomic, readwrite) NSString* name;
@property(nonatomic, readwrite) BOOL isVec4;
@property(nonatomic, readwrite) float value0, value1, value2, value3;
@end

@implementation KumoSurfaceParam
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

@interface KumoTranscriptEntry ()
@property(nonatomic, readwrite) KumoTranscriptKind kind;
@property(nonatomic, readwrite) NSString* text;
@property(nonatomic, readwrite) NSString* toolName;
@property(nonatomic, readwrite) NSString* json;
@end

@implementation KumoTranscriptEntry
@end

@interface KumoConfirmPrompt ()
@property(nonatomic, readwrite) uint64_t promptId;
@property(nonatomic, readwrite) NSString* toolName;
@property(nonatomic, readwrite) NSString* argumentsJson;
@end

@implementation KumoConfirmPrompt
@end

@interface KumoDirectorEvent ()
@property(nonatomic, readwrite) KumoDirectorEventKind kind;
@property(nonatomic, readwrite) NSString* stage;
@property(nonatomic, readwrite) NSString* text;
@property(nonatomic, readwrite) NSArray<NSString*>* imagePaths;
@end

@implementation KumoDirectorEvent
@end

namespace {

KumoTranscriptEntry* toKumoEntry(const agent::AgentSession::TranscriptEntry& entry) {
    KumoTranscriptEntry* out = [[KumoTranscriptEntry alloc] init];
    out.kind = toKumoTranscriptKind(entry.kind);
    out.text = toNSString(entry.text);
    out.toolName = toNSString(entry.toolName);
    out.json = toNSString(entry.json);
    return out;
}

KumoConfirmPrompt* toKumoPrompt(const agent::ConfirmationGate::Prompt& prompt) {
    KumoConfirmPrompt* out = [[KumoConfirmPrompt alloc] init];
    out.promptId = prompt.id;
    out.toolName = toNSString(prompt.toolName);
    out.argumentsJson = toNSString(prompt.argumentsJson);
    return out;
}

} // namespace

@interface KumoEngine () {
    // Declaration order is the destruction contract, mirroring EngineRuntime:
    // the runtime is torn down first (dealloc resets it explicitly below),
    // then the surface, then the device it was created from.
    gpu::Ptr<gpu::Device> _device;
    gpu::Ptr<gpu::Surface> _surface;
    std::unique_ptr<facade::EngineRuntime> _runtime;
    // Counts ticks, not rendered frames: render-on-demand skips rendering on
    // an idle scene, but the scripted-exit smoke test must still terminate.
    int _ticks;
    int _maxFrames;
    NSString* _configPath;
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
    _configPath = [configPath copy];

    _device = gpu::createDevice();
    if (!_device) {
        return nil;
    }
    auto* metalLayer = reinterpret_cast<CA::MetalLayer*>((__bridge void*)layer);
    _surface = gpu::metal::createSurface(*_device, metalLayer, false);
    if (!_surface) {
        return nil;
    }
    const double scale = layer.contentsScale > 0 ? layer.contentsScale : 1.0;
    const auto width = static_cast<std::uint32_t>(std::max(1.0, layer.bounds.size.width * scale));
    const auto height = static_cast<std::uint32_t>(std::max(1.0, layer.bounds.size.height * scale));
    configureSurface(*_surface, width, height);

    // Settings-saved keys live only in the Keychain (ADR 0044 slice G4); seed
    // the process env from them before the config layer resolves providers, so
    // its existing env-over-file precedence (ADR 0012) picks them up unchanged.
    // `setenv(..., 0)` never overwrites a real env var the user already set.
    applyKeychainEnv(@"OPENAI_API_KEY", "OPENAI_API_KEY", 0);
    applyKeychainEnv(@"ANTHROPIC_API_KEY", "ANTHROPIC_API_KEY", 0);

    const std::string assetDirStd = assetDir.UTF8String;
    const facade::EngineRuntime::Desc desc{
        .modelPath = assetDirStd + "/models/DamagedHelmet.glb",
        .envPath = assetDirStd + "/env/studio_small_09_2k.hdr",
        .configPath = configPath.UTF8String,
        .envFilePath = envFilePath.UTF8String,
        .shaderDir = shaderDir.UTF8String,
        .assetDir = assetDirStd,
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
        std::optional<gpu::SurfaceFrame> surfaceFrame = _surface->acquire();
        if (surfaceFrame) {
            gpu::Ptr<gpu::CommandEncoder> encoder = _device->queue().createCommandEncoder();
            _runtime->render(*encoder, &surfaceFrame->texture());
            encoder->finishAndSubmit(&*surfaceFrame);
        } else {
            // Drawable starvation (occluded/minimized/timeout) must not consume
            // the render-on-demand token permanently.
            _runtime->markDirty();
        }
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

- (void)orbitRotateDX:(float)dx dy:(float)dy {
    _runtime->orbitRotate(dx, dy);
}

- (void)orbitZoom:(float)delta {
    _runtime->orbitZoom(delta);
}

- (void)panCameraForward:(float)forward right:(float)right {
    _runtime->orbitPan(forward, right);
}

- (float)orbitDistance {
    return _runtime->orbitDistance();
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

- (NSArray<KumoSurfaceParam*>*)entitySurfaceParams:(NSString*)entityId {
    NSMutableArray<KumoSurfaceParam*>* out = [NSMutableArray array];
    for (const renderer::ForwardRenderer::SurfaceParam& param :
         _runtime->entitySurfaceParams(toStdString(entityId))) {
        KumoSurfaceParam* item = [[KumoSurfaceParam alloc] init];
        item.name = toNSString(param.name);
        item.isVec4 = param.isVec4 ? YES : NO;
        item.value0 = param.value[0];
        item.value1 = param.value[1];
        item.value2 = param.value[2];
        item.value3 = param.value[3];
        [out addObject:item];
    }
    return out;
}

- (BOOL)setEntitySurfaceParam:(NSString*)entityId
                         name:(NSString*)name
                       values:(NSArray<NSNumber*>*)values {
    std::vector<float> value;
    for (NSNumber* number in values) {
        value.push_back(number.floatValue);
    }
    return _runtime->setEntitySurfaceParam(toStdString(entityId), toStdString(name), value) ? YES
                                                                                            : NO;
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

- (KumoLightDetail*)sunLightDetail {
    return toKumoLightDetail(_runtime->sunLightState());
}

- (BOOL)setSunLightAzimuthDeg:(float)azimuthDeg
                 elevationDeg:(float)elevationDeg
                    intensity:(float)intensity
                            r:(float)r
                            g:(float)g
                            b:(float)b {
    return _runtime->setSunLight(azimuthDeg, elevationDeg, intensity, {r, g, b}) ? YES : NO;
}

- (BOOL)shadowsEnabled {
    return _runtime->renderer().shadowsEnabled() ? YES : NO;
}

- (void)setShadowsEnabled:(BOOL)enabled {
    _runtime->renderer().setShadowsEnabled(enabled == YES);
    _runtime->markDirty();
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

- (BOOL)agentAvailable:(KumoAgentKind)kind {
    return sessionFor(*_runtime, kind) != nullptr ? YES : NO;
}

- (NSString*)agentHint:(KumoAgentKind)kind {
    if (sessionFor(*_runtime, kind) != nullptr) {
        return @"";
    }
    return kind == KumoAgentKindScene ? kSceneUnavailableHint : kShaderUnavailableHint;
}

- (BOOL)agentBusy:(KumoAgentKind)kind {
    agent::AgentSession* session = sessionFor(*_runtime, kind);
    return session != nullptr && session->busy() ? YES : NO;
}

- (NSString*)agentStatusLine:(KumoAgentKind)kind {
    agent::AgentSession* session = sessionFor(*_runtime, kind);
    if (session == nullptr) {
        return @"";
    }
    const agent::AgentSession::Status status = session->status();
    std::string line = statusText(status);
    facade::EngineRuntime::Notice& notice = noticeFor(*_runtime, kind);
    if (status == agent::AgentSession::Status::WaitingForModel) {
        if (const std::string retry = notice.get(); !retry.empty()) {
            line = retry;
        }
    } else {
        notice.clear();
    }
    return toNSString(line);
}

- (BOOL)submit:(KumoAgentKind)kind text:(NSString*)text {
    agent::AgentSession* session = sessionFor(*_runtime, kind);
    return session != nullptr && session->submit(toStdString(text)) ? YES : NO;
}

- (BOOL)submit:(KumoAgentKind)kind
           text:(NSString*)text
     imagePaths:(NSArray<NSString*>*)imagePaths
    imageDetail:(NSString*)imageDetail {
    agent::AgentSession* session = sessionFor(*_runtime, kind);
    if (session == nullptr) {
        return NO;
    }
    std::vector<agent::UserImage> images;
    for (NSString* path in imagePaths) {
        NSString* ext = [[path pathExtension] lowercaseString];
        std::string mediaType;
        if ([ext isEqualToString:@"png"]) {
            mediaType = "image/png";
        } else if ([ext isEqualToString:@"jpg"] || [ext isEqualToString:@"jpeg"]) {
            mediaType = "image/jpeg";
        } else {
            return NO;
        }
        NSData* data = [NSData dataWithContentsOfFile:path];
        // The Swift side pre-downscales to <=1024px; the cap is a safety net
        // against oversized requests from any other caller.
        if (data == nil || data.length == 0 || data.length > 8 * 1024 * 1024) {
            return NO;
        }
        images.push_back({.base64 = toStdString([data base64EncodedStringWithOptions:0]),
                          .mediaType = std::move(mediaType)});
    }
    return session->submit(toStdString(text), std::move(images), toStdString(imageDetail)) ? YES
                                                                                           : NO;
}

- (NSArray<KumoTranscriptEntry*>*)drainTranscript:(KumoAgentKind)kind {
    NSMutableArray<KumoTranscriptEntry*>* out = [NSMutableArray array];
    agent::AgentSession* session = sessionFor(*_runtime, kind);
    if (session == nullptr) {
        return out;
    }
    for (const agent::AgentSession::TranscriptEntry& entry : session->drainTranscript()) {
        [out addObject:toKumoEntry(entry)];
    }
    return out;
}

- (nullable KumoConfirmPrompt*)pendingConfirm {
    agent::ConfirmationGate* gate = _runtime->confirmGate();
    if (gate == nullptr) {
        return nil;
    }
    const std::optional<agent::ConfirmationGate::Prompt> prompt = gate->pending();
    return prompt.has_value() ? toKumoPrompt(*prompt) : nil;
}

- (void)resolveConfirm:(uint64_t)promptId approved:(BOOL)approved {
    agent::ConfirmationGate* gate = _runtime->confirmGate();
    if (gate == nullptr) {
        return;
    }
    gate->resolve(promptId, approved == YES);
}

- (BOOL)directorConfigured {
    return _runtime->directorSession() != nullptr;
}

- (BOOL)criticConfigured {
    return _runtime->criticSession() != nullptr;
}

- (BOOL)directorActive {
    facade::Director* director = _runtime->director();
    return director != nullptr && director->active();
}

- (NSString*)directorState {
    facade::Director* director = _runtime->director();
    return toNSString(director != nullptr ? facade::Director::stateName(director->state())
                                          : "idle");
}

- (BOOL)directorStart:(NSString*)brief hero:(BOOL)hero {
    facade::Director* director = _runtime->director();
    if (director == nullptr) {
        return NO;
    }
    return director->start(std::string([brief UTF8String] ?: ""),
                           hero ? facade::Director::Tier::Hero : facade::Director::Tier::Standard)
               ? YES
               : NO;
}

- (void)directorCancel {
    if (facade::Director* director = _runtime->director()) {
        director->cancel();
    }
}

- (NSArray<KumoDirectorEvent*>*)directorDrainEvents {
    NSMutableArray<KumoDirectorEvent*>* out = [NSMutableArray array];
    facade::Director* director = _runtime->director();
    if (director == nullptr) {
        return out;
    }
    for (const facade::Director::Event& event : director->drainEvents()) {
        KumoDirectorEvent* bridged = [[KumoDirectorEvent alloc] init];
        switch (event.kind) {
        case facade::Director::Event::Kind::Stage:
            bridged.kind = KumoDirectorEventKindStage;
            break;
        case facade::Director::Event::Kind::Note:
            bridged.kind = KumoDirectorEventKindNote;
            break;
        case facade::Director::Event::Kind::Screenshots:
            bridged.kind = KumoDirectorEventKindScreenshots;
            break;
        case facade::Director::Event::Kind::Verdict:
            bridged.kind = KumoDirectorEventKindVerdict;
            break;
        case facade::Director::Event::Kind::Error:
            bridged.kind = KumoDirectorEventKindError;
            break;
        }
        bridged.stage = toNSString(facade::Director::stateName(event.stage));
        bridged.text = toNSString(event.text);
        NSMutableArray<NSString*>* paths = [NSMutableArray array];
        for (const std::string& path : event.imagePaths) {
            [paths addObject:toNSString(path)];
        }
        bridged.imagePaths = paths;
        [out addObject:bridged];
    }
    return out;
}

- (NSString*)configPath {
    return _configPath;
}

- (BOOL)saveSceneToPath:(NSString*)path {
    return _runtime->saveScene(toPath(path)) ? YES : NO;
}

- (BOOL)loadSceneFromPath:(NSString*)path {
    // loadScene() already calls markDirty() on success (facade contract).
    return _runtime->loadScene(toPath(path)) ? YES : NO;
}

- (BOOL)reloadAgentSessions {
    applyKeychainEnv(@"OPENAI_API_KEY", "OPENAI_API_KEY", 1);
    applyKeychainEnv(@"ANTHROPIC_API_KEY", "ANTHROPIC_API_KEY", 1);
    return _runtime->reloadAgentSessions() ? YES : NO;
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
