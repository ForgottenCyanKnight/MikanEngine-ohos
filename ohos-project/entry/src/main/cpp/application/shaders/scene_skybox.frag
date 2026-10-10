#version 450
precision highp float;
precision highp int;

layout(set = 0, binding = 2) uniform samplerCube skyboxTexture;

layout(location = 0) in vec3 vDirection;
layout(location = 0) out vec4 outColor;

void main()
{
    // Skybox assets are authored as sRGB JPEGs.  The Vulkan scene target is
    // linear HDR, matching the GLES lighting pass before tonemap/bloom.
    outColor = vec4(pow(texture(skyboxTexture, normalize(vDirection)).rgb,
        vec3(2.2)), 1.0);
}
