#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

// Scene tree row (ADR 0044): a flattened, value-typed mirror of
// kumo::facade::EngineRuntime::EntityInfo.
@interface KumoEntityInfo : NSObject
@property(nonatomic, readonly) NSString* entityId;
@property(nonatomic, readonly) NSString* name;
@property(nonatomic, readonly) NSString* primitive; // empty when glTF-sourced
@end

// Sun light (world().light(0)) snapshot: mirrors apps/viewer/ui.cpp's
// LightSettings az/el<->direction formulas. `found` is NO when index 0 does
// not exist (empty scene); every other field is meaningless in that case.
@interface KumoLightDetail : NSObject
@property(nonatomic, readonly) BOOL found;
@property(nonatomic, readonly) float azimuthDeg;
@property(nonatomic, readonly) float elevationDeg;
@property(nonatomic, readonly) float intensity;
@property(nonatomic, readonly) float colorR, colorG, colorB;
@end

// Inspector snapshot of one entity (ADR 0044): a flattened, value-typed mirror
// of kumo::facade::EngineRuntime::EntityDetail. Flat floats (not simd) keep it
// Swift-friendly and match the setEntityTransform/setEntityMaterial signatures
// below. `found` is NO when the id no longer resolves (e.g. after an undo);
// every other field is meaningless in that case.
@interface KumoEntityDetail : NSObject
@property(nonatomic, readonly) BOOL found;
@property(nonatomic, readonly) NSString* entityId;
@property(nonatomic, readonly) NSString* name;
@property(nonatomic, readonly) NSString* primitive;
@property(nonatomic, readonly) float positionX, positionY, positionZ;
@property(nonatomic, readonly) float eulerX, eulerY, eulerZ;
@property(nonatomic, readonly) float scaleX, scaleY, scaleZ;
@property(nonatomic, readonly) BOOL hasMaterial;
@property(nonatomic, readonly) float baseColorR, baseColorG, baseColorB, baseColorA;
@property(nonatomic, readonly) float metallic;
@property(nonatomic, readonly) float roughness;
@property(nonatomic, readonly) float emissiveR, emissiveG, emissiveB;
@property(nonatomic, readonly) BOOL hasCustomShader;
@end

// Which agent session a chat call targets (ADR 0044 slice G4): mirrors
// EngineRuntime::sceneSession()/shaderSession().
typedef NS_ENUM(NSInteger, KumoAgentKind) {
    KumoAgentKindScene = 0,
    KumoAgentKindShader = 1,
};

// Flattened mirror of kumo::agent::AgentSession::TranscriptEntry::Kind.
typedef NS_ENUM(NSInteger, KumoTranscriptKind) {
    KumoTranscriptKindUser,
    KumoTranscriptKindAssistant,
    KumoTranscriptKindToolCall,
    KumoTranscriptKindToolResult,
    KumoTranscriptKindError,
    KumoTranscriptKindInfo,
};

// One transcript line (ADR 0044): value-typed mirror of
// kumo::agent::AgentSession::TranscriptEntry. `toolName`/`json` are only
// meaningful for the ToolCall/ToolResult kinds, matching the C++ struct.
@interface KumoTranscriptEntry : NSObject
@property(nonatomic, readonly) KumoTranscriptKind kind;
@property(nonatomic, readonly) NSString *text, *toolName, *json;
@end

// A destructive-tool confirmation awaiting a decision (ADR 0022): mirror of
// kumo::agent::ConfirmationGate::Prompt.
@interface KumoConfirmPrompt : NSObject
@property(nonatomic, readonly) uint64_t promptId;
@property(nonatomic, readonly) NSString *toolName, *argumentsJson;
@end

// ObjC++ facade over kumo::facade::EngineRuntime (ADR 0044): the only seam the
// Swift shell talks to. All methods must be called on the main thread.
@interface KumoEngine : NSObject

// Returns nil when the engine fails to assemble (already logged to stderr).
- (nullable instancetype)initWithLayer:(CAMetalLayer*)layer
                              assetDir:(NSString*)assetDir
                             shaderDir:(NSString*)shaderDir
                            configPath:(NSString*)configPath
                           envFilePath:(NSString*)envFilePath;

// Convenience over the initializer above: every path comes from this target's
// own compile definitions (KUMO_ASSET_DIR/KUMO_SHADER_DIR/KUMO_REPO_DIR), so
// the Swift shell only ever hands over the layer.
- (nullable instancetype)initWithLayerUsingDefaults:(CAMetalLayer*)layer;

// Pumps tool work and renders one frame. NO means the runtime asked to quit.
- (BOOL)tick;
- (void)resizeWidth:(uint32_t)width height:(uint32_t)height;

// Orbit camera input (ADR 0039): dx/dy are in the GLFW convention (y grows
// downward); AppKit's mouse events are y-up, so the caller flips the sign.
- (void)orbitRotateDX:(float)dx dy:(float)dy;
- (void)orbitZoom:(float)delta;
// Pans the pivot along the camera's horizontal forward/right axes, meters
// (ADR 0039 arbitration, same as orbitRotate/orbitZoom). Product GUI's WASD.
- (void)panCameraForward:(float)forward right:(float)right;
// Current orbit distance, for scaling WASD pan speed by the framing.
- (float)orbitDistance;

// Scene tree + inspector (ADR 0044).
- (NSArray<KumoEntityInfo*>*)listEntities;
- (KumoEntityDetail*)entityDetail:(NSString*)entityId;

// Records one undo point; subsequent setEntityTransform/setEntityMaterial
// calls until the next beginEdit (or an agent tool call) belong to that
// gesture.
- (void)beginEdit:(NSString*)label;
- (BOOL)setEntityTransform:(NSString*)entityId
                        px:(float)px
                        py:(float)py
                        pz:(float)pz
                        rx:(float)rx
                        ry:(float)ry
                        rz:(float)rz
                        sx:(float)sx
                        sy:(float)sy
                        sz:(float)sz;
- (BOOL)setEntityMaterial:(NSString*)entityId
                        r:(float)r
                        g:(float)g
                        b:(float)b
                        a:(float)a
                 metallic:(float)metallic
                roughness:(float)roughness
                       er:(float)er
                       eg:(float)eg
                       eb:(float)eb;
- (nullable NSString*)entityShaderSource:(NSString*)entityId;
- (BOOL)clearEntityShader:(NSString*)entityId; // records its own undo point
- (nullable NSString*)generatedShaderPath:(NSString*)entityId;

// Sun light editor (light index 0) + shadow toggle: reuses the same
// beginEdit/commit undo model as setEntityTransform/setEntityMaterial above.
// `setSunLight...` returns NO (no undo step, pending snapshot left open) when
// light 0 does not exist; the Swift side hides/disables the section then.
- (KumoLightDetail*)sunLightDetail;
- (BOOL)setSunLightAzimuthDeg:(float)azimuthDeg
                 elevationDeg:(float)elevationDeg
                    intensity:(float)intensity
                            r:(float)r
                            g:(float)g
                            b:(float)b;
// ForwardRenderer::shadowsEnabled mirror: a renderer setting, not scene state,
// so it is not part of the undo stack (matches the GLFW viewer's checkbox).
- (BOOL)shadowsEnabled;
- (void)setShadowsEnabled:(BOOL)enabled;

- (BOOL)undoAvailable;
- (BOOL)redoAvailable;
- (NSString*)undoLabel; // empty when none
- (NSString*)redoLabel; // empty when none
- (BOOL)undo;
- (BOOL)redo;

// Chat, tool log and destructive-confirmation surface (ADR 0044 slice G4).
// UI copy returned here is Chinese (app-side, ADR 0028); engine-facing
// strings stay English and never cross this seam.
- (BOOL)agentAvailable:(KumoAgentKind)kind;
// Chinese configuration hint when unavailable; empty string otherwise.
- (NSString*)agentHint:(KumoAgentKind)kind;
- (BOOL)agentBusy:(KumoAgentKind)kind;
// Chinese status line (思考中…/执行工具中…/等待确认…/idle hint), with the
// provider retry notice overriding it while WaitingForModel.
- (NSString*)agentStatusLine:(KumoAgentKind)kind;
- (BOOL)submit:(KumoAgentKind)kind text:(NSString*)text;
// Moves the session's pending entries out; call every poll tick for both
// kinds regardless of which tab is visible, or the engine-side transcript
// accumulates unbounded.
- (NSArray<KumoTranscriptEntry*>*)drainTranscript:(KumoAgentKind)kind;
- (nullable KumoConfirmPrompt*)pendingConfirm;
- (void)resolveConfirm:(uint64_t)promptId approved:(BOOL)approved;

// Full path to kumo.config.json, for the settings form to read/overlay/write
// (api_key fields excluded; those live in the Keychain only).
- (NSString*)configPath;

// Scene persistence (ADR 0044 slice: product GUI's Cmd+S/Cmd+O). `path` is a
// full file path chosen by the caller's NSSavePanel/NSOpenPanel.
- (BOOL)saveSceneToPath:(NSString*)path;
- (BOOL)loadSceneFromPath:(NSString*)path;

// Settings hot apply (ADR 0044 slice G4): re-seeds the process env from the
// Keychain with overwrite (the user just changed a key in-app, unlike the
// launch-time seed which never clobbers a real env var), then rebuilds both
// agent sessions from the current config/env. NO when a session is still
// mid-turn (unchanged, logged); the caller does not retry automatically.
- (BOOL)reloadAgentSessions;

@end

NS_ASSUME_NONNULL_END
