#version 450

// The cube is transformed on the GPU: the model-view-projection matrix is
// uploaded once per frame through the UBO, and the vertex buffer holds
// static model-space positions (w = 1).  This keeps per-frame host work to
// a single small UBO upload, which matters a lot on the emulator where every
// map/unmap round-trips through the Vulkan proxy process.
layout(set = 0, binding = 0) uniform SceneUniforms
{
    mat4 cubeMvp;
    mat4 skyMvp;
    mat4 shadowMvp;
} scene;

layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vTexCoord;

void main()
{
    gl_Position = scene.cubeMvp * inPosition;
    vColor = inColor;
    vTexCoord = inTexCoord;
}
