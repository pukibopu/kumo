#version 460
#extension GL_GOOGLE_include_directive : require

#include "common.glsl"
#include "shadow.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;

layout(location = 0) out vec4 fragColor;

layout(set = 1, binding = 0) uniform texture2D baseColorTex;
layout(set = 1, binding = 1) uniform texture2D metallicRoughnessTex;
layout(set = 1, binding = 2) uniform texture2D normalTex;
layout(set = 1, binding = 3) uniform texture2D occlusionTex;
layout(set = 1, binding = 4) uniform texture2D emissiveTex;
layout(set = 1, binding = 5) uniform sampler materialSampler;
layout(set = 1, binding = 6, std140) uniform MaterialFactors {
    vec4 baseColor;
    vec4 metallicRoughness; // x metallic, y roughness
    vec4 emissive;          // xyz
    vec4 uvTiling;          // xy uv scale, zw reserved
}
material;

layout(set = 2, binding = 0) uniform textureCube irradianceMap;
layout(set = 2, binding = 1) uniform textureCube prefilteredMap;
layout(set = 2, binding = 2) uniform texture2D brdfLut;
layout(set = 2, binding = 3) uniform sampler iblSampler;

const float PI = 3.14159265359;

// Cotangent frame from screen-space derivatives (Mikkelsen); the source meshes
// carry no reliable tangents, so vertex tangents are ignored entirely.
vec3 perturbNormal(vec3 N, vec3 worldPos, vec2 uv) {
    vec3 texNormal = texture(sampler2D(normalTex, materialSampler), uv).xyz * 2.0 - 1.0;

    vec3 dp1 = dFdx(worldPos);
    vec3 dp2 = dFdy(worldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-20);
    return normalize(mat3(T * invmax, B * invmax, N) * texNormal);
}

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec2 uv = vUv * material.uvTiling.xy;

    vec4 baseSample = texture(sampler2D(baseColorTex, materialSampler), uv) * material.baseColor;
    vec3 albedo = baseSample.rgb;

    vec3 mrSample = texture(sampler2D(metallicRoughnessTex, materialSampler), uv).rgb;
    float metallic = clamp(
        mrSample.b * material.metallicRoughness.x * frame.materialOverride.x, 0.0, 1.0);
    float roughness = clamp(
        mrSample.g * material.metallicRoughness.y * frame.materialOverride.y, 0.045, 1.0);
    float occlusion = texture(sampler2D(occlusionTex, materialSampler), uv).r;
    vec3 emissive =
        texture(sampler2D(emissiveTex, materialSampler), uv).rgb * material.emissive.rgb;

    vec3 V = normalize(frame.cameraPos.xyz - vWorldPos);
    vec3 N = perturbNormal(normalize(vNormal), vWorldPos, uv);
    float NdotV = max(dot(N, V), 1e-4);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 direct = vec3(0.0);
    for (int i = 0; i < frame.lightCount; ++i) {
        Light light = frame.lights[i];
        vec3 L;
        float attenuation = 1.0;
        if (light.positionType.w < 0.5) {
            L = normalize(-light.directionRange.xyz);
        } else {
            vec3 toLight = light.positionType.xyz - vWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-4);
            attenuation = 1.0 / max(dist * dist, 1e-4);
        }
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) {
            continue;
        }
        vec3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);

        float D = distributionGGX(NdotH, roughness);
        float G = geometrySmith(NdotV, NdotL, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 1e-4);
        vec3 kd = (1.0 - F) * (1.0 - metallic);

        float shadow = i == int(frame.shadowParams.w) ? kumoShadowPcf(vWorldPos, N) : 1.0;
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation * shadow;
        direct += (kd * albedo / PI + specular) * radiance * NdotL;
    }

    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    vec3 irradiance = texture(samplerCube(irradianceMap, iblSampler), N).rgb;
    vec3 diffuseIbl = kd * irradiance * albedo;

    vec3 R = reflect(-V, N);
    float lod = roughness * (frame.prefilteredMipCount - 1.0);
    vec3 prefiltered = textureLod(samplerCube(prefilteredMap, iblSampler), R, lod).rgb;
    vec2 brdf = texture(sampler2D(brdfLut, iblSampler), vec2(NdotV, roughness)).rg;
    vec3 specularIbl = prefiltered * (F * brdf.x + brdf.y);

    vec3 ambient = (diffuseIbl + specularIbl) * occlusion;
    fragColor = vec4(direct + ambient + emissive, baseSample.a);
}
