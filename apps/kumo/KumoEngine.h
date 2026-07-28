#pragma once
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

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

@end

NS_ASSUME_NONNULL_END
