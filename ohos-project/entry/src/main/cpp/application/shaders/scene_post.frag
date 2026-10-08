#version 450

// Final GLES-equivalent resolve: the Vulkan bloom chain renders the six
// downsample and five upsample targets before this pass, then this pass
// combines the linear scene and the half-resolution bloom result, applies
// AgX/sRGB and performs the final LDR neighbourhood resolve.
layout(set = 2, binding = 0) uniform sampler2D sceneTexture;
layout(set = 2, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform PostProcessPushConstants
{
    // x = AgX exposure, y = bloom strength, z/w = full-resolution texel size.
    vec4 params;
} post;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

vec3 SampleScene(vec2 uv)
{
    return texture(sceneTexture, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

vec3 SampleBloom(vec2 uv)
{
    return texture(bloomTexture, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
}

float Luma(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 Sanitize(vec3 color)
{
    bvec3 nan = isnan(color);
    bvec3 inf = isinf(color);
    return vec3((nan.x || inf.x) ? 0.0 : color.x,
        (nan.y || inf.y) ? 0.0 : color.y,
        (nan.z || inf.z) ? 0.0 : color.z);
}

vec3 AgxContrastApprox(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 0.021 * x + 4.0111 * x2 - 25.682 * x2 * x + 70.359 * x4 -
        74.778 * x4 * x + 27.069 * x4 * x2;
}

vec3 Agx(vec3 color)
{
    color = max(Sanitize(color) * post.params.x, vec3(2e-10));
    const mat3 inset = mat3(
        0.5449081368, 0.1404400588, 0.0888274119,
        0.3737794596, 0.7541095986, 0.1788771247,
        0.0813849767, 0.1054335857, 0.7322499996);
    const mat3 outset = mat3(
        1.9645509603, -0.2993224339, -0.1643683381,
        -0.8558584512, 1.3264510742, -0.2382246407,
        -0.1088671083, -0.0270840210, 1.4026653471);
    const float minEv = -12.4739311883;
    const float maxEv = 4.0260688117;
    color = inset * color;
    color = clamp(log2(max(color, vec3(2e-10))), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = AgxContrastApprox(color);
    color = pow(max(color, vec3(0.0)), vec3(2.4));
    return max(outset * color, vec3(0.0));
}

vec3 LinearToSrgb(vec3 color)
{
    color = clamp(color, vec3(0.0), vec3(1.0));
    const vec3 a = vec3(0.055);
    return mix((vec3(1.0) + a) * pow(color, vec3(1.0 / 2.4)) - a,
        12.92 * color, lessThan(color, vec3(0.0031308)));
}

vec3 ResolveLdr(vec2 uv)
{
    vec3 linear = SampleScene(uv) + SampleBloom(uv) * post.params.y;
    return LinearToSrgb(Agx(linear));
}

float Bayer2(vec2 a)
{
    a = floor(a);
    return fract(a.x / 2.0 + a.y * a.y * 0.75);
}

float Bayer4(vec2 a)
{
    return Bayer2(0.5 * a) * 0.25 + Bayer2(a);
}

float Bayer8(vec2 a)
{
    return Bayer4(0.5 * a) * 0.25 + Bayer2(a);
}

void main()
{
    vec2 texel = post.params.zw;
    vec3 color = ResolveLdr(vTexCoord);
    vec3 left = ResolveLdr(vTexCoord - vec2(texel.x, 0.0));
    vec3 right = ResolveLdr(vTexCoord + vec2(texel.x, 0.0));
    vec3 up = ResolveLdr(vTexCoord - vec2(0.0, texel.y));
    vec3 down = ResolveLdr(vTexCoord + vec2(0.0, texel.y));
    float lumaMin = min(Luma(color), min(min(Luma(left), Luma(right)),
        min(Luma(up), Luma(down))));
    float lumaMax = max(Luma(color), max(max(Luma(left), Luma(right)),
        max(Luma(up), Luma(down))));
    float edgeBlend = smoothstep(0.08, 0.30, lumaMax - lumaMin) * 0.12;
    color = mix(color, (left + right + up + down) * 0.25, edgeBlend);
    color += (Bayer8(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);
    outColor = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
