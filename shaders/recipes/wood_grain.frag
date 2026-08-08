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

void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {
    vec2 p = i.worldPos.xz * material.grainScale;
    float wobble = kumoNoise(p * 0.7) * material.turbulence;
    float rings = fract((length(p) + wobble) * 0.5 + kumoNoise(p * 4.0) * 0.1);
    float grain = smoothstep(0.3, 0.7, rings);
    s.albedo = mix(material.lightColor.rgb, material.darkColor.rgb, grain);
    s.roughness = mix(0.55, 0.85, grain);
    s.metallic = 0.0;
}
