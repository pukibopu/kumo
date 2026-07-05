#version 460

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

layout(set = 1, binding = 0) uniform texture2D baseColor;
layout(set = 1, binding = 1) uniform sampler baseSampler;

void main() {
    fragColor = texture(sampler2D(baseColor, baseSampler), vUv);
}
