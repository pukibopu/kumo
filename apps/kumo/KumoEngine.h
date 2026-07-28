#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

// Scene tree row (ADR 0044): a flattened, value-typed mirror of
// kumo::facade::EngineRuntime::EntityInfo.
@interface KumoEntityInfo : NSObject
@property(nonatomic, readonly) NSString* entityId;
@property(nonatomic, readonly) NSString* name;
@property(nonatomic, readonly) NSString* primitive; // empty when glTF-sourced
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

- (BOOL)undoAvailable;
- (BOOL)redoAvailable;
- (NSString*)undoLabel; // empty when none
- (NSString*)redoLabel; // empty when none
- (BOOL)undo;
- (BOOL)redo;

@end

NS_ASSUME_NONNULL_END
