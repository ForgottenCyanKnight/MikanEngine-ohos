#version 450
precision highp float;
precision highp int;

layout(push_constant) uniform UiPushConstants
{
    vec2 resolution;
    float unusedPxRange;
    float unusedPadding;
} ui;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inScreenPxRange;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vScreenPxRange;
layout(location = 3) out vec2 vPixel;
layout(location = 4) out flat vec3 vScreen;

void main()
{
    // Match GLES' top-left pixel coordinates on the positive-height Vulkan
    // viewport used by the OHOS swapchain.
    vec2 clip = vec2(inPosition.x / ui.resolution.x * 2.0 - 1.0,
                     inPosition.y / ui.resolution.y * 2.0 - 1.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    vTexCoord = inTexCoord;
    vColor = inColor;
    vScreenPxRange = inScreenPxRange;
    vPixel = inPosition;
    vScreen = vec3(ui.resolution,ui.unusedPxRange);
}
