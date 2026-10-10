#version 450
precision highp float;
precision highp int;

layout(set = 2, binding = 0) uniform sampler2D inputTexture;

layout(push_constant) uniform PostProcessPushConstants
{
    vec4 params;
} post;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 px = 1.0 / vec2(textureSize(inputTexture, 0));
    vec3 sum = texture(inputTexture, vTexCoord).rgb * 4.0;
    sum += texture(inputTexture, vTexCoord + vec2(-px.x,  px.y)).rgb;
    sum += texture(inputTexture, vTexCoord + vec2( px.x,  px.y)).rgb;
    sum += texture(inputTexture, vTexCoord + vec2(-px.x, -px.y)).rgb;
    sum += texture(inputTexture, vTexCoord + vec2( px.x, -px.y)).rgb;
    outColor = vec4(sum * 0.125, 1.0);
}
