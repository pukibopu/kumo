#version 460

layout(location = 0) in vec3 vDirection;

layout(location = 0) out vec4 fragColor;

layout(set = 1, binding = 0) uniform textureCube environmentMap;
layout(set = 1, binding = 1) uniform sampler environmentSampler;

void main() {
    vec3 color = texture(samplerCube(environmentMap, environmentSampler), vDirection).rgb;
    fragColor = vec4(color, 1.0);
}
