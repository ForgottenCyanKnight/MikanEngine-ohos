#version 450

// Full-screen triangle for the Vulkan post-processing resolve.  The positive
// height viewport uses NDC y=-1 at the top, so the interpolated texture
// coordinate keeps the render-target's top-left convention without another
// vertical flip.
layout(location = 0) out vec2 vTexCoord;

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
    vTexCoord = position * 0.5 + 0.5;
}
