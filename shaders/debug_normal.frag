#version 460
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

// pbr.frag's full binding surface, unused but declared so the reflected
// layouts stay identical (ADR 0029).
layout(set = 1, binding = 0) uniform texture2D baseColorTex;
layout(set = 1, binding = 1) uniform texture2D metallicRoughnessTex;
layout(set = 1, binding = 2) uniform texture2D normalTex;
layout(set = 1, binding = 3) uniform texture2D occlusionTex;
layout(set = 1, binding = 4) uniform texture2D emissiveTex;
layout(set = 1, binding = 5) uniform sampler materialSampler;
layout(set = 1, binding = 6, std140) uniform MaterialFactors {
    vec4 baseColor;
    vec4 metallicRoughness;
    vec4 emissive;
    vec4 uvTiling;
}
material;

layout(set = 2, binding = 0) uniform textureCube irradianceMap;
layout(set = 2, binding = 1) uniform textureCube prefilteredMap;
layout(set = 2, binding = 2) uniform texture2D brdfLut;
layout(set = 2, binding = 3) uniform sampler iblSampler;

void main() {
    fragColor = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0);
}
