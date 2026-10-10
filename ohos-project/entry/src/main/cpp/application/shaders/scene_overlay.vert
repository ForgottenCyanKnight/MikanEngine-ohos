#version 450
precision highp float;
precision highp int;

// Window-pixel overlay geometry.  The positive-height Vulkan viewport used by
// the OHOS swapchain has the same top-left UI convention as the GLES backend,
// so the RHI can feed both paths the same coordinates.
layout(push_constant) uniform UiPushConstants
{
    vec2 resolution;
    float unusedPxRange;
    float unusedPadding;
} ui;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 2) in vec4 inShape;
layout(location = 3) in float inRadius;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vPixel;
layout(location = 2) out flat vec4 vShape;
layout(location = 3) out flat float vRadius;
layout(location = 4) out flat vec3 vScreen;

void main()
{
    // Vulkan's positive-height viewport maps NDC y=-1 to the top edge on this
    // OHOS swapchain.  UI geometry is authored in the same top-left pixel
    // space as GLES, so keep y increasing downwards here.
    vec2 clip = vec2(inPosition.x / ui.resolution.x * 2.0 - 1.0,
                     inPosition.y / ui.resolution.y * 2.0 - 1.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    vColor = inColor;
    vPixel = inPosition;
    vShape = inShape;
    vRadius = inRadius;
    vScreen = vec3(ui.resolution, ui.unusedPxRange);
}
