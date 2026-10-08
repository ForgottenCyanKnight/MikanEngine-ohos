#version 450

layout(set = 1, binding = 0) uniform sampler2D textAtlas;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vScreenPxRange;

layout(location = 0) out vec4 outColor;

float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

void main()
{
    vec4 sample4 = texture(textAtlas, vTexCoord);
    float sd = median(sample4.r, sample4.g, sample4.b);
    float effectiveRange = max(vScreenPxRange, 1.0);
    float preciseAlpha = clamp(effectiveRange * (sd - 0.5) + 0.5, 0.0, 1.0);
    float width = fwidth(sd);
    float fallback = smoothstep(0.5 - width, 0.5 + width, sd);
    float blend = smoothstep(0.5, 1.5, vScreenPxRange);
    float alpha = mix(fallback, preciseAlpha, blend);
    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
