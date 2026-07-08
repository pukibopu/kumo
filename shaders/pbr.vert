#version 460
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
// Unused: the fragment shader derives its tangent frame from screen-space
// derivatives. Declared so the fixed vertex layout binds without warnings.
layout(location = 2) in vec4 tangent;
layout(location = 3) in vec2 uv;

layout(push_constant) uniform PerDraw {
    mat4 model;
    mat4 normalMatrix;
}
draw;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;

void main() {
    vec4 world = draw.model * vec4(position, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(draw.normalMatrix) * normal;
    vUv = uv;
    gl_Position = frame.proj * frame.view * world;
}
