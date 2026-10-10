#version 450
precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform SceneUniforms
{
    mat4 cubeMvp;
    mat4 skyMvp;
    mat4 modelMvp;
    mat4 modelMatrix;
    vec4 cameraPosition;
    mat4 cameraToWorld;
    mat4 shadowMvp;
} scene;

layout(location = 0) out vec3 vDirection;

void main()
{
    vec2 position;
    if (gl_VertexIndex == 0) {
        position = vec2(-1.0, -1.0);
    } else if (gl_VertexIndex == 1) {
        position = vec2(3.0, -1.0);
    } else {
        position = vec2(-1.0, 3.0);
    }
    // The skybox is rendered as a full-screen triangle.  Reconstruct the same
    // camera-space ray as GLES from the shared projection, then rotate it with
    // the shared orbit-camera basis so background and reflections track yaw /
    // pitch identically on both paths.
    gl_Position = vec4(position, 0.0, 1.0);
    vec3 cameraDirection = normalize(vec3(
        position.x / scene.skyMvp[0][0],
        position.y / scene.skyMvp[1][1],
        -1.0));
    vDirection = mat3(scene.cameraToWorld) * cameraDirection;
}
