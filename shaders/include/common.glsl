#ifndef KUMO_COMMON_GLSL
#define KUMO_COMMON_GLSL

// Layout mirrored by renderer::FrameUniforms; keep std140 offsets in sync.
struct Light {
    vec4 positionType;   // xyz position (point), w type: 0 directional, 1 point
    vec4 colorIntensity; // rgb color, w intensity
    vec4 directionRange; // xyz direction (directional), w range (point)
};

layout(set = 0, binding = 0, std140) uniform FrameUniforms {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;         // xyz world position
    vec4 materialOverride;  // x metallic multiplier, y roughness multiplier
    Light lights[16];
    int lightCount;
    float prefilteredMipCount;
    mat4 lightViewProj;  // std140 offset 944 (8B pad after prefilteredMipCount)
    vec4 shadowParams;   // x enabled, y texel size, z depth bias, w shadow-casting light index
    vec4 timeParams;     // x seconds since renderer init, yzw reserved
}
frame;

layout(set = 0, binding = 1) uniform texture2D shadowMap;
layout(set = 0, binding = 2) uniform samplerShadow shadowSampler;
layout(set = 0, binding = 3) uniform sampler shadowRawSampler;

#endif
