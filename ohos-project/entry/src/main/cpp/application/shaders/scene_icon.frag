#version 450
precision highp float;
precision highp int;

layout(set = 1, binding = 1) uniform sampler2D iconTexture;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    float alpha = texture(iconTexture, vUv).a;
    outColor = vec4(vColor.rgb, vColor.a * alpha);
}
