#version 460
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"

layout(location = 0) out vec3 vDirection;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    // z = 0 is the reversed-Z far plane; drawn after opaque geometry with
    // GreaterEqual and depth writes off, it fills only untouched pixels.
    gl_Position = vec4(ndc, 0.0, 1.0);

    // Unproject analytically: the projection only scales x/y, and the view
    // matrix is rigid so its rotation transposes to an inverse.
    vec3 viewDir = vec3(ndc.x / frame.proj[0][0], ndc.y / frame.proj[1][1], -1.0);
    vDirection = transpose(mat3(frame.view)) * viewDir;
}
