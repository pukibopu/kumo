#ifndef KUMO_CUBEMAP_GLSL
#define KUMO_CUBEMAP_GLSL

// World direction for a cube face texel; uv in [-1, 1], face order +X -X +Y -Y
// +Z -Z (identical layout on Metal and Vulkan).
vec3 cubeFaceDirection(int face, vec2 uv) {
    if (face == 0) {
        return normalize(vec3(1.0, -uv.y, -uv.x));
    }
    if (face == 1) {
        return normalize(vec3(-1.0, -uv.y, uv.x));
    }
    if (face == 2) {
        return normalize(vec3(uv.x, 1.0, uv.y));
    }
    if (face == 3) {
        return normalize(vec3(uv.x, -1.0, -uv.y));
    }
    if (face == 4) {
        return normalize(vec3(uv.x, -uv.y, 1.0));
    }
    return normalize(vec3(-uv.x, -uv.y, -1.0));
}

#endif
