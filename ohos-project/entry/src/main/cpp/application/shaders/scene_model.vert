#version 450
precision highp float;
precision highp int;

// The push range remains part of the shared pipeline layout for UI/shadow
// compatibility. Model transforms now come from the Mikan-style instance
// stream below so characters can share one draw per material.
layout(push_constant) uniform ModelPushConstants
{
    mat4 mvp;
    mat4 model;
    // Enemy death dissolve amount (0 = intact).  Gated on the skin index so
    // only instance 1 of the character batch can ever burn away.
    float dissolve;
    float pad[3];
} object;

layout(set = 0, binding = 0) uniform SceneUniforms
{
    mat4 cubeMvp;
    mat4 skyMvp;
    mat4 modelMvp;
    mat4 modelMatrix;
    vec4 cameraPosition;
    mat4 cameraToWorld;
    mat4 shadowMvp;
    mat4 viewProj;
} scene;

// Same column-major LBS contract as GLES: joint = normalise * global * IBM.
// Two palettes keep player and enemy animation independent while the mesh and
// material descriptor remain shared by the instance batch.
layout(set = 0, binding = 7) uniform SkinUniforms
{
    mat4 jointMats[256];
} playerSkinBlock;

layout(set = 0, binding = 12) uniform EnemySkinUniforms
{
    mat4 jointMats[256];
} enemySkinBlock;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inBaseColorFactor;
layout(location = 4) in vec4 inMaterialParams0;
layout(location = 5) in vec4 inMaterialParams1;
layout(location = 6) in uvec4 inJoints;
layout(location = 7) in vec4 inWeights;
layout(location = 8) in vec4 inInstanceModel0;
layout(location = 9) in vec4 inInstanceModel1;
layout(location = 10) in vec4 inInstanceModel2;
layout(location = 11) in vec4 inInstanceModel3;
layout(location = 12) in vec4 inInstanceColor;
layout(location = 13) in uint inSkinIndex;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec4 vBaseColorFactor;
layout(location = 3) out vec3 vWorldPosition;
layout(location = 4) out vec4 vMaterialParams0;
layout(location = 5) out vec4 vMaterialParams1;
layout(location = 6) out vec4 vInstanceMarker;
layout(location = 7) out flat float vDissolve;
layout(location = 8) out flat float vWaterTime;

void main()
{
    mat4 instanceModel = mat4(inInstanceModel0, inInstanceModel1,
        inInstanceModel2, inInstanceModel3);
    vec3 skinnedPosition = inPosition;
    vec3 skinnedNormal = inNormal;
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
        skinnedNormal = mat3(skinMatrix) * inNormal;
    }
    vec4 worldPosition = instanceModel * vec4(skinnedPosition, 1.0);
    gl_Position = scene.viewProj * worldPosition;
    vWaterTime = object.pad[0];
    vNormal = normalize(mat3(instanceModel) * skinnedNormal);
    vTexCoord = inTexCoord;
    vBaseColorFactor = inBaseColorFactor;
    vInstanceMarker = inInstanceColor;
    vWorldPosition = worldPosition.xyz;
    vMaterialParams0 = inMaterialParams0;
    vMaterialParams1 = inMaterialParams1;
    vDissolve = inSkinIndex == 1u ? object.dissolve : 0.0;
}
