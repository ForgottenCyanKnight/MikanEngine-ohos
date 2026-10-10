#version 450
precision highp float;
precision highp int;

// Forward material pass matching the GLES deferred lighting equations.  The
// sampled albedo remains source material data; every exposure, gamma and
// environment contribution below is presentation/lighting.
layout(set = 0, binding = 0) uniform SceneUniforms
{
    mat4 cubeMvp;
    mat4 skyMvp;
    mat4 modelMvp;
    mat4 modelMatrix;
    vec4 cameraPosition;
    mat4 cameraToWorld;
    mat4 shadowMvp;
} scene;
layout(set = 0, binding = 1) uniform sampler2D baseColorTexture;
layout(set = 0, binding = 2) uniform samplerCube skyboxTexture;
layout(set = 0, binding = 3) uniform sampler2D metallicRoughnessTexture;
layout(set = 0, binding = 4) uniform sampler2D normalTexture;
layout(set = 0, binding = 5) uniform sampler2D aoTexture;
layout(set = 0, binding = 6) uniform sampler2D emissiveTexture;
layout(set = 0, binding = 8) uniform samplerCube irradianceTexture;
layout(set = 0, binding = 9) uniform samplerCube prefilteredTexture;
layout(set = 0, binding = 10) uniform sampler2D brdfLutTexture;
layout(set = 0, binding = 11) uniform sampler2D shadowMap;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vTexCoord;
layout(location = 2) in vec4 vBaseColorFactor;
layout(location = 3) in vec3 vWorldPosition;
layout(location = 4) in vec4 vMaterialParams0;
layout(location = 5) in vec4 vMaterialParams1;
layout(location = 6) in vec4 vInstanceMarker;
layout(location = 7) in flat float vDissolve;
layout(location = 8) in flat float vWaterTime;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// Enemy death dissolve, matching the GLES deferred G-buffer shader: a smooth
// value-noise field over the world position recedes with the dissolve amount,
// fragments below the threshold are gone, the band around it burns like embers.
float DissolveHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float DissolveNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(DissolveHash(i),
                DissolveHash(i + vec3(1.0, 0.0, 0.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 0.0)),
                DissolveHash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(mix(DissolveHash(i + vec3(0.0, 0.0, 1.0)),
                DissolveHash(i + vec3(1.0, 0.0, 1.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 1.0)),
                DissolveHash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

vec3 sRGBToLinear(vec3 srgb)
{
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

float ShadowVisibility(vec3 worldPosition, vec3 N, vec3 L)
{
    vec4 shadowH = scene.shadowMvp * vec4(worldPosition, 1.0);
    vec3 shadowNdc = shadowH.xyz / max(shadowH.w, 1e-5);
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float receiverDepth = shadowNdc.z;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 ||
        shadowUv.y >= 1.0 || receiverDepth <= 0.0 || receiverDepth >= 1.0) {
        return 1.0;
    }
    // Keep this in lockstep with the GLES deferred-lighting shader.
    // Original compact values: highp shadow coordinates removed the need
    // for the enlarged offsets that lifted shadows off contact points.
    float bias = max(0.008 * (1.0 - max(dot(N, L), 0.0)), 0.0015);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float shadowDepth = texture(shadowMap, shadowUv + vec2(x, y) * texel).r;
            visible += receiverDepth - bias <= shadowDepth ? 1.0 : 0.0;
        }
    }
    return visible / 9.0;
}

float D_GGX(float NoH, float perceptualRoughness)
{
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float f = NoH * NoH * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * f * f);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float perceptualRoughness)
{
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float ggxV = NoL * sqrt(NoV * NoV * (1.0 - alphaSq) + alphaSq);
    float ggxL = NoV * sqrt(NoL * NoL * (1.0 - alphaSq) + alphaSq);
    return 0.5 / max(ggxV + ggxL, 1e-4);
}

vec3 F_Schlick(vec3 f0, float VoH)
{
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}

vec3 EnvBRDF(vec3 f0, float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    vec3 fssEss = f0 * ab.x + ab.y;
    float ems = 1.0 - (ab.x + ab.y);
    vec3 fAvg = f0 + (1.0 - f0) / 21.0;
    vec3 fmsEms = ems * fssEss * fAvg / max(1.0 - fAvg * ems, 1e-4);
    return fssEss + fmsEms;
}

float SpecOcclusion(float NoV, float ao, float roughness)
{
    return clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

vec3 PointLight(vec3 lightPos, vec3 lightColor, vec3 N, vec3 V, float NoV,
    vec3 albedo, float metallic, float roughness, vec3 F0, vec3 worldPos)
{
    vec3 toLight = lightPos - worldPos;
    float distanceToLight = length(toLight);
    vec3 L = toLight / max(distanceToLight, 1e-4);
    float attenuation = 1.0 / (1.0 + distanceToLight * distanceToLight);
    vec3 H = normalize(V + L);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    vec3 F = F_Schlick(F0, VoH);
    vec3 specular = D_GGX(NoH, roughness) * V_SmithGGXCorrelated(NoV,
        max(NoL, 1e-4), roughness) * F;
    vec3 diffuse = (1.0 - F) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * lightColor * NoL * attenuation;
}

vec3 PerturbNormal(vec3 geometricNormal, vec3 worldPosition, vec2 uv, float normalScale)
{
    vec3 mapNormal = texture(normalTexture, uv).xyz * 2.0 - 1.0;
    mapNormal.xy *= normalScale;
    vec3 dp1 = dFdx(worldPosition);
    vec3 dp2 = dFdy(worldPosition);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, geometricNormal);
    vec3 dp1perp = cross(geometricNormal, dp1);
    vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float invMax = inversesqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
    mat3 tbn = mat3(tangent * invMax, bitangent * invMax, geometricNormal);
    return normalize(tbn * mapNormal);
}

// Four advected noise octaves with analytic height gradients. Geometry stays
// at the fixed sea level; only the reflection normal changes.
float WaterHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

vec2 WaterNoiseGradient(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    // Quintic interpolation gives continuous first and second derivatives
    // across lattice boundaries, without finite-difference texture samples.
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    float a = WaterHash(cell);
    float b = WaterHash(cell + vec2(1.0, 0.0));
    float c = WaterHash(cell + vec2(0.0, 1.0));
    float d = WaterHash(cell + vec2(1.0, 1.0));
    return vec2(mix(b - a, d - c, u.y) * du.x,
                mix(c - a, d - b, u.x) * du.y);
}

vec3 WaterNormal(vec3 worldPosition, float time)
{
    vec2 p = worldPosition.xz;
    float footprint = max(length(dFdx(p)), length(dFdy(p)));
    const mat2 turn = mat2(0.8, 0.6, -0.6, 0.8);
    mat2 basis = mat2(1.0);
    vec2 drift = vec2(0.12, -0.08);
    vec2 offset = vec2(17.3, -9.2);
    vec2 slope = vec2(0.0);
    float frequency = 0.45;
    float amplitude = 0.24;
    for (int octave = 0; octave < 4; ++octave) {
        // Each layer has its own scale, orientation, origin and flow direction.
        vec2 q = basis * p * frequency + drift * (time * 3.0) + offset;
        float filterWeight = 1.0 - smoothstep(0.25, 0.85, footprint * frequency);
        slope += transpose(basis) * WaterNoiseGradient(q)
            * (amplitude * frequency * filterWeight);
        basis = turn * basis;
        drift = turn * drift * 1.19;
        offset = turn * offset + vec2(23.7, 11.9);
        frequency *= 2.17;
        amplitude *= 0.43;
    }
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

// Dominant-axis mapping gives scaled cube instances a consistent brick size.
vec2 CubeWorldUv(vec3 p, vec3 n)
{
    vec3 a = abs(n);
    if (a.y >= a.x && a.y >= a.z) return vec2(p.x, -p.z * (n.y >= 0.0 ? 1.0 : -1.0));
    if (a.x >= a.z) return vec2(-p.z * (n.x >= 0.0 ? 1.0 : -1.0), p.y);
    return vec2(p.x * (n.z >= 0.0 ? 1.0 : -1.0), p.y);
}

void main()
{
    vec2 materialUv = vMaterialParams0.w > 1.5 ? CubeWorldUv(vWorldPosition, normalize(vNormal)) : vTexCoord;
    vec4 materialColor = texture(baseColorTexture, materialUv) * vBaseColorFactor;
    // The marker is a per-instance gameplay presentation tint.  It leaves
    // the source material untouched when its alpha/strength is zero.
    materialColor.rgb = mix(materialColor.rgb, vInstanceMarker.rgb,
        clamp(vInstanceMarker.a, 0.0, 1.0));
    vec3 albedo = sRGBToLinear(materialColor.rgb);
    if (vMaterialParams0.w > 0.5 && vMaterialParams0.w < 1.5) {
        vec3 N = WaterNormal(vWorldPosition, vWaterTime);
        vec3 V = normalize(scene.cameraPosition.xyz - vWorldPosition);
        if (dot(N, V) < 0.0) N = -N;
        float NoV = max(dot(N, V), 0.0);
        float fresnel = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);
        vec3 reflection = sRGBToLinear(textureLod(skyboxTexture, reflect(-V, N),
            vMaterialParams0.y * 4.0).rgb);
        // Main-engine water Fresnel F0=0.02 and sky-cube fallback, without SSR.
        outColor = vec4(mix(albedo, reflection, fresnel), 1.0);
        return;
    }
    vec4 mr = texture(metallicRoughnessTexture, materialUv);
    float metallic = clamp(mr.b * vMaterialParams0.x, 0.0, 1.0);
    float roughness = clamp(mr.g * vMaterialParams0.y, 0.045, 1.0);
    float ao = clamp(texture(aoTexture, materialUv).r * vMaterialParams1.w, 0.0, 1.0);
    vec3 N = normalize(vNormal);
    if (vMaterialParams0.z > 0.0) {
        N = PerturbNormal(N, vWorldPosition, materialUv, vMaterialParams0.z);
    }
    vec3 V = normalize(scene.cameraPosition.xyz - vWorldPosition);
    float NoV = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 L = normalize(vec3(0.45, 1.0, 0.55));
    vec3 H = normalize(V + L);
    float NoL = max(dot(N, L), 0.0);
    vec3 F = F_Schlick(F0, max(dot(V, H), 0.0));
    vec3 direct = ((1.0 - F) * (1.0 - metallic) * albedo / PI +
        D_GGX(max(dot(N, H), 0.0), roughness) *
        V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), roughness) * F) *
        vec3(1.0, 0.97, 0.92) * 3.0 * NoL *
        ShadowVisibility(vWorldPosition, N, L);

    // Only the shadow-casting sun contributes directional lighting.

    vec3 pointLights =
        PointLight(vec3(12.0, -0.64, -12.0), vec3(1.0, 0.12, 0.10) * 8.0,
            N, V, NoV, albedo, metallic, roughness, F0, vWorldPosition) +
        PointLight(vec3(0.0, -0.64, 0.0), vec3(0.10, 0.25, 1.0) * 8.0,
            N, V, NoV, albedo, metallic, roughness, F0, vWorldPosition) +
        PointLight(vec3(-5.0, -0.64, 5.0), vec3(0.10, 1.0, 0.18) * 8.0,
            N, V, NoV, albedo, metallic, roughness, F0, vWorldPosition);

    // Match GLES' full IBL contract: irradiance is a cosine-convolved cube,
    // prefilteredTexture is the six-level GGX chain, and BrdfLut.png supplies
    // the split-sum coefficients.  These resources are linear RGBA8 maps, so
    // unlike the raw sRGB skybox they must not be gamma-expanded here.
    float environmentMaxLod = max(scene.cameraPosition.w, 0.0);
    vec3 irradiance = texture(irradianceTexture, N).rgb;
    vec3 Fibl = F_Schlick(F0, NoV);
    vec3 diffuseIbl = irradiance * albedo * (1.0 - Fibl) * (1.0 - metallic);
    vec3 prefiltered = textureLod(prefilteredTexture, reflect(-V, N),
        roughness * environmentMaxLod).rgb;
    vec2 splitSum = texture(brdfLutTexture, vec2(NoV, roughness)).rg;
    vec3 fssEss = F0 * splitSum.x + vec3(splitSum.y);
    float ems = clamp(1.0 - (splitSum.x + splitSum.y), 0.0, 1.0);
    vec3 fAvg = F0 + (1.0 - F0) / 21.0;
    vec3 fmsEms = ems * fssEss * fAvg / max(1.0 - fAvg * ems, 1e-4);
    vec3 envBrdf = fssEss + fmsEms;
    vec3 specularIbl = prefiltered * SpecOcclusion(NoV, ao, roughness) * envBrdf;
    vec3 emissive = sRGBToLinear(texture(emissiveTexture, materialUv).rgb) *
        vMaterialParams1.xyz;
    vec3 color = direct + pointLights + diffuseIbl * ao + specularIbl + emissive;
    if (vDissolve > 0.0) {
        float n = DissolveNoise(vWorldPosition * 5.0);
        float cut = vDissolve * 1.18 - 0.09;
        if (n < cut) {
            discard;
        }
        float edge = 1.0 - smoothstep(0.0, 0.12, abs(n - cut));
        // Unlit additive ember so the burning edge stays bright regardless of
        // the light facing; matches the GLES ember albedo/emissive mix.
        color += vec3(1.6, 0.55, 0.12) * edge * 2.0;
    }
    outColor = vec4(max(color, vec3(0.0)), materialColor.a);
}
