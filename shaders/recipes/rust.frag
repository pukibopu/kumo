float kumoHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float kumoNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(kumoHash(i), kumoHash(i + vec2(1, 0)), u.x),
               mix(kumoHash(i + vec2(0, 1)), kumoHash(i + vec2(1, 1)), u.x), u.y);
}

float kumoFbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 4; ++octave) {
        value += amplitude * kumoNoise(p);
        p *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {
    float field = kumoFbm(i.worldPos.xy * material.noiseScale + i.worldPos.zx * 0.7);
    float mask = smoothstep(1.0 - material.rustAmount, 1.2 - material.rustAmount, field);
    vec3 rust = mix(material.rustColor.rgb, material.rustColor.rgb * 0.6,
                    kumoNoise(i.worldPos.xz * material.noiseScale * 6.0));
    s.albedo = mix(s.albedo, rust, mask);
    s.metallic = mix(s.metallic, 0.0, mask);
    s.roughness = mix(s.roughness, 0.95, mask);
}
