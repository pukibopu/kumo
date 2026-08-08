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
    vec2 p = i.worldPos.xz * material.veinScale;
    float turbulence = kumoFbm(p * 2.0) * 2.5;
    float veins = pow(abs(sin((p.x + p.y) * 1.5 + turbulence)), 0.35);
    s.albedo = mix(material.veinColor.rgb, material.baseTone.rgb, veins);
    s.roughness = mix(0.15, 0.35, veins);
    s.metallic = 0.0;
}
