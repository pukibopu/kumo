#ifndef KUMO_SHADOW_GLSL
#define KUMO_SHADOW_GLSL

// PCSS (contact-hardening PCF) directional shadow with normal-offset bias.

const float kLightSizeUV = 0.02;  // apparent sun size in shadow-UV space; drives max softness
const float kMaxRadiusUV = 0.015; // hard clamp on the PCF filter radius
const float kSearchRadiusUV = kLightSizeUV;

// Fixed 8-tap Poisson disk (PCSS reference implementation) for the blocker search.
const vec2 kPoissonDisk8[8] = vec2[](
    vec2(-0.326212, -0.405805), vec2(-0.840144, -0.073580), vec2(-0.695914, 0.457137),
    vec2(-0.203345, 0.620716), vec2(0.962340, -0.194983), vec2(0.473434, -0.480026),
    vec2(0.519456, 0.767022), vec2(0.185461, -0.893124));

// Fixed 16-tap Poisson disk (opengl-tutorial.org shadow mapping reference) for the PCF filter.
const vec2 kPoissonDisk16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// Interleaved gradient noise (Jimenez, "Next Generation Post-Processing in Call of
// Duty: Advanced Warfare"): cheap per-pixel rotation angle that hides the fixed
// Poisson taps' banding without a noise texture.
float kumoInterleavedGradientNoise(vec2 fragCoord) {
    return fract(52.9829189 * fract(0.06711056 * fragCoord.x + 0.00583715 * fragCoord.y));
}

float kumoShadowPcf(vec3 worldPos, vec3 normal) {
    if (frame.shadowParams.x < 0.5) return 1.0;

    // Normal-offset bias: nudge the receiver point along the surface normal by
    // ~1.5 shadow texels (world space) before projecting; avoids acne on sloped
    // surfaces without the peter-panning a large constant depth bias causes.
    // GLSL mat4 is column-major (m[col][row]); row 0 of the upper 3x3 of a
    // view*proj built from an orthonormal view is (m[0][0], m[1][0], m[2][0]),
    // whose length is 2/orthoWidth for an orthographic projection.
    vec3 row0 = vec3(frame.lightViewProj[0][0], frame.lightViewProj[1][0], frame.lightViewProj[2][0]);
    float len = max(length(row0), 1e-6);
    float orthoWidth = 2.0 / len;
    float texelWorld = orthoWidth / 2048.0;
    vec3 offsetPos = worldPos + normalize(normal) * texelWorld * 1.5;

    vec4 lp = frame.lightViewProj * vec4(offsetPos, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0 || ndc.z < 0.0) return 1.0;
    float receiver = ndc.z - frame.shadowParams.z;

    // Blocker search: raw depth reads (no hardware compare) averaged over a
    // fixed disk; early out to full light when nothing occludes the receiver.
    // Classification uses the UNBIASED depth (reference PCSS): the bias margin
    // exists to stop the final compare from self-shadowing, and excluding
    // contact-adjacent casters here would skew the penumbra estimate soft
    // exactly where hardening matters. False self-blockers this admits average
    // out to avgBlocker ~ ndc.z, so the clamp below degrades them to the
    // minimum (sharp) radius instead of acne.
    float blockerSum = 0.0;
    int blockerCount = 0;
    for (int i = 0; i < 8; ++i) {
        vec2 sampleUv = uv + kPoissonDisk8[i] * kSearchRadiusUV;
        float depth = texture(sampler2D(shadowMap, shadowRawSampler), sampleUv).r;
        if (depth < ndc.z) {
            blockerSum += depth;
            ++blockerCount;
        }
    }
    if (blockerCount == 0) return 1.0;
    float avgBlocker = blockerSum / float(blockerCount);

    // Penumbra estimate: similar-triangles between receiver, blocker and light.
    float penumbraUV = clamp((receiver - avgBlocker) * kLightSizeUV / max(avgBlocker, 1e-4),
                             frame.shadowParams.y, kMaxRadiusUV);

    // Rotate the filter disk per pixel (IGN) so the fixed taps do not leave a
    // visible fixed pattern across the penumbra.
    float angle = kumoInterleavedGradientNoise(gl_FragCoord.xy) * 6.28318530718;
    float s = sin(angle);
    float c = cos(angle);
    mat2 rotation = mat2(c, s, -s, c);

    float sum = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rotation * (kPoissonDisk16[i] * penumbraUV);
        sum += texture(sampler2DShadow(shadowMap, shadowSampler), vec3(uv + offset, receiver));
    }
    return sum / 16.0;
}

#endif
