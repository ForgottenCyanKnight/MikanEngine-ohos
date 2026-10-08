#version 450

// Depth-only scene shadow pass.  It uses the same skin UBO and object push
// constants as the forward model pass so animated and static meshes cast from
// the exact geometry that the color pass displays.
layout(push_constant) uniform ModelPushConstants
{
    mat4 mvp;
    mat4 model;
    // Same enemy dissolve amount as the color pass; gated on the skin index.
    float dissolve;
    float pad[3];
} object;

layout(set = 0, binding = 7) uniform SkinUniforms
{
    mat4 jointMats[256];
} playerSkinBlock;

layout(set = 0, binding = 12) uniform EnemySkinUniforms
{
    mat4 jointMats[256];
} enemySkinBlock;

layout(location = 0) in vec3 inPosition;
layout(location = 6) in uvec4 inJoints;
layout(location = 7) in vec4 inWeights;
layout(location = 8) in vec4 inInstanceModel0;
layout(location = 9) in vec4 inInstanceModel1;
layout(location = 10) in vec4 inInstanceModel2;
layout(location = 11) in vec4 inInstanceModel3;
layout(location = 13) in uint inSkinIndex;

layout(location = 0) out vec3 vShadowWorldPos;
layout(location = 1) out flat float vDissolve;

void main()
{
    mat4 instanceModel = mat4(inInstanceModel0, inInstanceModel1,
        inInstanceModel2, inInstanceModel3);
    vec3 skinnedPosition = inPosition;
    if (dot(inWeights, vec4(1.0)) > 0.001) {
        ivec4 joint = clamp(ivec4(inJoints), ivec4(0), ivec4(255));
        mat4 skinMatrix = inSkinIndex == 0u
            ? inWeights.x * playerSkinBlock.jointMats[joint.x]
                + inWeights.y * playerSkinBlock.jointMats[joint.y]
                + inWeights.z * playerSkinBlock.jointMats[joint.z]
                + inWeights.w * playerSkinBlock.jointMats[joint.w]
            : inWeights.x * enemySkinBlock.jointMats[joint.x]
                + inWeights.y * enemySkinBlock.jointMats[joint.y]
                + inWeights.z * enemySkinBlock.jointMats[joint.z]
                + inWeights.w * enemySkinBlock.jointMats[joint.w];
        skinnedPosition = (skinMatrix * vec4(inPosition, 1.0)).xyz;
    }
    vec4 shadowWorldPosition = instanceModel * vec4(skinnedPosition, 1.0);
    gl_Position = object.mvp * shadowWorldPosition;
    vShadowWorldPos = shadowWorldPosition.xyz;
    vDissolve = inSkinIndex == 1u ? object.dissolve : 0.0;
}
