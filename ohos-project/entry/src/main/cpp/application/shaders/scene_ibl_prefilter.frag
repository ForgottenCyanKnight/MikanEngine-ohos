#version 450

layout(set = 0, binding = 0) uniform samplerCube skyboxTexture;

layout(push_constant) uniform IblPushConstants
{
    int cubeFace;
    float roughness;
} ibl;

layout(location = 0) in vec3 vDirection;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), RadicalInverseVdC(i));
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float rough)
{
    float alpha = rough * rough;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    vec3 up = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

void main()
{
    vec3 N = normalize(vDirection);
    vec3 V = N;
    const uint kSamples = 64u;
    vec3 color = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = Hammersley(i, kSamples);
        vec3 H = ImportanceSampleGGX(xi, N, ibl.roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NoL = max(dot(N, L), 0.0);
        if (NoL > 0.0) {
            color += pow(textureLod(skyboxTexture, L, 0.0).rgb, vec3(2.2)) * NoL;
            weight += NoL;
        }
    }
    outColor = vec4(color / max(weight, 1e-4), 1.0);
}
