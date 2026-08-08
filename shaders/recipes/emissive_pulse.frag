void kumoSurface(inout SurfaceOutputs s, in SurfaceInputs i) {
    float pulse = 0.5 + 0.5 * sin(i.time * material.speed + i.worldPos.y * material.phaseByHeight);
    s.emissive = material.glowColor.rgb * material.intensity * pulse;
}
