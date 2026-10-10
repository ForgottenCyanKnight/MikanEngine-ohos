#version 450
precision highp float;
precision highp int;

layout(set = 2, binding = 0) uniform sampler2D inputTexture;

layout(push_constant) uniform PostProcessPushConstants
{
    // x = threshold, y = soft-knee ratio.  z/w are unused by this pass.
    vec4 params;
} post;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

float SoftKneeContribution(float brightness)
{
    float threshold = post.params.x;
    float softKnee = post.params.y * threshold;
    float soft = brightness - threshold + softKnee;
    soft = clamp(soft, 0.0, 2.0 * softKnee);
    soft = soft * soft / (4.0 * softKnee + 1e-4);
    return max(soft, brightness - threshold) / max(brightness, 1e-4);
}

vec3 Threshold(vec3 color)
{
    return color * SoftKneeContribution(dot(color, vec3(0.2126, 0.7152, 0.0722)));
}

void main()
{
    vec2 px = 1.0 / vec2(textureSize(inputTexture, 0));
    vec3 sum = Threshold(texture(inputTexture, vTexCoord).rgb);
    sum += Threshold(texture(inputTexture, vTexCoord + vec2(-px.x,  px.y)).rgb);
    sum += Threshold(texture(inputTexture, vTexCoord + vec2( px.x,  px.y)).rgb);
    sum += Threshold(texture(inputTexture, vTexCoord + vec2(-px.x, -px.y)).rgb);
    sum += Threshold(texture(inputTexture, vTexCoord + vec2( px.x, -px.y)).rgb);
    outColor = vec4(sum * 0.125, 1.0);
}
