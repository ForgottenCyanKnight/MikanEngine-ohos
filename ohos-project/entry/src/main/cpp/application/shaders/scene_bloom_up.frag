#version 450

layout(set = 2, binding = 0) uniform sampler2D inputTexture;
layout(set = 2, binding = 1) uniform sampler2D previousTexture;

layout(push_constant) uniform PostProcessPushConstants
{
    vec4 params;
} post;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

vec3 DualKernelInput(vec2 uv)
{
    vec2 px = 1.0 / vec2(textureSize(inputTexture, 0));
    vec3 sum = vec3(0.0);
    sum += texture(inputTexture, uv + vec2(-px.x,  px.y)).rgb * 2.0;
    sum += texture(inputTexture, uv + vec2( px.x,  px.y)).rgb * 2.0;
    sum += texture(inputTexture, uv + vec2(-px.x, -px.y)).rgb * 2.0;
    sum += texture(inputTexture, uv + vec2( px.x, -px.y)).rgb * 2.0;
    sum += texture(inputTexture, uv + vec2(0.0,  px.y * 2.0)).rgb;
    sum += texture(inputTexture, uv + vec2(px.x * 2.0, 0.0)).rgb;
    sum += texture(inputTexture, uv + vec2(0.0, -px.y * 2.0)).rgb;
    sum += texture(inputTexture, uv + vec2(-px.x * 2.0, 0.0)).rgb;
    return sum * 0.0833;
}

vec3 DualKernelPrevious(vec2 uv)
{
    vec2 px = 1.0 / vec2(textureSize(previousTexture, 0));
    vec3 sum = vec3(0.0);
    sum += texture(previousTexture, uv + vec2(-px.x,  px.y)).rgb * 2.0;
    sum += texture(previousTexture, uv + vec2( px.x,  px.y)).rgb * 2.0;
    sum += texture(previousTexture, uv + vec2(-px.x, -px.y)).rgb * 2.0;
    sum += texture(previousTexture, uv + vec2( px.x, -px.y)).rgb * 2.0;
    sum += texture(previousTexture, uv + vec2(0.0,  px.y * 2.0)).rgb;
    sum += texture(previousTexture, uv + vec2(px.x * 2.0, 0.0)).rgb;
    sum += texture(previousTexture, uv + vec2(0.0, -px.y * 2.0)).rgb;
    sum += texture(previousTexture, uv + vec2(-px.x * 2.0, 0.0)).rgb;
    return sum * 0.0833;
}

void main()
{
    outColor = vec4(DualKernelInput(vTexCoord) + DualKernelPrevious(vTexCoord), 1.0);
}
