#version 450
precision highp float;
precision highp int;
layout(set = 2, binding = 0) uniform sampler2D sceneTexture;
layout(set = 2, binding = 1) uniform sampler2D bloomTexture;
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vPixel;
layout(location = 2) in flat vec4 vShape;
layout(location = 3) in flat float vRadius;
layout(location = 4) in flat vec3 vScreen;
layout(location = 0) out vec4 outColor;
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

vec3 GlassScene(vec2 uv) { return ResolveLdr(uv); }
// Rounded glass lens in top-left window pixels. Background excludes all UI.
vec4 ShadeGlass(vec2 pixel, vec4 shape, float radius, vec4 tint, vec2 resolution)
{
    if (radius < 3.0 || min(shape.z, shape.w) < 16.0 || tint.a < 0.001)
        return tint; // Masks, status fills and opacity-zero controls stay unchanged.
    vec2 p = pixel - shape.xy;
    vec2 q = abs(p) - (shape.zw - vec2(radius));
    vec2 corner = max(q, vec2(0.0));
    float distance = length(corner) + min(max(q.x, q.y), 0.0) - radius;
    vec2 direction = length(corner) > 0.001 ? normalize(corner) :
        (q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    direction *= vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    // SDF bevel -> curved lens normal. Artistically exaggerated optical
    // thickness and RGB IORs make dispersion readable at mobile UI sizes.
    float bezel = clamp(min(shape.z,shape.w)*0.42, 12.0, 34.0);
    float edge = 1.0 - smoothstep(0.0, bezel, max(-distance,0.0));
    float slope = edge * 0.98;
    vec3 surfaceNormal = vec3(direction*slope, sqrt(max(1.0-slope*slope,0.001)));
    vec3 incident = vec3(0.0,0.0,-1.0);
    vec3 rayR = refract(incident, surfaceNormal, 1.0/1.42);
    vec3 rayG = refract(incident, surfaceNormal, 1.0/1.52);
    vec3 rayB = refract(incident, surfaceNormal, 1.0/1.66);
    float thickness = clamp(min(shape.z,shape.w)*0.90, 22.0, 56.0);
    vec2 magnify = -p*0.028*(1.0-edge);
    vec2 bendR = rayR.xy / max(abs(rayR.z),0.25) * thickness;
    vec2 bendG = rayG.xy / max(abs(rayG.z),0.25) * thickness;
    vec2 bendB = rayB.xy / max(abs(rayB.z),0.25) * thickness;
    vec2 uvR = (pixel+magnify+bendR)/resolution;
    vec2 uvG = (pixel+magnify+bendG)/resolution;
    vec2 uvB = (pixel+magnify+bendB)/resolution;
    vec3 spectral = vec3(GlassScene(uvR).r, GlassScene(uvG).g, GlassScene(uvB).b);
    // Clear lip preserves colour fringes; frost is concentrated in the core.
    vec2 spread = vec2(mix(clamp(min(shape.z,shape.w)*0.065,2.0,5.0),0.45,edge))/resolution;
    vec3 frost = (GlassScene(uvG+spread)+GlassScene(uvG-spread)+
        GlassScene(uvG+vec2(spread.x,-spread.y))+GlassScene(uvG+vec2(-spread.x,spread.y)))*0.25;
    vec3 blurred = mix(spectral,frost,0.48*(1.0-edge));
    float lens = edge;
    float saturation = max(tint.r,max(tint.g,tint.b))-min(tint.r,min(tint.g,tint.b));
    vec3 color = mix(blurred, tint.rgb, 0.055 + 0.16*saturation);
    color = mix(color, vec3(0.86,0.92,1.0), 0.035);
    // White specular lip stays legible against both sea and bright sky.
    // Pixel widths use derivative AA, so the fine edge does not shimmer.
    float aa = max(fwidth(distance), 0.6);
    float rim = exp(-abs(distance + 1.15) / max(1.1, aa));
    float light = max(dot(direction, normalize(vec2(-0.55,-0.83))),0.0);
    float counterLight = max(dot(direction, normalize(vec2(0.70,0.71))),0.0);
    float shoulder = exp(-abs(distance + 3.8) / 3.2);
    float innerRim = exp(-abs(distance + bezel*0.25)/2.2);
    float specular = rim * (0.46 + 0.40*light + 0.18*counterLight);
    color = mix(color, vec3(1.0), clamp(specular, 0.0, 0.94));
    color += vec3(0.80,0.89,1.0) * shoulder * (0.06 + 0.16*light);
    color += vec3(0.36,0.48,0.64) * innerRim * counterLight * 0.32;
    color += vec3(0.10,0.13,0.17) * lens * (0.35 + 0.65*light);
    float coverage = 1.0 - smoothstep(-max(fwidth(distance),0.6),0.0,distance);
    return vec4(clamp(color,0.0,1.0), tint.a * coverage);
}
void main() { outColor = ShadeGlass(vPixel, vShape, vRadius, vColor, vScreen.xy); }
