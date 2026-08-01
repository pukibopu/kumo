#version 460

layout(location = 0) in vec3 inPosition;

layout(set = 0, binding = 0, std140) uniform ShadowUniforms { mat4 lightViewProj; } shadow;

layout(push_constant) uniform PerDraw { mat4 model; } draw;

void main() { gl_Position = shadow.lightViewProj * draw.model * vec4(inPosition, 1.0); }
