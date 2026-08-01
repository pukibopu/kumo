#ifndef KUMO_SHADOW_GLSL
#define KUMO_SHADOW_GLSL

float kumoShadowPcf(vec3 worldPos) {
    if (frame.shadowParams.x < 0.5) return 1.0;
    vec4 lp = frame.lightViewProj * vec4(worldPos, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * vec2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0 || ndc.z < 0.0) return 1.0;
    float ref = ndc.z - frame.shadowParams.z;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(sampler2DShadow(shadowMap, shadowSampler),
                           vec3(uv + vec2(x, y) * frame.shadowParams.y, ref));
    return sum / 9.0;
}

#endif
