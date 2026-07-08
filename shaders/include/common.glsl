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
}
frame;

#endif
