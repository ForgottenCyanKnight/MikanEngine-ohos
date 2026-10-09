#version 450

layout(set = 1, binding = 0) uniform sampler2D textAtlas;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vScreenPxRange;
layout(location = 3) in vec2 vPixel;
layout(location = 4) in flat vec3 vScreen;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneTexture;
layout(set = 2, binding = 1) uniform sampler2D bloomTexture;
vec3 SampleScene(vec2 uv) { return texture(sceneTexture,clamp(uv,vec2(0.0),vec2(1.0))).rgb; }
vec3 SampleBloom(vec2 uv) { return texture(bloomTexture,clamp(uv,vec2(0.0),vec2(1.0))).rgb; }
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
    color = max(Sanitize(color) * 1.3, vec3(2e-10));
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
    vec3 linear = SampleScene(uv) + SampleBloom(uv) * vScreen.z;
    return LinearToSrgb(Agx(linear));
}

vec3 TitleScene(vec2 uv) { return ResolveLdr(uv); }
// The SDF gradient supplies the curved glyph edge normal in window pixels.
vec3 GlassTitle(vec2 pixel, vec2 gradient, float insidePx, vec2 resolution)
{
    vec2 direction = gradient / max(length(gradient),0.00001);
    float bevel = 1.0 - smoothstep(0.0,3.2,max(insidePx,0.0));
    float slope = bevel*0.94;
    vec3 normal = vec3(-direction*slope,sqrt(max(1.0-slope*slope,0.001)));
    vec3 rayR = refract(vec3(0.0,0.0,-1.0),normal,1.0/1.42);
    vec3 rayG = refract(vec3(0.0,0.0,-1.0),normal,1.0/1.52);
    vec3 rayB = refract(vec3(0.0,0.0,-1.0),normal,1.0/1.66);
    vec2 offsetR = rayR.xy/max(abs(rayR.z),0.25)*11.0;
    vec2 offsetG = rayG.xy/max(abs(rayG.z),0.25)*11.0;
    vec2 offsetB = rayB.xy/max(abs(rayB.z),0.25)*11.0;
    vec3 through = vec3(TitleScene((pixel+offsetR)/resolution).r,
        TitleScene((pixel+offsetG)/resolution).g,TitleScene((pixel+offsetB)/resolution).b);
    float rim = exp(-abs(insidePx)/1.15);
    float highlight = 0.25 + 0.75*max(dot(-direction,normalize(vec2(-0.55,-0.83))),0.0);
    vec3 color = mix(through,vec3(0.78,0.87,0.98),0.38);
    color += vec3(0.65,0.76,0.90)*rim*highlight*0.70;
    return clamp(color,0.0,1.0);
}
float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

void main()
{
    vec4 sample4 = texture(textAtlas, vTexCoord);
    float sd = median(sample4.r, sample4.g, sample4.b);
    float effectiveRange = max(abs(vScreenPxRange), 1.0);
    float preciseAlpha = clamp(effectiveRange * (sd - 0.5) + 0.5, 0.0, 1.0);
    float width = fwidth(sd);
    float fallback = smoothstep(0.5 - width, 0.5 + width, sd);
    float blend = smoothstep(0.5, 1.5, abs(vScreenPxRange));
    float alpha = mix(fallback, preciseAlpha, blend);
    vec3 color = vColor.rgb;
    if (vScreenPxRange < 0.0) color = GlassTitle(vPixel,vec2(dFdx(sd),dFdy(sd)),
        abs(vScreenPxRange)*(sd-0.5),vScreen.xy);
    outColor = vec4(color, vColor.a * alpha);
}
