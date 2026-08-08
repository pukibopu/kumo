float kumoHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {
    float streak = kumoHash(vec2(floor(i.uv.y * material.brushScale), 0.0));
    float fine = kumoHash(vec2(floor(i.uv.y * material.brushScale * 7.0), 1.0));
    s.albedo = material.tint.rgb * (0.85 + 0.15 * streak);
    s.metallic = 1.0;
    s.roughness = mix(material.roughnessMin, material.roughnessMax, 0.6 * streak + 0.4 * fine);
}
