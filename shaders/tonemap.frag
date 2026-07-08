#version 460

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

layout(set = 1, binding = 0) uniform texture2D hdrColor;
layout(set = 1, binding = 1) uniform sampler hdrSampler;

// ACES filmic fit (Narkowicz 2015).
vec3 acesTonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// The swapchain view is Unorm, so sRGB encoding happens here.
vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec3 hdr = texture(sampler2D(hdrColor, hdrSampler), vUv).rgb;
    fragColor = vec4(linearToSrgb(acesTonemap(hdr)), 1.0);
}
