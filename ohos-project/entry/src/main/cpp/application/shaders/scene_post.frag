#version 450
precision highp float;
precision highp int;

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
    int fxaaEnabled;
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

vec3 ResolveLdr(vec2 uv)
{
    vec3 linear = SampleScene(uv) + SampleBloom(uv) * post.params.y;
    vec3 color = LinearToSrgb(Agx(linear));

    // GLES writes its tonemapped target with Bayer dither before the FXAA
    // pass. Use the sampled scene pixel coordinate here so FXAA sees dither
    // in its input rather than adding it after the edge resolve.
    vec2 sourceSize = vec2(textureSize(sceneTexture, 0));
    vec2 sourcePixel = min(floor(clamp(uv, vec2(0.0), vec2(1.0)) * sourceSize),
        sourceSize - vec2(1.0));
    color += (Bayer8(sourcePixel) - 0.5) * (1.0 / 255.0);
    return clamp(color, vec3(0.0), vec3(1.0));
}

float FxaaLuma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float fxaaQuality[12] = float[12](
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float FxaaLumaAt(vec2 uv)
{
    return FxaaLuma(ResolveLdr(uv));
}

vec3 FxaaColorAt(vec2 uv)
{
    return ResolveLdr(uv);
}

// FXAA 3.11 quality profile, kept in sync with the GLES implementation.
// Input is the final AgX/sRGB LDR resolve and view is the source texel size.
vec3 FXAA311(vec3 color, vec2 texCoord, vec2 view)
{
    const float edgeThresholdMin = 0.03125;
    const float edgeThresholdMax = 0.125;
    const float subpixelQuality = 0.75;
    const int iterations = 12;

    float lumaCenter = FxaaLumaAt(texCoord);
    float lumaDown = FxaaLumaAt(texCoord + vec2(0.0, -1.0) * view);
    float lumaUp = FxaaLumaAt(texCoord + vec2(0.0, 1.0) * view);
    float lumaLeft = FxaaLumaAt(texCoord + vec2(-1.0, 0.0) * view);
    float lumaRight = FxaaLumaAt(texCoord + vec2(1.0, 0.0) * view);

    float lumaMin = min(lumaCenter,
        min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter,
        max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange > max(edgeThresholdMin, lumaMax * edgeThresholdMax)) {
        float lumaDownLeft = FxaaLumaAt(texCoord + vec2(-1.0, -1.0) * view);
        float lumaUpRight = FxaaLumaAt(texCoord + vec2(1.0, 1.0) * view);
        float lumaUpLeft = FxaaLumaAt(texCoord + vec2(-1.0, 1.0) * view);
        float lumaDownRight = FxaaLumaAt(texCoord + vec2(1.0, -1.0) * view);

        float lumaDownUp = lumaDown + lumaUp;
        float lumaLeftRight = lumaLeft + lumaRight;
        float lumaLeftCorners = lumaDownLeft + lumaUpLeft;
        float lumaDownCorners = lumaDownLeft + lumaDownRight;
        float lumaRightCorners = lumaDownRight + lumaUpRight;
        float lumaUpCorners = lumaUpRight + lumaUpLeft;

        float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) +
            abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +
            abs(-2.0 * lumaRight + lumaRightCorners);
        float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) +
            abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +
            abs(-2.0 * lumaDown + lumaDownCorners);

        bool isHorizontal = edgeHorizontal >= edgeVertical;
        float luma1 = isHorizontal ? lumaDown : lumaLeft;
        float luma2 = isHorizontal ? lumaUp : lumaRight;
        float gradient1 = luma1 - lumaCenter;
        float gradient2 = luma2 - lumaCenter;
        bool is1Steepest = abs(gradient1) >= abs(gradient2);
        float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

        float stepLength = isHorizontal ? view.y : view.x;
        float lumaLocalAverage;
        if (is1Steepest) {
            stepLength = -stepLength;
            lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
        } else {
            lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
        }

        vec2 currentUv = texCoord;
        if (isHorizontal) {
            currentUv.y += stepLength * 0.5;
        } else {
            currentUv.x += stepLength * 0.5;
        }

        vec2 offset = isHorizontal ? vec2(view.x, 0.0) : vec2(0.0, view.y);
        vec2 uv1 = currentUv - offset;
        vec2 uv2 = currentUv + offset;
        float lumaEnd1 = FxaaLumaAt(uv1) - lumaLocalAverage;
        float lumaEnd2 = FxaaLumaAt(uv2) - lumaLocalAverage;
        bool reached1 = abs(lumaEnd1) >= gradientScaled;
        bool reached2 = abs(lumaEnd2) >= gradientScaled;
        bool reachedBoth = reached1 && reached2;

        if (!reached1) {
            uv1 -= offset;
        }
        if (!reached2) {
            uv2 += offset;
        }

        if (!reachedBoth) {
            for (int i = 2; i < iterations; ++i) {
                if (!reached1) {
                    lumaEnd1 = FxaaLumaAt(uv1) - lumaLocalAverage;
                }
                if (!reached2) {
                    lumaEnd2 = FxaaLumaAt(uv2) - lumaLocalAverage;
                }

                reached1 = abs(lumaEnd1) >= gradientScaled;
                reached2 = abs(lumaEnd2) >= gradientScaled;
                reachedBoth = reached1 && reached2;
                if (!reached1) {
                    uv1 -= offset * fxaaQuality[i];
                }
                if (!reached2) {
                    uv2 += offset * fxaaQuality[i];
                }
                if (reachedBoth) {
                    break;
                }
            }
        }

        float distance1 = isHorizontal ? texCoord.x - uv1.x : texCoord.y - uv1.y;
        float distance2 = isHorizontal ? uv2.x - texCoord.x : uv2.y - texCoord.y;
        bool isDirection1 = distance1 < distance2;
        float distanceFinal = min(distance1, distance2);
        float edgeThickness = distance1 + distance2;
        float pixelOffset = -distanceFinal / edgeThickness + 0.5;
        bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
        bool correctVariation =
            ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
        float finalOffset = correctVariation ? pixelOffset : 0.0;

        float lumaAverage = (1.0 / 12.0) *
            (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
        float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
        float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) *
            subPixelOffset1 * subPixelOffset1;
        float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * subpixelQuality;
        finalOffset = max(finalOffset, subPixelOffsetFinal);

        vec2 finalUv = texCoord;
        if (isHorizontal) {
            finalUv.y += finalOffset * stepLength;
        } else {
            finalUv.x += finalOffset * stepLength;
        }
        color = FxaaColorAt(finalUv);
    }

    return color;
}

void main()
{
    vec3 color = ResolveLdr(vTexCoord);
    if (post.fxaaEnabled != 0) {
        color = FXAA311(color, vTexCoord, post.params.zw);
    }
    outColor = vec4(clamp(color * 0.98, vec3(0.0), vec3(1.0)), 1.0);
}
