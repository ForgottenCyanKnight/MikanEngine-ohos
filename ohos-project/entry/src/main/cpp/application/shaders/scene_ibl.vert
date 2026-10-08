#version 450

layout(push_constant) uniform IblPushConstants
{
    int cubeFace;
    float roughness;
} ibl;

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
    gl_Position = vec4(position, 0.0, 1.0);
    float u = position.x;
    float v = position.y;
    if (ibl.cubeFace == 0) {
        vDirection = vec3(1.0, -v, -u);
    } else if (ibl.cubeFace == 1) {
        vDirection = vec3(-1.0, -v, u);
    } else if (ibl.cubeFace == 2) {
        vDirection = vec3(u, 1.0, v);
    } else if (ibl.cubeFace == 3) {
        vDirection = vec3(u, -1.0, -v);
    } else if (ibl.cubeFace == 4) {
        vDirection = vec3(u, -v, 1.0);
    } else {
        vDirection = vec3(-u, -v, -1.0);
    }
}
