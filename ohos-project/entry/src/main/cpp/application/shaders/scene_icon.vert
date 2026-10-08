#version 450

// Window-pixel icon geometry.  Keep the same top-left coordinate convention
// as scene_overlay.vert so the GLES and Vulkan touch layouts are identical.
layout(push_constant) uniform UiPushConstants
{
    vec2 resolution;
    float unusedPxRange;
    float unusedPadding;
} ui;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main()
{
    vec2 clip = vec2(inPosition.x / ui.resolution.x * 2.0 - 1.0,
                     inPosition.y / ui.resolution.y * 2.0 - 1.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    vUv = inUv;
    vColor = inColor;
}
