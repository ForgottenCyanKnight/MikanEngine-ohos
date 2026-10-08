// GLES 3.0 backend.
//
// This renders the same scene as the Vulkan backend (cubemap skybox + the glTF
// model) over the OHOS DGLES transport, which is a different bridge from the
// express GPU one that wedges on the x86_64 emulator.  See rhi/rhi.h for why
// the backend has to be selectable at all.
//
// Deliberate choices:
//   * The CPU-side scene data is shared with the Vulkan path: model_loader is
//     API-agnostic and the skybox is the same six rawfile JPEGs.  Only the GPU
//     residency code differs, which is exactly the split an RHI is for.
//   * Uniforms go through a std140 UBO with the same four-mat4 layout as the
//     Vulkan path, so the two backends produce identical matrices from
//     identical bytes.
//   * The projection matrix is *not* the same: GL clip space is z in [-1, 1]
//     while Vulkan's is [0, 1] (and Vulkan's framebuffer y grows downward).
//     Each backend therefore builds its own projection; everything above it
//     (view, rotation, scaling) is shared.
//   * Every GL call is local: unlike the emulator's Vulkan proxy there is no
//     per-call round trip to the host, so the frame path can afford to be
//     straightforward instead of pre-recorded.

// Same directory as this file; the include path does not contain application/,
// only the cpp root, so the sibling header is addressed relatively.
#include "rhi.h"
#include "../audio/audio_manager.h"
#include "../inventory.h"
#include "../physics/jolt_gameplay_physics.h"
#include "../scene/scene_definition.h"

#include "sdf_font_metrics.h"

#include <cstdio>

// The loader sits one level up (application/); the shared CPU-side scene types
// deliberately stay there rather than being duplicated per backend.
#include "../model_loader.h"

#include <SDL3/SDL.h>

#include <GLES3/gl3.h>

#include "stb_image.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

extern "C" int OHOS_ReadRawFile(const char* path, void** data, size_t* size);
extern "C" void OHOS_FreeRawFile(void* data);

namespace {

// ---------------------------------------------------------------------------
// Math.  Column-major, matching both GLSL's mat4 memory layout and the helper
// functions in the Vulkan backend.
// ---------------------------------------------------------------------------

struct Mat4 {
    float value[16]{};
};

Mat4 Mat4Identity()
{
    Mat4 result{};
    result.value[0] = 1.0f;
    result.value[5] = 1.0f;
    result.value[10] = 1.0f;
    result.value[15] = 1.0f;
    return result;
}

Mat4 Mat4FromSceneEntity(const scene::Definition& definition, const scene::Entity& entity)
{
    const float runtimeGroundTop = physics::kGroundCollider.centerY +
        physics::kGroundCollider.halfExtentY;
    const scene::Transform transform =
        scene::RuntimeTransform(definition, entity, runtimeGroundTop);
    Mat4 result{};
    scene::BuildModelMatrix(transform, result.value);
    return result;
}

Mat4 Mat4Multiply(const Mat4& left, const Mat4& right)
{
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (int index = 0; index < 4; ++index) {
                value += left.value[index * 4 + row] * right.value[column * 4 + index];
            }
            result.value[column * 4 + row] = value;
        }
    }
    return result;
}

bool ProjectWorldToScreen(const Mat4& viewProj, float x, float y, float z,
    float width, float height, float* screenX, float* screenY)
{
    const float clipX = viewProj.value[0] * x + viewProj.value[4] * y
        + viewProj.value[8] * z + viewProj.value[12];
    const float clipY = viewProj.value[1] * x + viewProj.value[5] * y
        + viewProj.value[9] * z + viewProj.value[13];
    const float clipW = viewProj.value[3] * x + viewProj.value[7] * y
        + viewProj.value[11] * z + viewProj.value[15];
    if (clipW <= 1.0e-4f || width <= 0.0f || height <= 0.0f) {
        return false;
    }
    const float ndcX = clipX / clipW;
    const float ndcY = clipY / clipW;
    if (ndcX < -1.15f || ndcX > 1.15f || ndcY < -1.15f || ndcY > 1.15f) {
        return false;
    }
    *screenX = (ndcX * 0.5f + 0.5f) * width;
    // GLES' viewport is bottom-left based; the UI overlay is top-left based.
    *screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * height;
    return true;
}

// Standard GL perspective: clip z maps to [-1, 1].  The Vulkan backend needs
// the other convention (z in [0, 1]), which is why the two do not share this.
Mat4 Mat4PerspectiveGl(float fieldOfViewRadians, float aspect, float nearPlane, float farPlane)
{
    Mat4 result{};
    const float focalLength = 1.0f / std::tan(fieldOfViewRadians * 0.5f);
    result.value[0] = focalLength / aspect;
    result.value[5] = focalLength;
    result.value[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.value[11] = -1.0f;
    result.value[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

// Stable directional-light camera shared with the Vulkan shadow path. The
// center, extent and light direction are intentionally kept identical in both
// RHI implementations so shadow silhouettes remain comparable.
Mat4 Mat4ShadowView()
{
    constexpr float kLightX = 0.45f;
    constexpr float kLightY = 1.0f;
    constexpr float kLightZ = 0.55f;
    constexpr float kLightDistance = 20.0f;
    constexpr float kCenterX = 0.0f;
    constexpr float kCenterY = -0.2f;
    constexpr float kCenterZ = 0.0f;
    const float lightLength = std::sqrt(kLightX * kLightX + kLightY * kLightY + kLightZ * kLightZ);
    const float lx = kLightX / lightLength;
    const float ly = kLightY / lightLength;
    const float lz = kLightZ / lightLength;
    const float fx = -lx;
    const float fy = -ly;
    const float fz = -lz;
    float rx = fy;
    float ry = -fx;
    const float rightLength = std::sqrt(rx * rx + ry * ry);
    rx /= rightLength;
    ry /= rightLength;
    const float ux = ry * fz;
    const float uy = -rx * fz;
    const float uz = rx * fy - ry * fx;
    const float eyeX = kCenterX + lx * kLightDistance;
    const float eyeY = kCenterY + ly * kLightDistance;
    const float eyeZ = kCenterZ + lz * kLightDistance;
    Mat4 result = Mat4Identity();
    result.value[0] = rx;
    result.value[4] = ry;
    result.value[8] = 0.0f;
    result.value[12] = -(rx * eyeX + ry * eyeY);
    result.value[1] = ux;
    result.value[5] = uy;
    result.value[9] = uz;
    result.value[13] = -(ux * eyeX + uy * eyeY + uz * eyeZ);
    result.value[2] = -fx;
    result.value[6] = -fy;
    result.value[10] = -fz;
    result.value[14] = fx * eyeX + fy * eyeY + fz * eyeZ;
    return result;
}

Mat4 Mat4ShadowProjection()
{
    constexpr float kHalfExtent = 12.0f;
    constexpr float kNearPlane = 0.1f;
    constexpr float kFarPlane = 40.0f;
    Mat4 result = Mat4Identity();
    result.value[0] = 1.0f / (2.0f * kHalfExtent);
    result.value[5] = 1.0f / (2.0f * kHalfExtent);
    result.value[10] = -2.0f / (kFarPlane - kNearPlane);
    result.value[14] = -(kFarPlane + kNearPlane) / (kFarPlane - kNearPlane);
    return result;
}

Mat4 Mat4Translation(float x, float y, float z)
{
    Mat4 result = Mat4Identity();
    result.value[12] = x;
    result.value[13] = y;
    result.value[14] = z;
    return result;
}

Mat4 Mat4RotationX(float radians)
{
    Mat4 result = Mat4Identity();
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    result.value[5] = cosine;
    result.value[6] = sine;
    result.value[9] = -sine;
    result.value[10] = cosine;
    return result;
}

Mat4 Mat4RotationY(float radians)
{
    Mat4 result = Mat4Identity();
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    result.value[0] = cosine;
    result.value[2] = -sine;
    result.value[8] = sine;
    result.value[10] = cosine;
    return result;
}

// Uniform scale about the origin (static props only; the skinned character
// carries its normalisation inside the joint matrices instead).
Mat4 Mat4Scale(float factor)
{
    Mat4 result = Mat4Identity();
    result.value[0] = factor;
    result.value[5] = factor;
    result.value[10] = factor;
    return result;
}

// Non-uniform scale (per-axis half-extents of a normalised [-1,1] base mesh):
// the ground slab is one cube.glb stretched thin and wide with this.
Mat4 Mat4Scale3(float sx, float sy, float sz)
{
    Mat4 result = Mat4Identity();
    result.value[0] = sx;
    result.value[5] = sy;
    result.value[10] = sz;
    return result;
}

// General column-major 4x4 inverse (adjugate method), needed to back-project
// the depth buffer into world space in the deferred lighting pass.  Returns
// identity for a singular input; the perspective*view chain is never singular.
Mat4 Mat4Invert(const Mat4& matrix)
{
    const float* s = matrix.value;
    float inv[16];
    inv[0] = s[5] * s[10] * s[15] - s[5] * s[11] * s[14] - s[9] * s[6] * s[15] + s[9] * s[7] * s[14]
        + s[13] * s[6] * s[11] - s[13] * s[7] * s[10];
    inv[4] = -s[4] * s[10] * s[15] + s[4] * s[11] * s[14] + s[8] * s[6] * s[15] - s[8] * s[7] * s[14]
        - s[12] * s[6] * s[11] + s[12] * s[7] * s[10];
    inv[8] = s[4] * s[9] * s[15] - s[4] * s[11] * s[13] - s[8] * s[5] * s[15] + s[8] * s[7] * s[13]
        + s[12] * s[5] * s[11] - s[12] * s[7] * s[9];
    inv[12] = -s[4] * s[9] * s[14] + s[4] * s[10] * s[13] + s[8] * s[5] * s[14] - s[8] * s[6] * s[13]
        - s[12] * s[5] * s[10] + s[12] * s[6] * s[9];
    inv[1] = -s[1] * s[10] * s[15] + s[1] * s[11] * s[14] + s[9] * s[2] * s[15] - s[9] * s[3] * s[14]
        - s[13] * s[2] * s[11] + s[13] * s[3] * s[10];
    inv[5] = s[0] * s[10] * s[15] - s[0] * s[11] * s[14] - s[8] * s[2] * s[15] + s[8] * s[3] * s[14]
        + s[12] * s[2] * s[11] - s[12] * s[3] * s[10];
    inv[9] = -s[0] * s[9] * s[15] + s[0] * s[11] * s[13] + s[8] * s[1] * s[15] - s[8] * s[3] * s[13]
        - s[12] * s[1] * s[11] + s[12] * s[3] * s[9];
    inv[13] = s[0] * s[9] * s[14] - s[0] * s[10] * s[13] - s[8] * s[1] * s[14] + s[8] * s[2] * s[13]
        + s[12] * s[1] * s[10] - s[12] * s[2] * s[9];
    inv[2] = s[1] * s[6] * s[15] - s[1] * s[7] * s[14] - s[5] * s[2] * s[15] + s[5] * s[3] * s[14]
        + s[13] * s[2] * s[7] - s[13] * s[3] * s[6];
    inv[6] = -s[0] * s[6] * s[15] + s[0] * s[7] * s[14] + s[4] * s[2] * s[15] - s[4] * s[3] * s[14]
        - s[12] * s[2] * s[7] + s[12] * s[3] * s[6];
    inv[10] = s[0] * s[5] * s[15] - s[0] * s[7] * s[13] - s[4] * s[1] * s[15] + s[4] * s[3] * s[13]
        + s[12] * s[1] * s[7] - s[12] * s[3] * s[5];
    inv[14] = -s[0] * s[5] * s[14] + s[0] * s[6] * s[13] + s[4] * s[1] * s[14] - s[4] * s[2] * s[13]
        - s[12] * s[1] * s[6] + s[12] * s[2] * s[5];
    inv[3] = -s[1] * s[6] * s[11] + s[1] * s[7] * s[10] + s[5] * s[2] * s[11] - s[5] * s[3] * s[10]
        - s[9] * s[2] * s[7] + s[9] * s[3] * s[6];
    inv[7] = s[0] * s[6] * s[11] - s[0] * s[7] * s[10] - s[4] * s[2] * s[11] + s[4] * s[3] * s[10]
        + s[8] * s[2] * s[7] - s[8] * s[3] * s[6];
    inv[11] = -s[0] * s[5] * s[11] + s[0] * s[7] * s[9] + s[4] * s[1] * s[11] - s[4] * s[3] * s[9]
        - s[8] * s[1] * s[7] + s[8] * s[3] * s[5];
    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9]
        + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];
    const float determinant = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (determinant == 0.0f) {
        return Mat4Identity();
    }
    const float inverseDeterminant = 1.0f / determinant;
    Mat4 result{};
    for (int index = 0; index < 16; ++index) {
        result.value[index] = inv[index] * inverseDeterminant;
    }
    return result;
}

// Same member order and std140 offsets as the Vulkan path's SceneUniforms, so
// the two backends upload identical bytes.
struct SceneUniforms {
    Mat4 cubeMvp;
    Mat4 skyMvp;
    Mat4 modelMvp;
    Mat4 modelMatrix;
    Mat4 viewProj;
};

static_assert(sizeof(SceneUniforms) == sizeof(float) * 80, "Scene UBO must contain five mat4 values");

// ---------------------------------------------------------------------------
// Skeletal animation math (glTF semantics, column-major float[16] buffers).
// skin joint = normalise * (globalJoint * inverseBind);  globals propagate
// parent-first over Model::nodes.  Mirrors MikanEngine's ModelLoader sampling.
// ---------------------------------------------------------------------------

// Column-major multiply for raw 16-float matrices.
void Mat4Multiply16(const float* a, const float* b, float* out)
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out[column * 4 + row] = a[row] * b[column * 4] + a[4 + row] * b[column * 4 + 1]
                + a[8 + row] * b[column * 4 + 2] + a[12 + row] * b[column * 4 + 3];
        }
    }
}

// Compose TRS (glTF order: scale first, then rotate, then translate) into a
// column-major matrix.  Matches cgltf_node_transform_local exactly (see the
// vendored cgltf.h); scale folds into the rotation's columns.
void ComposeTrs(const float t[3], const float q[4], const float s[3], float* m)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = (1.0f - 2.0f * (y * y + z * z)) * s[0];
    m[1] = (2.0f * (x * y + z * w)) * s[0];
    m[2] = (2.0f * (x * z - y * w)) * s[0];
    m[3] = 0.0f;
    m[4] = (2.0f * (x * y - z * w)) * s[1];
    m[5] = (1.0f - 2.0f * (x * x + z * z)) * s[1];
    m[6] = (2.0f * (y * z + x * w)) * s[1];
    m[7] = 0.0f;
    m[8] = (2.0f * (x * z + y * w)) * s[2];
    m[9] = (2.0f * (y * z - x * w)) * s[2];
    m[10] = (1.0f - 2.0f * (x * x + y * y)) * s[2];
    m[11] = 0.0f;
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1.0f;
}

// Rotation interpolation: slerp with a shortest-path guard, degrading to
// normalised lerp for nearly-parallel quats (matches MikanEngine's sampling).
void SlerpQuat(const float a[4], const float b[4], float f, float out[4])
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    if (dot < 0.0f) {
        bx = -bx; by = -by; bz = -bz; bw = -bw;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        out[0] = a[0] + (bx - a[0]) * f;
        out[1] = a[1] + (by - a[1]) * f;
        out[2] = a[2] + (bz - a[2]) * f;
        out[3] = a[3] + (bw - a[3]) * f;
        const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]
            + out[3] * out[3]);
        if (len > 1e-8f) {
            out[0] /= len; out[1] /= len; out[2] /= len; out[3] /= len;
        }
        return;
    }
    const float theta0 = std::acos(std::min(std::max(dot, -1.0f), 1.0f));
    const float sin0 = std::sin(theta0);
    const float s0 = std::sin((1.0f - f) * theta0) / sin0;
    const float s1 = std::sin(f * theta0) / sin0;
    out[0] = a[0] * s0 + bx * s1;
    out[1] = a[1] * s0 + by * s1;
    out[2] = a[2] * s0 + bz * s1;
    out[3] = a[3] * s0 + bw * s1;
}

// ---------------------------------------------------------------------------
// Shaders.  Hand-written GLSL ES 3.00 rather than transpiled from the Vulkan
// SPIR-V: the scene shaders are small, and sharing a shader compiler between
// two APIs would be a far bigger dependency than this file is worth.
// ---------------------------------------------------------------------------

// Full-screen triangle for the deferred lighting pass.  UVs reconstruct the
// G-buffer sample position; geometry comes from gl_VertexID, no buffers.
const char* const kFullscreenVertexSource = R"(#version 300 es
out vec2 vUV;

void main()
{
    vec2 position;
    if (gl_VertexID == 0) {
        position = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        position = vec2(3.0, -1.0);
    } else {
        position = vec2(-1.0, 3.0);
    }
    gl_Position = vec4(position, 0.0, 1.0);
    vUV = position * 0.5 + 0.5;
}
)";

// Vertex shader for the cubemap convolution passes (irradiance / prefilter).
// The background skybox may use the fixed (x, y, -1) screen ray because the
// camera never rotates, but a cubemap face target needs its own basis: with a
// shared mapping every face received the SAME swath of the skybox, so the
// reflections read as multiple copies of the sky tiled across the helmet.
// Directions below follow the GL spec cube-map selection table exactly
// (u, v = NDC within the face viewport).
const char* const kConvolutionVertexSource = R"(#version 300 es
out vec3 vDirection;
uniform int cubeFace;

void main()
{
    vec2 position;
    if (gl_VertexID == 0) {
        position = vec2(-1.0, -1.0);
    } else if (gl_VertexID == 1) {
        position = vec2(3.0, -1.0);
    } else {
        position = vec2(-1.0, 3.0);
    }
    gl_Position = vec4(position, 0.0, 1.0);
    float u = position.x;
    float v = position.y;
    if (cubeFace == 0) {
        vDirection = vec3(1.0, -v, -u);   // +X
    } else if (cubeFace == 1) {
        vDirection = vec3(-1.0, -v, u);   // -X
    } else if (cubeFace == 2) {
        vDirection = vec3(u, 1.0, v);     // +Y
    } else if (cubeFace == 3) {
        vDirection = vec3(u, -1.0, -v);   // -Y
    } else if (cubeFace == 4) {
        vDirection = vec3(u, -v, 1.0);    // +Z
    } else {
        vDirection = vec3(-u, -v, -1.0);  // -Z
    }
}
)";

// On-screen joystick overlay: a tiny flat-shader pair drawing window-pixel
// circles for the movement stick (position fed by the touch layer each frame).
const char* const kOverlayVertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform vec2 overlayResolution;

void main()
{
    vec2 clip = vec2(aPos.x / overlayResolution.x * 2.0 - 1.0,
                     1.0 - aPos.y / overlayResolution.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
}
)";

const char* const kOverlayFragmentSource = R"(#version 300 es
precision mediump float;

uniform vec4 overlayColor;
out vec4 outColor;

void main()
{
    outColor = overlayColor;
}
)";

// MikanEngine action icons are white-on-transparent PNGs.  Keep the button
// face in the flat overlay path and sample only the icon alpha here so the
// same source artwork works on GLES and Vulkan without baking button colours
// into the asset.
const char* const kIconVertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform vec2 iconResolution;
out vec2 iconUV;

void main()
{
    vec2 clip = vec2(aPos.x / iconResolution.x * 2.0 - 1.0,
                     1.0 - aPos.y / iconResolution.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    iconUV = aUV;
}
)";

const char* const kIconFragmentSource = R"(#version 300 es
precision mediump float;

uniform sampler2D iconTexture;
uniform vec4 iconColor;
in vec2 iconUV;
out vec4 outColor;

void main()
{
    float alpha = texture(iconTexture, iconUV).a;
    outColor = vec4(iconColor.rgb, iconColor.a * alpha);
}
)";

// SDF text overlay -- direct GLES 3.0 port of MikanEngine's ui2d_sdf.frag.
// The atlas stores one normalised signed distance in R=G=B, so median()
// degenerates to the plain SDF while keeping the MSDF shader shape.  AA is
// the engine's dual mode: the precise 1-screen-pixel edge when screenPxRange
// >= 1, an fwidth-adaptive fallback for tiny text, blended in between.
const char* const kTextVertexSource = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform vec2 textResolution;
uniform float textScreenPxRange;
out vec2 fragUV;
out vec4 fragColor;
out float fragScreenPxRange;

void main()
{
    vec2 clip = vec2(aPos.x / textResolution.x * 2.0 - 1.0,
                     1.0 - aPos.y / textResolution.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    fragUV = aUV;
    fragColor = aColor;
    fragScreenPxRange = textScreenPxRange;
}
)";

const char* const kTextFragmentSource = R"(#version 300 es
precision mediump float;

uniform sampler2D textAtlas;
in vec2 fragUV;
in vec4 fragColor;
in float fragScreenPxRange;
out vec4 outColor;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec4 sample4 = texture(textAtlas, fragUV);
    float sd = median(sample4.r, sample4.g, sample4.b);
    float effectiveRange = max(fragScreenPxRange, 1.0);
    float preciseAlpha = clamp(effectiveRange * (sd - 0.5) + 0.5, 0.0, 1.0);
    float w = fwidth(sd);
    float fallback = smoothstep(0.5 - w, 0.5 + w, sd);
    float k = smoothstep(0.5, 1.5, fragScreenPxRange);
    float alpha = mix(fallback, preciseAlpha, k);
    outColor = vec4(fragColor.rgb, fragColor.a * alpha);
}
)";

// Display exposure shared by the skybox and PBR display transforms.  Reinhard
// alone dims LDR sky art (0.8 sRGB -> ~0.70 on screen); a >1 exposure restores
// the sky's punch while the reflection <-> background match stays intact.
const float kDisplayExposure = 1.8f;

// Deferred lighting pass (MikanEngine fullscreen.frag structure): samples the
// MRT G-buffer, reconstructs the world position from depth via invViewProj,
// decodes the octahedral normal, and runs the same Cook-Torrance + IBL
// composition the forward path used.  Cleared depth marks the sky, which is
// sampled with the perspective-correct world view ray -- panorama rotation now
// follows the orbit camera implicitly through invViewProj.
const char* const kLightingFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D albedoTexture;     // attachment 0: albedo (sRGB bytes)
uniform sampler2D normalTexture;     // attachment 1: octahedral normal xy
uniform sampler2D materialTexture;   // attachment 2: metallic/roughness/ao
uniform sampler2D emissiveTexture;   // attachment 3: emissive (sRGB bytes)
uniform sampler2D depthTexture;
uniform samplerCube irradianceTexture;
uniform samplerCube envTexture;
uniform sampler2D brdfLutTexture;
uniform samplerCube skyboxTexture;
uniform sampler2D shadowMap;
uniform float skyboxMaxLod;
uniform mat4 invViewProj;
uniform mat4 shadowMatrix;
uniform vec3 cameraPosition;
uniform vec3 emissiveFactor;
uniform int envPrefiltered;
uniform int useBrdfLut;
uniform int useShadowMap;
// 1 = output raw linear HDR for the bloom + AgX tonemap chain; 0 = the legacy
// inline Reinhard + gamma LDR path (used when the driver cannot render to a
// float attachment).
uniform int hdrOutput;
// Debug output switch: 1 = albedo buffer, 2 = decoded world normal.
uniform int debugNormal;

in vec2 vUV;
out vec4 outColor;

const float PI = 3.14159265358979;

vec3 sRGBToLinear(vec3 srgb)
{
    return pow(srgb, vec3(2.2));
}

// Octahedral decode (MikanEngine fullscreen.frag OctahedronDecode).  Uses
// sign-not-zero instead of GLSL sign() so the -Z pole (oct.xy == 0 exactly)
// survives: sign(0) == 0 would collapse both poles onto +Z.
vec2 SignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec3 OctahedronDecode(vec2 oct)
{
    vec3 n = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return normalize(n);
}

float ShadowVisibility(vec3 worldPosition, vec3 N, vec3 L)
{
    if (useShadowMap == 0) {
        return 1.0;
    }
    vec4 shadowH = shadowMatrix * vec4(worldPosition, 1.0);
    vec3 shadowNdc = shadowH.xyz / max(shadowH.w, 1e-5);
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float receiverDepth = shadowNdc.z * 0.5 + 0.5;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 ||
        shadowUv.y >= 1.0 || receiverDepth <= 0.0 || receiverDepth >= 1.0) {
        return 1.0;
    }
    // Use a larger slope-scaled receiver bias for this single 1024^2 map.
    // The raster pass also applies polygon offset; keeping both terms here
    // suppresses acne without relying on a driver-specific depth precision.
    float bias = max(0.008 * (1.0 - max(dot(N, L), 0.0)), 0.0015);
    vec2 texel = vec2(1.0 / 1024.0);
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float shadowDepth = texture(shadowMap, shadowUv + vec2(x, y) * texel).r;
            visible += receiverDepth - bias <= shadowDepth ? 1.0 : 0.0;
        }
    }
    return visible / 9.0;
}

// ---- direct light terms (identical to MikanEngine fullscreen.frag) ------

float D_GGX(float NoH, float perceptualRoughness)
{
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float f = NoH * NoH * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * f * f);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float perceptualRoughness)
{
    float alpha = perceptualRoughness * perceptualRoughness;
    float alphaSq = alpha * alpha;
    float ggxV = NoL * sqrt(NoV * NoV * (1.0 - alphaSq) + alphaSq);
    float ggxL = NoV * sqrt(NoL * NoL * (1.0 - alphaSq) + alphaSq);
    return 0.5 / (ggxV + ggxL);
}

vec3 F_Schlick(vec3 f0, float VoH)
{
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}

vec3 EnvBRDF(vec3 f0, float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;

    vec3 fssEss = f0 * ab.x + ab.y;
    float ems = 1.0 - (ab.x + ab.y);
    vec3 fAvg = f0 + (1.0 - f0) / 21.0;
    vec3 fmsEms = ems * fssEss * fAvg / max(1.0 - fAvg * ems, 1e-4);
    return fssEss + fmsEms;
}

float SpecOcclusion(float NoV, float ao, float roughness)
{
    return clamp(pow(NoV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

vec3 PointLight(vec3 lightPos, vec3 lightColor, vec3 N, vec3 V, float NoV,
    vec3 albedo, float metallic, float roughness, vec3 F0, vec3 worldPos)
{
    vec3 toLight = lightPos - worldPos;
    float dist = length(toLight);
    vec3 L = toLight / max(dist, 1e-4);
    float attenuation = 1.0 / (1.0 + dist * dist);
    vec3 H = normalize(V + L);
    float NoL = max(dot(N, L), 0.0);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);
    float d = D_GGX(NoH, roughness);
    float vis = V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), roughness);
    vec3 F = F_Schlick(F0, VoH);
    vec3 spec = d * vis * F;
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * lightColor * NoL * attenuation;
}

void main()
{
    float depth = texture(depthTexture, vUV).r;
    // GL clip z spans [-1, 1]; the depth buffer stores [0, 1].
    vec4 worldH = invViewProj * vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 worldPos = worldH.xyz / worldH.w;

    if (debugNormal == 1) {
        outColor = vec4(sRGBToLinear(texture(albedoTexture, vUV).rgb), 1.0);
        return;
    }
    if (debugNormal == 2) {
        vec2 oct = texture(normalTexture, vUV).xy * 2.0 - 1.0;
        outColor = vec4(OctahedronDecode(oct) * 0.5 + 0.5, 1.0);
        return;
    }

    vec3 V = normalize(cameraPosition - worldPos);

    if (depth >= 0.9999) {
        // Sky background: perspective-correct world view ray.  With the default
        // 60-degree vertical FOV the horizontal angular range (~91 degrees) is
        // nearly identical to the old flat (ndc, -1) mapping, so the panorama
        // scale reads the same while reflections and background now share one
        // exact ray model.
        vec4 farH = invViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
        vec3 dir = normalize(farH.xyz / farH.w - cameraPosition);
        vec3 sky = pow(texture(skyboxTexture, dir).rgb, vec3(2.2));  // sRGB -> linear
        if (hdrOutput == 1) {
            // Linear scene value into the HDR composite; the tonemap pass owns
            // exposure and the display transform (engine chain order).
            outColor = vec4(sky, 1.0);
            return;
        }
        sky *= 1.8;   // keep in sync with kDisplayExposure (GLSL sees a literal)
        sky = sky / (sky + vec3(1.0));
        outColor = vec4(pow(sky, vec3(1.0 / 2.2)), 1.0);
        return;
    }

    vec3 albedo = sRGBToLinear(texture(albedoTexture, vUV).rgb);
    vec4 mat = texture(materialTexture, vUV);
    float metallic = clamp(mat.x, 0.0, 1.0);
    float roughness = clamp(mat.y, 0.045, 1.0);
    float ao = mat.z;
    vec3 emissive = sRGBToLinear(texture(emissiveTexture, vUV).rgb) * emissiveFactor;
    vec3 N = OctahedronDecode(texture(normalTexture, vUV).xy * 2.0 - 1.0);

    // Key light is deliberately angled so the scene shadow has a visible
    // footprint instead of collapsing into a small noon-time patch.
    vec3 L = normalize(vec3(0.45, 1.0, 0.55));
    vec3 H = normalize(V + L);
    vec3 lightColor = vec3(1.0, 0.97, 0.92) * 3.0;

    float NoL = max(dot(N, L), 0.0);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = max(dot(N, H), 0.0);
    float VoH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float distribution = D_GGX(NoH, roughness);
    float visibility = V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), roughness);
    vec3 F = F_Schlick(F0, VoH);
    vec3 specular = distribution * visibility * F;
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 direct = (kD * albedo / PI + specular) * lightColor * NoL *
        ShadowVisibility(worldPos, N, L);

    // Fill light, anchored to the camera (three-point turntable fill).
    vec3 Lf = normalize(vec3(0.0, 0.35, 1.0));
    vec3 Hf = normalize(V + Lf);
    vec3 fillColor = vec3(0.55, 0.62, 0.75) * 1.1;
    float NoLf = max(dot(N, Lf), 0.0);
    float NoHf = max(dot(N, Hf), 0.0);
    float VoHf = max(dot(V, Hf), 0.0);
    float distributionF = D_GGX(NoHf, roughness);
    float visibilityF = V_SmithGGXCorrelated(NoV, max(NoLf, 1e-4), roughness);
    vec3 FF = F_Schlick(F0, VoHf);
    vec3 specularF = distributionF * visibilityF * FF;
    vec3 kDF = (1.0 - FF) * (1.0 - metallic);
    vec3 fill = (kDF * albedo / PI + specularF) * fillColor * NoLf;

    // Rim accent pair: red portside, blue starboard.
    vec3 pointLights =
        PointLight(vec3(-1.3, 0.4, 1.1), vec3(1.0, 0.12, 0.10) * 8.0,
            N, V, NoV, albedo, metallic, roughness, F0, worldPos)
      + PointLight(vec3(1.3, 0.4, 1.1), vec3(0.10, 0.25, 1.0) * 8.0,
            N, V, NoV, albedo, metallic, roughness, F0, worldPos);

    // IBL ambient (MikanEngine full-tier composition).
    vec3 R = reflect(-V, N);
    vec3 F_ibl = F_Schlick(F0, NoV);
    vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceTexture, N).rgb;
    vec3 diffuseIBL = irradiance * albedo * kD_ibl;
    vec3 envSample = textureLod(envTexture, R, roughness * skyboxMaxLod).rgb;
    vec3 prefiltered = envPrefiltered == 1 ? envSample : sRGBToLinear(envSample);
    // Split-sum environment BRDF: the offline LUT when loaded, otherwise the
    // analytic Karis fit, both with the Fdez-Aguera multiscatter compensation.
    vec2 splitSum = texture(brdfLutTexture, vec2(NoV, roughness)).rg;
    vec3 envBrdf;
    if (useBrdfLut == 1) {
        vec3 fssEss = F0 * splitSum.x + vec3(splitSum.y);
        float ems = clamp(1.0 - (splitSum.x + splitSum.y), 0.0, 1.0);
        vec3 fAvg = F0 + (1.0 - F0) / 21.0;
        vec3 fmsEms = ems * fssEss * fAvg / max(1.0 - fAvg * ems, 1e-4);
        envBrdf = fssEss + fmsEms;
    } else {
        envBrdf = EnvBRDF(F0, roughness, NoV);
    }
    vec3 specularIBL = prefiltered * SpecOcclusion(NoV, ao, roughness) * envBrdf;
    // Karis specular occlusion already fades the specular term by ao; global
    // ao belongs to diffuse only (double occlusion was the ao^2 bug).
    vec3 ambient = diffuseIBL * ao + specularIBL;

    vec3 color = direct + fill + pointLights + ambient + emissive;
    if (hdrOutput == 1) {
        // Linear HDR straight to the float-capable post target; exposure and
        // the AgX curve are applied by the tonemap pass (engine chain order).
        outColor = vec4(color, 1.0);
    } else {
        color *= 1.8;                                 // display exposure (kDisplayExposure)
        color = color / (color + vec3(1.0));          // Reinhard tonemap
        color = pow(color, vec3(1.0 / 2.2));          // linear -> sRGB
        outColor = vec4(color, 1.0);
    }
}
)";

// Global render resolution: the G-buffer, deferred lighting and post chain
// all run at this fixed size regardless of the drawable size; the final FXAA
// resolve upscales to the screen with linear filtering (normalized UVs, so no
// shader change is needed when this value moves).  Note: 1920/1080 = 1.778 vs
// the drawable's 1.691 aspect -- if the ~5% horizontal stretch shows, drop to
// 1826x1080.
constexpr uint32_t kRenderWidth = 1920;
constexpr uint32_t kRenderHeight = 1080;
constexpr GLsizei kShadowMapSize = 1024;

// HDR composite format preference.  R11G11B10 packs into 32 bpp -- half the
// bandwidth of RGBA16F for the same exponent range (up to 65024, ~6e-5 min),
// and it is filterable in ES 3.0 core.  Every consumer of the composite and
// bloom chain reads .rgb only, so the missing alpha is irrelevant.  Probe
// order: R11G11B10 -> RGBA16F -> RGBA8 LDR fallback; all renderable via
// EXT_color_buffer_float, completeness-checked by actually attaching.
constexpr GLenum kHdrFormats[] = {GL_R11F_G11F_B10F, GL_RGBA16F};

// Diagnostic toggle for the flickering-black-blocks investigation: false
// skips the whole bloom ds/up chain (12 of the 15 per-frame FBO binds) and
// zeroes the tonemap bloom weight, leaving G-buffer -> lighting -> AgX
// tonemap -> FXAA.  If the blocks vanish with bloom off, the DGLES
// translation layer is choking on the chained render passes.
constexpr float kBloomStrength = 0.1f;   // engine default 0.2, halved on request

// FXAA 3.11 (quality profile), ported from MikanEngine fxaa.frag.  Input is
// the final LDR image (tonemapped + gamma encoded) produced by the lighting
// pass: luma is measured directly on it, matching the engine decision to run
// FXAA in LDR space so HDR values cannot skew the edge weights.
const char* const kFxaaFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D colorTex;

in vec2 vUV;
out vec4 outColor;

float FxaaLuma(vec3 col) {
    return dot(col, vec3(0.299, 0.587, 0.114));
}

float fxaaQuality[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float fxaaLumaAt(vec2 uv) {
    return FxaaLuma(texture(colorTex, uv).rgb);
}
vec3 fxaaColorAt(vec2 uv) {
    return texture(colorTex, uv).rgb;
}

vec3 FXAA311(vec3 color, vec2 texCoord, vec2 view) {
    float edgeThresholdMin = 0.03125;
    float edgeThresholdMax = 0.125;
    float subpixelQuality = 0.75;
    int iterations = 12;

    float lumaCenter = fxaaLumaAt(texCoord);
    float lumaDown  = fxaaLumaAt(texCoord + vec2( 0.0, -1.0) * view);
    float lumaUp    = fxaaLumaAt(texCoord + vec2( 0.0,  1.0) * view);
    float lumaLeft  = fxaaLumaAt(texCoord + vec2(-1.0,  0.0) * view);
    float lumaRight = fxaaLumaAt(texCoord + vec2( 1.0,  0.0) * view);

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));

    float lumaRange = lumaMax - lumaMin;

    if (lumaRange > max(edgeThresholdMin, lumaMax * edgeThresholdMax)) {
        float lumaDownLeft  = fxaaLumaAt(texCoord + vec2(-1.0, -1.0) * view);
        float lumaUpRight   = fxaaLumaAt(texCoord + vec2( 1.0,  1.0) * view);
        float lumaUpLeft    = fxaaLumaAt(texCoord + vec2(-1.0,  1.0) * view);
        float lumaDownRight = fxaaLumaAt(texCoord + vec2( 1.0, -1.0) * view);

        float lumaDownUp    = lumaDown + lumaUp;
        float lumaLeftRight = lumaLeft + lumaRight;

        float lumaLeftCorners  = lumaDownLeft  + lumaUpLeft;
        float lumaDownCorners  = lumaDownLeft  + lumaDownRight;
        float lumaRightCorners = lumaDownRight + lumaUpRight;
        float lumaUpCorners    = lumaUpRight   + lumaUpLeft;

        float edgeHorizontal = abs(-2.0 * lumaLeft   + lumaLeftCorners ) +
                               abs(-2.0 * lumaCenter + lumaDownUp      ) * 2.0 +
                               abs(-2.0 * lumaRight  + lumaRightCorners);
        float edgeVertical   = abs(-2.0 * lumaUp     + lumaUpCorners   ) +
                               abs(-2.0 * lumaCenter + lumaLeftRight   ) * 2.0 +
                               abs(-2.0 * lumaDown   + lumaDownCorners );

        bool isHorizontal = (edgeHorizontal >= edgeVertical);

        float luma1 = isHorizontal ? lumaDown : lumaLeft;
        float luma2 = isHorizontal ? lumaUp : lumaRight;
        float gradient1 = luma1 - lumaCenter;
        float gradient2 = luma2 - lumaCenter;

        bool is1Steepest = abs(gradient1) >= abs(gradient2);
        float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

        float stepLength = isHorizontal ? view.y : view.x;

        float lumaLocalAverage = 0.0;

        if (is1Steepest) {
            stepLength = - stepLength;
            lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
        } else {
            lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
        }

        vec2 currentUv = texCoord;
        if (isHorizontal) {
            currentUv.y += stepLength * 0.5;
        } else {
            currentUv.x += stepLength * 0.5;
        }

        vec2 offset = isHorizontal ? vec2(view.x, 0.0) : vec2(0.0, view.y);

        vec2 uv1 = currentUv - offset;
        vec2 uv2 = currentUv + offset;

        float lumaEnd1 = fxaaLumaAt(uv1);
        float lumaEnd2 = fxaaLumaAt(uv2);
        lumaEnd1 -= lumaLocalAverage;
        lumaEnd2 -= lumaLocalAverage;

        bool reached1 = abs(lumaEnd1) >= gradientScaled;
        bool reached2 = abs(lumaEnd2) >= gradientScaled;
        bool reachedBoth = reached1 && reached2;

        if (!reached1) {
            uv1 -= offset;
        }
        if (!reached2) {
            uv2 += offset;
        }

        if (!reachedBoth) {
            for(int i = 2; i < iterations; i++) {
                if (!reached1) {
                    lumaEnd1 = fxaaLumaAt(uv1);
                    lumaEnd1 = lumaEnd1 - lumaLocalAverage;
                }
                if (!reached2) {
                    lumaEnd2 = fxaaLumaAt(uv2);
                    lumaEnd2 = lumaEnd2 - lumaLocalAverage;
                }

                reached1 = abs(lumaEnd1) >= gradientScaled;
                reached2 = abs(lumaEnd2) >= gradientScaled;
                reachedBoth = reached1 && reached2;

                if (!reached1) {
                    uv1 -= offset * fxaaQuality[i];
                }
                if (!reached2) {
                    uv2 += offset * fxaaQuality[i];
                }

                if (reachedBoth) break;
            }
        }

        float distance1 = isHorizontal ? (texCoord.x - uv1.x) : (texCoord.y - uv1.y);
        float distance2 = isHorizontal ? (uv2.x - texCoord.x) : (uv2.y - texCoord.y);

        bool isDirection1 = distance1 < distance2;
        float distanceFinal = min(distance1, distance2);

        float edgeThickness = (distance1 + distance2);

        float pixelOffset = - distanceFinal / edgeThickness + 0.5;

        bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;

        bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;

        float finalOffset = correctVariation ? pixelOffset : 0.0;

        float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
        float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
        float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
        float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * subpixelQuality;

        finalOffset = max(finalOffset, subPixelOffsetFinal);

        vec2 finalUv = texCoord;
        if (isHorizontal) {
            finalUv.y += finalOffset * stepLength;
        } else {
            finalUv.x += finalOffset * stepLength;
        }

        color = fxaaColorAt(finalUv);
    }

    return color;
}

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(colorTex, 0));
    vec3 color = texture(colorTex, vUV).rgb;
    // Same 0.98 as the engine's display pass.
    color = FXAA311(color, vUV, texelSize) * 0.98;
    outColor = vec4(color, 1.0);
}
)";

// ---- Bloom + AgX tonemap chain, ported from MikanEngine ----
// Chain order matches engine/postprocess_chain_mobile.json:
//   ds1 (soft-knee threshold) -> ds2-6 (dual kernel) -> up5-1 (dual kernel,
//   fused with the previous up level) -> tonemap (bloom combine + AgX +
//   Bayer dither).  The GLES lighting pass replaces the engine composite.

// bloom_down2x_th.frag: soft-knee threshold extraction on the first
// downsample.  Thresholds operate on linear HDR values (hdrPipeline_) or on
// the LDR fallback image; both are fed through uBloomParams from C++.
const char* const kBloomThresholdFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D inputTex;
uniform vec2 uBloomParams;   // x = threshold, y = knee

in vec2 vUV;
out vec4 outColor;

float SoftKneeContribution(float brightness) {
    float threshold = uBloomParams.x;
    float softKnee = uBloomParams.y * threshold;
    float soft = brightness - threshold + softKnee;
    soft = clamp(soft, 0.0, 2.0 * softKnee);
    soft = soft * soft / (4.0 * softKnee + 1e-4);
    return max(soft, brightness - threshold) / max(brightness, 1e-4);
}

void main() {
    vec2 px = 1.0 / vec2(textureSize(inputTex, 0));
    // Dual downsample kernel: center x4 + four diagonal taps, /8.  Every tap
    // is thresholded before averaging (extract-then-downsample).
    vec3 sum = texture(inputTex, vUV).rgb;
    sum *= SoftKneeContribution(dot(sum, vec3(0.2126, 0.7152, 0.0722)));
    vec3 c = texture(inputTex, vUV + vec2(-px.x,  px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, vUV + vec2( px.x,  px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, vUV + vec2(-px.x, -px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    c = texture(inputTex, vUV + vec2( px.x, -px.y)).rgb;
    sum += c * SoftKneeContribution(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    outColor = vec4(sum * 0.125, 1.0);
}
)";

// bloom_down2x.frag (BLOOM_KERNEL_DUAL variant): Kawase dual kernel,
// effective radius ~2 texels for a wide spread.
const char* const kBloomDownFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D inputTex;

in vec2 vUV;
out vec4 outColor;

void main() {
    vec2 px = 1.0 / vec2(textureSize(inputTex, 0));
    vec3 sum = texture(inputTex, vUV).rgb * 4.0;
    sum += texture(inputTex, vUV + vec2(-px.x,  px.y)).rgb;
    sum += texture(inputTex, vUV + vec2( px.x,  px.y)).rgb;
    sum += texture(inputTex, vUV + vec2(-px.x, -px.y)).rgb;
    sum += texture(inputTex, vUV + vec2( px.x, -px.y)).rgb;
    outColor = vec4(sum * 0.125, 1.0);
}
)";

// bloom_up2x.frag (BLOOM_KERNEL_DUAL variant): dual kernel, fused -- current
// level's downsampled detail (full weight) + previous up level (coarser).
const char* const kBloomUpFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D inputTex;   // current level ds image (this output's size)
uniform sampler2D prevTex;    // previous up level (half size)

in vec2 vUV;
out vec4 outColor;

void main() {
    vec2 pxCurr = 1.0 / vec2(textureSize(inputTex, 0));
    vec2 pxPrev = 1.0 / vec2(textureSize(prevTex, 0));
    vec3 sum = vec3(0.0);
    sum += texture(inputTex, vUV + vec2(-pxCurr.x,  pxCurr.y)).rgb * 2.0;
    sum += texture(inputTex, vUV + vec2( pxCurr.x,  pxCurr.y)).rgb * 2.0;
    sum += texture(inputTex, vUV + vec2(-pxCurr.x, -pxCurr.y)).rgb * 2.0;
    sum += texture(inputTex, vUV + vec2( pxCurr.x, -pxCurr.y)).rgb * 2.0;
    sum += texture(inputTex, vUV + vec2(0.0,  pxCurr.y * 2.0)).rgb;
    sum += texture(inputTex, vUV + vec2(pxCurr.x * 2.0, 0.0)).rgb;
    sum += texture(inputTex, vUV + vec2(0.0, -pxCurr.y * 2.0)).rgb;
    sum += texture(inputTex, vUV + vec2(-pxCurr.x * 2.0, 0.0)).rgb;
    sum *= 0.0833;
    sum += texture(prevTex, vUV + vec2(-pxPrev.x,  pxPrev.y)).rgb * 2.0;
    sum += texture(prevTex, vUV + vec2( pxPrev.x,  pxPrev.y)).rgb * 2.0;
    sum += texture(prevTex, vUV + vec2(-pxPrev.x, -pxPrev.y)).rgb * 2.0;
    sum += texture(prevTex, vUV + vec2( pxPrev.x, -pxPrev.y)).rgb * 2.0;
    sum += texture(prevTex, vUV + vec2(0.0,  pxPrev.y * 2.0)).rgb;
    sum += texture(prevTex, vUV + vec2(pxPrev.x * 2.0, 0.0)).rgb;
    sum += texture(prevTex, vUV + vec2(0.0, -pxPrev.y * 2.0)).rgb;
    sum += texture(prevTex, vUV + vec2(-pxPrev.x * 2.0, 0.0)).rgb;
    sum *= 0.0833;
    outColor = vec4(sum, 1.0);
}
)";

// tonemap.frag (TONEMAP_MODE 0, AgX): combines composite + bloom (weight
// 0.4/0.3/0.2/0.1 collapsed into the up-chain, applied here as BLOOM_STRENGTH
// 0.2), then AgX (Godot 4.4 EaryChow sigmoid polynomial) + sRGB EOTF + Bayer
// dither.  hdrMode == 0 keeps the legacy LDR image untouched (Reinhard +
// gamma already applied by the lighting pass) and only adds the bloom glow.
const char* const kTonemapFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D bloomTex;    // composite (linear HDR when hdrMode == 1)
uniform sampler2D inputTex;    // bloom up-chain final (0.5x image)
uniform int hdrMode;
uniform float agxExposure;
uniform float bloomStrength;   // 0 when the bloom chain is disabled

in vec2 vUV;
out vec4 outColor;

vec3 agx_contrast_approx(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 0.021 * x + 4.0111 * x2 - 25.682 * x2 * x + 70.359 * x4 - 74.778 * x4 * x + 27.069 * x4 * x2;
}

vec3 tonemap_agx(vec3 color) {
    color = max(color * agxExposure, 2e-10);
    const mat3 srgb_to_rec2020_agx_inset_matrix = mat3(
        0.54490813676363087053, 0.14044005884001287035, 0.088827411851915368603,
        0.37377945959812267119, 0.75410959864013760045, 0.17887712465043811023,
        0.081384976686407536266, 0.10543358536857773485, 0.73224999956948382528);
    const mat3 agx_outset_rec2020_to_srgb_matrix = mat3(
        1.9645509602733325934, -0.29932243390911083839, -0.16436833806080403409,
        -0.85585845117807513559, 1.3264510741502356555, -0.23822464068860595117,
        -0.10886710826831608324, -0.027084020983874825605, 1.402665347143271889);
    const float min_ev = -12.4739311883324;
    const float max_ev = 4.02606881166759;

    color = max(color, 2e-10);
    color = srgb_to_rec2020_agx_inset_matrix * color;
    color = clamp(log2(color), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);
    color = agx_contrast_approx(color);
    color = pow(color, vec3(2.4));
    color = agx_outset_rec2020_to_srgb_matrix * color;
    return color;
}

vec3 linear_to_srgb(vec3 color) {
    color = clamp(color, vec3(0.0), vec3(1.0));
    const vec3 a = vec3(0.055);
    return mix((vec3(1.0) + a) * pow(color, vec3(1.0 / 2.4)) - a, 12.92 * color,
        lessThan(color, vec3(0.0031308)));
}

vec3 sanitize(vec3 c) {
    // Must be a real select: mix(c, 0, 1) evaluates NaN * 0.0 which is still
    // NaN, so a mix() version filters nothing.  Raced texels can also be huge
    // finite values, so clamp after the NaN/Inf removal.
    bvec3 nan = isnan(c);
    bvec3 inf = isinf(c);
    bvec3 bad = bvec3(nan.x || inf.x, nan.y || inf.y, nan.z || inf.z);
    vec3 r = vec3(bad.x ? 0.0 : c.x, bad.y ? 0.0 : c.y, bad.z ? 0.0 : c.z);
    return min(r, vec3(64.0));   // scene HDR values never exceed this
}

vec3 getbloom(vec2 uv) {
    vec3 comp = sanitize(texture(bloomTex, uv).rgb);
    vec3 bloom = sanitize(texture(inputTex, uv).rgb);
    return comp + bloom * bloomStrength;
}

float bayer2(vec2 a) { a = floor(a); return fract(a.x / 2.0 + a.y * a.y * 0.75); }
float bayer4(vec2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
float bayer8(vec2 a) { return bayer4(0.5 * a) * 0.25 + bayer2(a); }

void main() {
    vec3 linear = getbloom(vUV);
    vec3 mapped;
    if (hdrMode == 1) {
        mapped = tonemap_agx(linear);
        mapped = linear_to_srgb(mapped);
    } else {
        mapped = linear;   // legacy LDR path: Reinhard + gamma already applied
    }
    // Dither before the UNORM clamp so the dither boundary survives.
    mapped += (bayer8(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);
    outColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
)";

// One-time diffuse IBL precompute, rendered into a small irradiance cubemap
// face by face: each texel integrates the skybox over the cosine-weighted
// hemisphere around its direction.  Irradiance is extremely low frequency, so
// 32x32 per face is plenty and keeps the convolution to a one-shot ~12M
// texture samples.
const char* const kIrradianceFragmentSource = R"(#version 300 es
precision mediump float;

uniform samplerCube skyboxTexture;

in vec3 vDirection;
out vec4 outColor;

const float PI = 3.14159265359;

void main()
{
    vec3 N = normalize(vDirection);
    vec3 irradiance = vec3(0.0);
    vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    float sampleDelta = 0.05;
    float sampleCount = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            // Linearise each sample before averaging: averaging sRGB texels
            // and converting afterwards is the wrong convolution space.
            vec3 sampleColor = pow(texture(skyboxTexture, sampleVec).rgb, vec3(2.2));
            irradiance += sampleColor * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }
    irradiance = PI * irradiance * (1.0 / sampleCount);
    outColor = vec4(irradiance, 1.0);
}
)";

// GGX-weighted specular prefilter (engine reference: atmo_cube_prefilter.comp).
// The mip-chain approximation blurs far less than the GGX lobe at the same
// roughness, which made every mid-roughness metal read as a mirror; this pass
// convolves the skybox with real GGX importance sampling per mip level.
const char* const kPrefilterFragmentSource = R"(#version 300 es
precision highp float;

uniform samplerCube skyboxTexture;
uniform float roughness;

in vec3 vDirection;
out vec4 outColor;

const float PI = 3.14159265359;

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), RadicalInverseVdC(i));
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float rough)
{
    float alpha = rough * rough;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    vec3 up = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

void main()
{
    vec3 N = normalize(vDirection);
    vec3 V = N;

    const uint kSamples = 64u;
    vec3 color = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = Hammersley(i, kSamples);
        vec3 H = ImportanceSampleGGX(xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NoL = max(dot(N, L), 0.0);
        if (NoL > 0.0) {
            // Base mip of the raw skybox, linearised per sample -- same
            // convolution-space rule as the irradiance convolver.
            color += pow(textureLod(skyboxTexture, L, 0.0).rgb, vec3(2.2)) * NoL;
            weight += NoL;
        }
    }
    outColor = vec4(color / max(weight, 1e-4), 1.0);
}
)";

const char* const kModelVertexSource = R"(#version 300 es
layout(std140) uniform SceneUniforms
{
    mat4 cubeMvp;
    mat4 skyMvp;
    mat4 modelMvp;
    mat4 modelMatrix;
    mat4 viewProj;
} scene;

// Mikan-style animated instancing: both character poses stay in separate
// 256-matrix palettes and gl_InstanceID selects the palette for the current
// instance. Each palette remains 16 KiB, the ES 3.0 minimum block size.
layout(std140) uniform SkinUniforms
{
    mat4 jointMats[256];
} playerSkinBlock;

layout(std140) uniform EnemySkinUniforms
{
    mat4 jointMats[256];
} enemySkinBlock;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
// JOINTS_0 as raw u8 (0..255) and WEIGHTS_0 as normalised u8 (0..1).  Not
// enabled for static meshes -- generic attrib value (0,0,0,1) would wrongly
// trigger skinning, hence the skinningOn gate below.
layout(location = 3) in vec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in vec4 inInstanceModel0;
layout(location = 6) in vec4 inInstanceModel1;
layout(location = 7) in vec4 inInstanceModel2;
layout(location = 8) in vec4 inInstanceModel3;
layout(location = 9) in vec4 inInstanceColor;

uniform int skinningOn;
uniform int instancingOn;
// Enemy death dissolve (0 = intact).  Only instance 1 of the skinned
// character batch is the enemy, so the per-draw uniform is gated on
// gl_InstanceID and static/prop draws (instance 0) can never be affected.
uniform float enemyDissolve;

out vec3 vNormal;
out vec2 vTexCoord;
out vec3 vWorldPosition;
// RGB is the per-instance gameplay marker; A is its blend strength.  A zero
// strength keeps the source material unchanged for the player and props.
out vec4 vInstanceColor;
flat out float vDissolve;

mat4 SkinMatrix(ivec4 joint, vec4 weights, int instanceIndex)
{
    if (instanceIndex == 0) {
        return weights.x * playerSkinBlock.jointMats[joint.x]
            + weights.y * playerSkinBlock.jointMats[joint.y]
            + weights.z * playerSkinBlock.jointMats[joint.z]
            + weights.w * playerSkinBlock.jointMats[joint.w];
    }
    return weights.x * enemySkinBlock.jointMats[joint.x]
        + weights.y * enemySkinBlock.jointMats[joint.y]
        + weights.z * enemySkinBlock.jointMats[joint.z]
        + weights.w * enemySkinBlock.jointMats[joint.w];
}

void main()
{
    mat4 model = scene.modelMatrix;
    vec4 instanceColor = vec4(1.0, 1.0, 1.0, 0.0);
    if (instancingOn == 1) {
        model = mat4(inInstanceModel0, inInstanceModel1, inInstanceModel2, inInstanceModel3);
        instanceColor = inInstanceColor;
    }
    vec3 skinnedPosition = inPosition;
    vec3 skinnedNormal = inNormal;
    if (skinningOn == 1 && dot(inWeights, vec4(1.0)) > 0.001) {
        ivec4 joint = clamp(ivec4(inJoints + vec4(0.5)), ivec4(0), ivec4(255));
        // LBS: skin = sum w_i * (normalise * global_i * inverseBind_i).  The
        // normalisation translate is irrelevant for normals; mat3() of the
        // joint matrix carries its uniform scale, removed by the renormalise.
        mat4 skinMatrix = SkinMatrix(joint, inWeights, gl_InstanceID);
        skinnedPosition = (skinMatrix * vec4(inPosition, 1.0)).xyz;
        skinnedNormal = mat3(skinMatrix) * inNormal;
    }
    vec4 worldPosition = model * vec4(skinnedPosition, 1.0);
    gl_Position = (instancingOn == 1 ? scene.viewProj : scene.modelMvp) *
        (instancingOn == 1 ? worldPosition : vec4(skinnedPosition, 1.0));
    // The loader bakes the node hierarchy and a uniform normalisation scale
    // into static meshes, so the object transform is a pure rotation and its
    // upper-left 3x3 is already the correct normal matrix.
    vNormal = normalize(mat3(model) * skinnedNormal);
    // Full transform (not just mat3): static props carry a translation.  For
    // the character the translation is zero, so this degenerates to the old
    // behaviour -- enough for view-vector reconstruction in the PBR shader.
    vWorldPosition = worldPosition.xyz;
    // No v flip: the model texture uses REPEAT wrap (glTF's default sampler),
    // and DamagedHelmet's UVs deliberately sit in v [1,2] -- REPEAT's fract()
    // folds them back into [0,1] with the correct orientation.  A v flip here
    // would mirror the image back to front.
    vTexCoord = inTexCoord;
    vInstanceColor = instanceColor;
    vDissolve = gl_InstanceID == 1 ? enemyDissolve : 0.0;
}
)";

// Depth-only vertex path for the single scene shadow map. It shares the
// model vertex layout and SkinUniforms contract with the G-buffer pass so
// animated and static meshes cast from the same displayed geometry.
const char* const kShadowVertexSource = R"(#version 300 es
layout(std140) uniform SkinUniforms
{
    mat4 jointMats[256];
} playerSkinBlock;

layout(std140) uniform EnemySkinUniforms
{
    mat4 jointMats[256];
} enemySkinBlock;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in vec4 inInstanceModel0;
layout(location = 6) in vec4 inInstanceModel1;
layout(location = 7) in vec4 inInstanceModel2;
layout(location = 8) in vec4 inInstanceModel3;

uniform mat4 shadowMvp;
uniform int skinningOn;
uniform int instancingOn;
uniform float enemyDissolve;

flat out float vDissolve;
out vec3 vShadowWorldPos;

mat4 SkinMatrix(ivec4 joint, vec4 weights, int instanceIndex)
{
    if (instanceIndex == 0) {
        return weights.x * playerSkinBlock.jointMats[joint.x]
            + weights.y * playerSkinBlock.jointMats[joint.y]
            + weights.z * playerSkinBlock.jointMats[joint.z]
            + weights.w * playerSkinBlock.jointMats[joint.w];
    }
    return weights.x * enemySkinBlock.jointMats[joint.x]
        + weights.y * enemySkinBlock.jointMats[joint.y]
        + weights.z * enemySkinBlock.jointMats[joint.z]
        + weights.w * enemySkinBlock.jointMats[joint.w];
}

void main()
{
    mat4 model = mat4(1.0);
    if (instancingOn == 1) {
        model = mat4(inInstanceModel0, inInstanceModel1, inInstanceModel2, inInstanceModel3);
    }
    vec3 shadowPosition = inPosition;
    if (skinningOn == 1 && dot(inWeights, vec4(1.0)) > 0.001) {
        ivec4 joint = clamp(ivec4(inJoints + vec4(0.5)), ivec4(0), ivec4(255));
        mat4 skinMatrix = SkinMatrix(joint, inWeights, gl_InstanceID);
        shadowPosition = (skinMatrix * vec4(inPosition, 1.0)).xyz;
    }
    vec4 shadowWorldPosition = model * vec4(shadowPosition, 1.0);
    gl_Position = shadowMvp * shadowWorldPosition;
    vDissolve = gl_InstanceID == 1 ? enemyDissolve : 0.0;
    vShadowWorldPos = shadowWorldPosition.xyz;
}
)";

const char* const kShadowFragmentSource = R"(#version 300 es
precision highp float;

flat in float vDissolve;
in vec3 vShadowWorldPos;

// Same noise threshold as the G-buffer dissolve so the shadow of the dying
// enemy crumbles away with the body instead of lingering as a full silhouette.
float DissolveHash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float DissolveNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(DissolveHash(i),
                DissolveHash(i + vec3(1.0, 0.0, 0.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 0.0)),
                DissolveHash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(mix(DissolveHash(i + vec3(0.0, 0.0, 1.0)),
                DissolveHash(i + vec3(1.0, 0.0, 1.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 1.0)),
                DissolveHash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

// Depth-only pass: the framebuffer has no color attachment, so fixed-function
// depth writes are the complete fragment output.
void main()
{
    if (vDissolve > 0.0) {
        if (DissolveNoise(vShadowWorldPos * 5.0) < vDissolve * 1.18 - 0.09) {
            discard;
        }
    }
}
)";

// Geometry pass of the deferred pipeline.  Writes MRT attachments following
// MikanEngine's model.frag layout:
//   attachment 0: albedo, sRGB bytes (linearised in the lighting pass)
//   attachment 1: octahedral-encoded world normal, xy only.  The engine stores
//     R16G16_SNORM; RGBA8 UNORM here because the DGLES bridge gives no float/
//     SNORM renderable guarantee -- 8-bit octahedral keeps the full signed
//     direction with no z-reconstruction ambiguity.
//   attachment 2: material (metallic, roughness, ao) -- engine material layout
//   attachment 3: emissive sRGB texel.  The engine's attachment 3 is the TAA
//     motion vector, skipped per current scope; emissive sits here so unlit
//     emission stays unlit instead of being lit through albedo.
const char* const kGBufferFragmentSource = R"(#version 300 es
precision highp float;

uniform sampler2D baseColorTexture;
uniform sampler2D metallicRoughnessTexture;
uniform sampler2D normalTexture;
uniform sampler2D aoTexture;
uniform sampler2D emissiveTexture;

uniform vec3 cameraPosition;
// x = metallic factor, y = roughness factor, z = normal scale, w = unused.
uniform vec4 materialParams;
uniform vec4 materialBaseColorFactor;
// 0 when the material has no normal map.  PerturbNormal reconstructs the
// tangent frame from screen-space derivatives, and feeding it the white
// fallback texel yields mapN = (1,1,1) -- a per-pixel pseudo-random rotation
// of the geometric normal, i.e. full-screen shading noise on textureless
// assets like the Quaternius character.
uniform int hasNormalMap;

in vec3 vNormal;
in vec2 vTexCoord;
in vec3 vWorldPosition;
in vec4 vInstanceColor;
flat in float vDissolve;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;

// Enemy death dissolve: a smooth value-noise field over the world position
// recedes with the dissolve amount.  Fragments below the threshold are gone,
// the band around it glows like burning embers.  Must stay in lockstep with
// kShadowFragmentSource so the shadow crumbles with the body.
float DissolveHash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float DissolveNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(DissolveHash(i),
                DissolveHash(i + vec3(1.0, 0.0, 0.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 0.0)),
                DissolveHash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(mix(DissolveHash(i + vec3(0.0, 0.0, 1.0)),
                DissolveHash(i + vec3(1.0, 0.0, 1.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 1.0)),
                DissolveHash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

// Octahedral mapping (MikanEngine model.frag).  The fold must use
// sign-not-zero: GLSL sign(0) == 0 would map the -Z pole and the +Z pole to
// the same (0,0) and the lighting pass could not recover the facing.
vec2 SignNotZero(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0,
                v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 OctahedronEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return n.xy;
}

// Cotangent-frame normal mapping (three.js perturbNormal2Arb): reconstructs
// the tangent basis per-fragment from screen-space derivatives, so assets
// without explicit TANGENT attributes still get normal-map detail.
vec3 PerturbNormal(vec3 N, vec3 V, vec2 uv)
{
    vec3 mapN = texture(normalTexture, uv).xyz * 2.0 - 1.0;
    mapN.xy *= materialParams.z;
    float faceDirection = gl_FrontFacing ? 1.0 : -1.0;

    vec3 dp1 = dFdx(V);
    vec3 dp2 = dFdy(V);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float invMax = inversesqrt(max(dot(T, T), dot(B, B)));
    mat3 TBN = mat3(T * invMax, B * invMax, N);
    mapN.xy *= faceDirection;
    return normalize(TBN * mapN);
}

void main()
{
    // Keep the source material texture/factors intact for normal instances;
    // the enemy marker is an explicit gameplay presentation tint carried by
    // the instance stream rather than a baked texture/material change.
    vec3 baseAlbedo = texture(baseColorTexture, vTexCoord).rgb
        * materialBaseColorFactor.rgb;
    float markerStrength = clamp(vInstanceColor.a, 0.0, 1.0);
    vec3 albedo = mix(baseAlbedo, vInstanceColor.rgb, markerStrength);
    vec4 mr = texture(metallicRoughnessTexture, vTexCoord);
    // Roughness lower bound (0.045) is applied in the lighting pass, after the
    // 8-bit round-trip, matching the forward clamp ordering.
    float metallic = clamp(mr.b * materialParams.x, 0.0, 1.0);
    float roughness = clamp(mr.g * materialParams.y, 0.0, 1.0);
    float ao = texture(aoTexture, vTexCoord).r;

    vec3 N = normalize(vNormal);
    vec3 V = normalize(cameraPosition - vWorldPosition);
    if (hasNormalMap == 1) {
        N = PerturbNormal(N, V, vTexCoord);
    }

    vec3 emberGlow = vec3(0.0);
    if (vDissolve > 0.0) {
        float n = DissolveNoise(vWorldPosition * 5.0);
        float cut = vDissolve * 1.18 - 0.09;
        if (n < cut) {
            discard;
        }
        float edge = 1.0 - smoothstep(0.0, 0.12, abs(n - cut));
        emberGlow = vec3(1.6, 0.55, 0.12) * edge;
        // The material's emissive factor may be zero, so bake part of the
        // ember tint into albedo as well -- the edge must read as "burning"
        // regardless of the emissive pipeline path.
        albedo = mix(albedo, clamp(emberGlow, 0.0, 1.0), edge * 0.85);
    }

    outAlbedo = vec4(albedo, 1.0);
    outNormal = vec4(OctahedronEncode(N) * 0.5 + 0.5, 0.0, 1.0);
    outMaterial = vec4(metallic, roughness, ao, 1.0);
    // Raw emissive texel; the emissive factor multiplies in the lighting pass
    // so factors above 1 are not clamped by the 8-bit attachment.  The
    // dissolve ember rides the same channel.
    outEmissive = vec4(texture(emissiveTexture, vTexCoord).rgb + emberGlow, 1.0);
}
)";

// Debug visualisation mode for the deferred lighting pass (see debugNormal).
// 1 = albedo G-buffer, 2 = decoded octahedral world normal.
constexpr int kDebugNormalMode = 0;

const char* GlErrorName(GLenum error)
{
    switch (error) {
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "GL_ERROR";
    }
}

void LogGlErrors(const char* where)
{
    for (int guard = 0; guard < 8; ++guard) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            return;
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=gl_error where=%s error=%s",
            where, GlErrorName(error));
    }
}

GLuint CompileShader(GLenum type, const char* source, const char* label)
{
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=shader_create_failed shader=%s", label);
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=shader_compile_failed shader=%s log=%s",
            label, log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkProgram(const char* vertexSource, const char* fragmentSource, const char* label)
{
    const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, label);
    if (vertexShader == 0) {
        return 0;
    }
    const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    // The shaders are owned by the program once linked.
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
        glGetProgramInfoLog(program, length, nullptr, log.data());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=program_link_failed program=%s log=%s",
            label, log.data());
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Binds the shared uniform block to binding point 0.  GLSL ES 3.00 has no
// `binding =` qualifier on uniform blocks, so it has to be done from the API.
void BindSceneUniformBlock(GLuint program, const char* label)
{
    const GLuint blockIndex = glGetUniformBlockIndex(program, "SceneUniforms");
    if (blockIndex == GL_INVALID_INDEX) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=uniform_block_missing program=%s", label);
        return;
    }
    glUniformBlockBinding(program, blockIndex, 0);
}

struct GlesMesh {
    GLuint vao = 0;
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLuint indexCount = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    int32_t materialIndex = 0;
};

// Per-instance payload follows MikanEngine's model renderer contract in a
// GLES-compatible form: one model matrix and one material marker per actor.
// gl_InstanceID selects the matching player/enemy animation palette.
struct CharacterInstanceData {
    Mat4 model;
    // RGB is a gameplay marker color; alpha is marker blend strength.
    float color[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};

static_assert(sizeof(CharacterInstanceData) == sizeof(float) * 20,
    "character instance data must contain model and color");

class GlesRenderer final : public rhi::IRenderer {
public:
    ~GlesRenderer() override
    {
        Destroy();
    }

    const char* Name() const override
    {
        return "gles";
    }

    // Touch layer pushes exactly one input frame per rendered frame; it is
    // consumed by UpdateSceneUniforms (the camera state lives there).
    void PushCameraInput(const rhi::CameraInput& input) override
    {
        cameraInput_ = input;
    }

    void SetPhysicsWorld(physics::JoltGameplayPhysics* world) override
    {
        physicsWorld_ = world;
        physicsReset_ = false;
    }

    void SetSceneDefinition(const scene::Definition& definition) override
    {
        sceneDefinition_ = &definition;
    }

    void OnApplicationBackground() override
    {
        if (context_ != nullptr && !SDL_GL_MakeCurrent(nullptr, nullptr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_GLES stage=background_unbind_failed error=%s", SDL_GetError());
        }
    }

    bool OnApplicationForeground() override
    {
        if (window_ == nullptr || context_ == nullptr) {
            SDL_SetError("GLES context is not available while resuming");
            return false;
        }
        // Force the OHOS video driver through MakeCurrent even if a quick
        // background/foreground cycle skipped the main loop's pause hook.
        if (SDL_GL_GetCurrentContext() == context_ && !SDL_GL_MakeCurrent(nullptr, nullptr)) {
            return false;
        }
        if (!SDL_GL_MakeCurrent(window_, context_)) {
            return false;
        }
        return true;
    }

    rhi::UiScreen CurrentScreen() const override
    {
        return uiScreen_;
    }

    bool Initialize(SDL_Window* window, uint64_t width, uint64_t height) override
    {
        if (window == nullptr || width == 0 || height == 0) {
            return false;
        }
        window_ = window;

        if (context_ == nullptr) {
            if (!SDL_GL_LoadLibrary(nullptr)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=gl_load_library_failed error=%s",
                    SDL_GetError());
                return false;
            }
            context_ = SDL_GL_CreateContext(window);
            if (context_ == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=gl_create_context_failed error=%s",
                    SDL_GetError());
                return false;
            }
            if (!SDL_GL_MakeCurrent(window, context_)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=gl_make_current_failed error=%s",
                    SDL_GetError());
                return false;
            }
            // Present on vsync.  Unlike the emulator's Vulkan proxy this costs
            // nothing extra: the frame never round-trips to the host.
            SDL_GL_SetSwapInterval(1);

            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            SDL_Log("SDL3_GLES stage=context_ready version=%s renderer=%s",
                version != nullptr ? version : "?", renderer != nullptr ? renderer : "?");

            if (!CreatePrograms() || !CreateUniformBuffer() || !LoadSkybox()) {
                return false;
            }
            CreateFallbackTexture();
            if (!CreateShadowMapTarget()) {
                SDL_Log("SDL3_GLES stage=shadow_unavailable reason=target_creation_failed");
            }
            // Diffuse IBL precompute.  Not fatal if it fails: the shader then
            // samples the raw skybox as the irradiance source, which skews the
            // ambient toward the sharp image but never leaves a slot unbound.
            if (!CreateIrradianceMap()) {
                SDL_Log("SDL3_GLES stage=irradiance_unavailable reason=creation_failed");
                irradianceTexture_ = skyboxTexture_;
            }
            // Specular IBL precompute.  Also not fatal: without it the model
            // shader falls back to the skybox's naive mip chain, which reads
            // glossier than the GGX lobe but never leaves a slot unbound.
            if (!CreatePrefilteredMap()) {
                SDL_Log("SDL3_GLES stage=prefilter_unavailable reason=creation_failed");
            }
            // Offline split-sum BRDF LUT.  Not fatal without it: the shader
            // falls back to the analytic multiscatter fit.
            if (!CreateBrdfLut()) {
                SDL_Log("SDL3_GLES stage=brdf_lut_unavailable reason=load_failed");
            }
            CreateOverlayResources();
            CreateTextResources();
            // A model that fails to load is not fatal: the skybox alone is a
            // valid scene, and the reason is logged by the loader.
            LoadModel();

            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDisable(GL_CULL_FACE);
            glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
            sceneReady_ = true;
        }

        // The drawable size is re-read every frame by the caller; the viewport
        // is set at draw time, so there is nothing to rebuild here.
        width_ = static_cast<uint32_t>(width);
        height_ = static_cast<uint32_t>(height);
        return true;
    }

    void CreateOverlayResources()
    {
        overlayProgram_ = LinkProgram(kOverlayVertexSource, kOverlayFragmentSource, "overlay");
        if (overlayProgram_ != 0) {
            overlayResolutionLocation_ = glGetUniformLocation(overlayProgram_, "overlayResolution");
            overlayColorLocation_ = glGetUniformLocation(overlayProgram_, "overlayColor");
            glGenBuffers(1, &overlayVbo_);
            glGenVertexArrays(1, &overlayVao_);
            glBindVertexArray(overlayVao_);
            glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray(0);
        }

        iconProgram_ = LinkProgram(kIconVertexSource, kIconFragmentSource, "touch_icons");
        if (iconProgram_ != 0) {
            iconResolutionLocation_ = glGetUniformLocation(iconProgram_, "iconResolution");
            iconSamplerLocation_ = glGetUniformLocation(iconProgram_, "iconTexture");
            iconColorLocation_ = glGetUniformLocation(iconProgram_, "iconColor");
            glGenBuffers(1, &iconVbo_);
            glGenVertexArrays(1, &iconVao_);
            glBindVertexArray(iconVao_);
            glBindBuffer(GL_ARRAY_BUFFER, iconVbo_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                reinterpret_cast<const void*>(2 * sizeof(float)));
            glBindVertexArray(0);
            LoadActionIconTextures();
        }
        LogGlErrors("create_overlay");
    }

    void LoadActionIconTextures()
    {
        static constexpr const char* kPaths[rhi::CameraInput::kActionButtonCount] = {
            "ui/actions/touch_attack.png",
            "ui/actions/touch_jump.png",
            "ui/actions/touch_sprint.png",
            "ui/actions/touch_crouch.png",
        };
        int loaded = 0;
        for (int index = 0; index < rhi::CameraInput::kActionButtonCount; ++index) {
            void* encoded = nullptr;
            size_t encodedSize = 0;
            if (!OHOS_ReadRawFile(kPaths[index], &encoded, &encodedSize) || encoded == nullptr ||
                encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
                if (encoded != nullptr) {
                    OHOS_FreeRawFile(encoded);
                }
                SDL_Log("SDL3_GLES stage=touch_icon_missing index=%d path=%s", index, kPaths[index]);
                continue;
            }
            int imageWidth = 0;
            int imageHeight = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
                static_cast<int>(encodedSize), &imageWidth, &imageHeight, &channels, 4);
            OHOS_FreeRawFile(encoded);
            if (pixels == nullptr || imageWidth <= 0 || imageHeight <= 0) {
                if (pixels != nullptr) {
                    stbi_image_free(pixels);
                }
                SDL_Log("SDL3_GLES stage=touch_icon_decode_failed index=%d path=%s",
                    index, kPaths[index]);
                continue;
            }
            actionIconTextures_[index] = CreateTexture2D(pixels,
                static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight));
            stbi_image_free(pixels);
            if (actionIconTextures_[index] != 0) {
                glBindTexture(GL_TEXTURE_2D, actionIconTextures_[index]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                ++loaded;
            }
        }
        SDL_Log("SDL3_GLES stage=touch_icons_ready count=%d", loaded);
    }

    // Uploads the offline SDF atlas (rawfile, generated by tools/gen_sdf_font.py)
    // and builds the glyph program.  Not fatal without the atlas: text simply
    // does not render.
    void CreateTextResources()
    {
        textProgram_ = LinkProgram(kTextVertexSource, kTextFragmentSource, "sdf_text");
        if (textProgram_ == 0) {
            return;
        }
        textResolutionLocation_ = glGetUniformLocation(textProgram_, "textResolution");
        textPxRangeLocation_ = glGetUniformLocation(textProgram_, "textScreenPxRange");
        textSamplerLocation_ = glGetUniformLocation(textProgram_, "textAtlas");

        void* encoded = nullptr;
        size_t encodedSize = 0;
        if (OHOS_ReadRawFile("SdfAtlas.png", &encoded, &encodedSize) && encoded != nullptr &&
            encodedSize <= static_cast<size_t>(std::numeric_limits<int>::max())) {
            int atlasW = 0;
            int atlasH = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
                static_cast<int>(encodedSize), &atlasW, &atlasH, &channels, 4);
            OHOS_FreeRawFile(encoded);
            if (pixels != nullptr) {
                glGenTextures(1, &textAtlasTexture_);
                glBindTexture(GL_TEXTURE_2D, textAtlasTexture_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlasW, atlasH, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                stbi_image_free(pixels);
            }
        } else if (encoded != nullptr) {
            OHOS_FreeRawFile(encoded);
        }

        glGenBuffers(1, &textVbo_);
        glGenVertexArrays(1, &textVao_);
        glBindVertexArray(textVao_);
        glBindBuffer(GL_ARRAY_BUFFER, textVbo_);
        // Interleaved pos(2) uv(2) color(4), 8 floats per vertex.
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
            reinterpret_cast<const void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
            reinterpret_cast<const void*>(4 * sizeof(float)));
        glBindVertexArray(0);
        LogGlErrors("create_text");
        SDL_Log("SDL3_GLES stage=sdf_text_ready atlas=%d", textAtlasTexture_ != 0 ? 1 : 0);
    }

    // Re-uploads one triangle-fan circle in window pixels (y down, matching
    // the overlay vertex shader's clip mapping).
    void UploadCircleFan(float centerX, float centerY, float radius)
    {
        float vertices[(kOverlaySegments + 2) * 2];
        vertices[0] = centerX;
        vertices[1] = centerY;
        for (int i = 0; i <= kOverlaySegments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kOverlaySegments) *
                6.28318530717958647692f;
            vertices[(i + 1) * 2] = centerX + std::cos(t) * radius;
            vertices[(i + 1) * 2 + 1] = centerY + std::sin(t) * radius;
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    }

    // ---- main menu / settings ------------------------------------------------
    // The renderer owns menu layout, drawing and hit-testing; the touch layer
    // just forwards raw taps (see CameraInput::menuTapEdge).  Buttons are
    // rounded-rectangle pills: overlay triangles + SDF text label.

    // Axis-aligned rounded rectangle, centre (cx, cy), half extent (hw, hh),
    // corner radius r (r == hh gives a pill).
    struct MenuButtonPos { float cx; float cy; float hw; float hh; float r; };

    // index meaning per screen -- MainMenu: 0 = START, 1 = SETTINGS;
    // Settings: 0 = FPS toggle, 1 = sensitivity, 2 = bloom toggle,
    // 3 = render scale, 4 = back (top-right corner button);
    // Gameplay: 0 = SET, 1 = BAG (vertical stack on the right edge);
    // GameplaySettings: 0 = close, 1 = FPS, 2 = bloom, 3 = render scale,
    // 4 = return to title; GameplayRetry: 0 = RETRY, 5 = MENU.
    MenuButtonPos MenuButtonLayout(int index) const
    {
        const float w = static_cast<float>(width_);
        const float h = static_cast<float>(height_);
        const float mn = w < h ? w : h;
        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            if (index == 0) {
                return {w * 0.5f, h * 0.62f, mn * 0.22f, mn * 0.072f,
                    mn * 0.072f};  // RETRY
            }
            return {w * 0.95f, h * 0.085f, mn * 0.040f, mn * 0.040f,
                mn * 0.040f};      // MENU
        }
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            // Vertical stack along the right edge, above the action-button
            // cluster: SET above, BAG below.  Easier to reach than the old
            // top row and clear of the camera-drag heartland.
            if (index == 0) {
                return {w * 0.93f, h * 0.26f, mn * 0.055f, mn * 0.055f,
                    mn * 0.055f};          // SET
            }
            return {w * 0.93f, h * 0.42f, mn * 0.055f, mn * 0.055f,
                mn * 0.055f};              // BAG
        }
        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            // 0 = close, 1..3 = equipment slots (weapon/helmet/armor),
            // 10+i = bag grid cell i (5 columns x 3 rows).
            if (index == 0) {
                return {w * 0.5f + mn * 0.395f, h * 0.17f,
                    mn * 0.026f, mn * 0.026f, mn * 0.018f};
            }
            if (index >= 1 && index <= 3) {
                static const float kEquipY[3] = {0.33f, 0.50f, 0.67f};
                return {w * 0.5f - mn * 0.335f, h * kEquipY[index - 1],
                    mn * 0.068f, mn * 0.068f, mn * 0.012f};
            }
            if (index >= 10) {
                const int cell = index - 10;
                const int col = cell % 5;
                const int row = cell / 5;
                static const float kGridRowY[3] = {0.34f, 0.47f, 0.60f};
                return {w * 0.5f - mn * 0.135f + mn * 0.043f + mn * 0.099f * col,
                    h * kGridRowY[row], mn * 0.043f, mn * 0.043f, mn * 0.010f};
            }
            return {w * 0.5f + mn * 0.395f, h * 0.17f,
                mn * 0.026f, mn * 0.026f, mn * 0.018f};
        }
        if (uiScreen_ == rhi::UiScreen::MainMenu) {
            if (index == 0) {
                return {w * 0.5f, h * 0.60f, mn * 0.24f, mn * 0.072f,
                    mn * 0.072f};  // START (pill)
            }
            return {w * 0.5f, h * 0.83f, mn * 0.24f, mn * 0.062f,
                mn * 0.062f};      // SETTINGS
        }
        if (uiScreen_ == rhi::UiScreen::GameplaySettings) {
            if (index >= 1 && index <= 4) {
                static const float kDisplayRowY[4] = {0.36f, 0.46f, 0.56f, 0.66f};
                return {w * 0.5f + mn * 0.17f, h * kDisplayRowY[index - 1],
                    mn * 0.09f, mn * 0.050f, mn * 0.012f};
            }
            if (index == 5) {
                return {w * 0.5f, h * 0.78f, mn * 0.27f, mn * 0.047f,
                    mn * 0.012f};
            }
            if (index == 0) {
                return {w * 0.5f + mn * 0.29f, h * 0.5f - mn * 0.29f,
                    mn * 0.028f, mn * 0.028f, mn * 0.010f};  // close
            }
            return {w * 0.5f, h * 0.78f, mn * 0.27f, mn * 0.047f,
                mn * 0.012f};
        }
        if (uiScreen_ == rhi::UiScreen::Settings) {
            static const float kSettingsRowY[5] = {0.33f, 0.43f, 0.53f, 0.63f, 0.73f};
            if (index >= 0 && index <= 4) {
                return {w * 0.5f + mn * 0.22f, h * kSettingsRowY[index],
                    mn * 0.095f, mn * 0.034f, mn * 0.008f};
            }
            if (index == 5) {
                return {w * 0.5f + mn * 0.36f, h * 0.17f,
                    mn * 0.026f, mn * 0.026f, mn * 0.018f};
            }
        }
        static const float kRowY[4] = {0.26f, 0.40f, 0.54f, 0.68f};
        switch (index) {
        case 0: return {w * 0.28f, h * kRowY[0], mn * 0.10f, mn * 0.055f,
            mn * 0.055f};          // FPS toggle
        case 1: return {w * 0.28f, h * kRowY[1], mn * 0.10f, mn * 0.055f,
            mn * 0.055f};          // SENSITIVITY
        case 2: return {w * 0.28f, h * kRowY[2], mn * 0.10f, mn * 0.055f,
            mn * 0.055f};          // BLOOM toggle
        case 3: return {w * 0.28f, h * kRowY[3], mn * 0.10f, mn * 0.055f,
            mn * 0.055f};          // RENDER SCALE
        default: return {w * 0.93f, h * 0.085f, mn * 0.048f, mn * 0.048f,
            mn * 0.048f};          // back to main menu, top-right corner
        }
    }

    static bool HitRect(float x, float y, const MenuButtonPos& b)
    {
        const float ex = std::fabs(x - b.cx);
        const float ey = std::fabs(y - b.cy);
        if (ex > b.hw || ey > b.hh) {
            return false;
        }
        // Distance from the inner rect's edge: only the corner regions need
        // the radial test.
        const float dx = ex - (b.hw - b.r) > 0.0f ? ex - (b.hw - b.r) : 0.0f;
        const float dy = ey - (b.hh - b.r) > 0.0f ? ey - (b.hh - b.r) : 0.0f;
        return dx * dx + dy * dy <= b.r * b.r;
    }

    void HandleMenuTap(float x, float y)
    {
        SDL_Log("SDL3_GLES stage=menu_tap x=%.0f y=%.0f screen=%d win=%ux%u",
            x, y, static_cast<int>(uiScreen_), width_, height_);
        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            if (HitRect(x, y, MenuButtonLayout(0))) {
                ResetGameplayScene();
                SDL_Log("SDL3_GLES stage=player_retry");
            } else if (HitRect(x, y, MenuButtonLayout(5))) {
                uiScreen_ = rhi::UiScreen::MainMenu;
                SDL_Log("SDL3_GLES stage=menu_open_from_death");
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            // Gameplay exposes one larger SET button plus the BAG shortcut.
            // Returning to the title is intentionally handled inside the
            // settings sheet.
            if (HitRect(x, y, MenuButtonLayout(0))) {
                uiScreen_ = rhi::UiScreen::GameplaySettings;
                SDL_Log("SDL3_GLES stage=gameplay_settings_open");
            } else if (HitRect(x, y, MenuButtonLayout(1))) {
                uiScreen_ = rhi::UiScreen::GameplayInventory;
                SDL_Log("SDL3_GLES stage=inventory_open");
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            if (HitRect(x, y, MenuButtonLayout(0))) {
                uiScreen_ = rhi::UiScreen::Gameplay;
                SDL_Log("SDL3_GLES stage=inventory_close");
                return;
            }
            for (int slot = 1; slot <= 3; ++slot) {
                if (HitRect(x, y, MenuButtonLayout(slot))) {
                    inventory_.TapEquip(slot - 1);
                    SDL_Log("SDL3_GLES stage=inventory_unequip slot=%d", slot - 1);
                    return;
                }
            }
            for (int cell = 0; cell < inventory::kBagCapacity; ++cell) {
                if (!HitRect(x, y, MenuButtonLayout(10 + cell))) {
                    continue;
                }
                if (inventory_.DefOf(cell) != nullptr) {
                    inventory_.TapBag(cell);
                    SDL_Log("SDL3_GLES stage=inventory_bag_tap cell=%d", cell);
                }
                return;
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::GameplaySettings) {
            for (int index = 0; index <= 5; ++index) {
                if (!HitRect(x, y, MenuButtonLayout(index))) {
                    continue;
                }
                switch (index) {
                case 0:
                    uiScreen_ = rhi::UiScreen::Gameplay;
                    SDL_Log("SDL3_GLES stage=gameplay_settings_close");
                    break;
                case 1:
                    settingFps_ = !settingFps_;
                    SDL_Log("SDL3_GLES stage=setting_fps on=%d", settingFps_ ? 1 : 0);
                    break;
                case 2:
                    bloomEnabled_ = !bloomEnabled_;
                    SDL_Log("SDL3_GLES stage=setting_bloom on=%d", bloomEnabled_ ? 1 : 0);
                    break;
                case 3:
                    resIndex_ = (resIndex_ + 1) % 4;
                    renderScale_ = kScaleChoices[resIndex_];
                    DestroyGBufferTarget();
                    DestroyPostTarget();
                    SDL_Log("SDL3_GLES stage=setting_res scale=%.2f", renderScale_);
                    break;
                case 4:
                    uiOpacityIndex_ = (uiOpacityIndex_ + 1) % 5;
                    uiOpacity_ = kOpacityChoices[uiOpacityIndex_];
                    SDL_Log("SDL3_GLES stage=setting_ui_opacity a=%.2f", uiOpacity_);
                    break;
                case 5:
                    uiScreen_ = rhi::UiScreen::MainMenu;
                    SDL_Log("SDL3_GLES stage=menu_open_from_gameplay_settings");
                    break;
                default:
                    break;
                }
                break;
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::MainMenu) {
            if (HitRect(x, y, MenuButtonLayout(0))) {
                ResetGameplayScene();
                SDL_Log("SDL3_GLES stage=menu_start");
            } else if (HitRect(x, y, MenuButtonLayout(1))) {
                uiScreen_ = rhi::UiScreen::Settings;
                SDL_Log("SDL3_GLES stage=menu_settings");
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::Settings) {
            // Hit-test every settings button; the first hit handles the tap.
            for (int index = 0; index < 6; ++index) {
                if (!HitRect(x, y, MenuButtonLayout(index))) {
                    continue;
                }
                switch (index) {
                case 0:
                    settingFps_ = !settingFps_;
                    SDL_Log("SDL3_GLES stage=setting_fps on=%d", settingFps_ ? 1 : 0);
                    break;
                case 1:
                    sensIndex_ = (sensIndex_ + 1) % 3;
                    lookSensitivityScale_ = kSensitivityChoices[sensIndex_];
                    SDL_Log("SDL3_GLES stage=setting_sens scale=%.1f", lookSensitivityScale_);
                    break;
                case 2:
                    bloomEnabled_ = !bloomEnabled_;
                    SDL_Log("SDL3_GLES stage=setting_bloom on=%d", bloomEnabled_ ? 1 : 0);
                    break;
                case 3:
                    resIndex_ = (resIndex_ + 1) % 4;
                    renderScale_ = kScaleChoices[resIndex_];
                    // The menu tap is consumed before rendering; rebuild lazily
                    // at the selected scale before this frame's skybox pass.
                    DestroyGBufferTarget();
                    DestroyPostTarget();
                    SDL_Log("SDL3_GLES stage=setting_res scale=%.2f", renderScale_);
                    break;
                case 4:
                    uiOpacityIndex_ = (uiOpacityIndex_ + 1) % 5;
                    uiOpacity_ = kOpacityChoices[uiOpacityIndex_];
                    SDL_Log("SDL3_GLES stage=setting_ui_opacity a=%.2f", uiOpacity_);
                    break;
                default:
                    uiScreen_ = rhi::UiScreen::MainMenu;
                    SDL_Log("SDL3_GLES stage=menu_back");
                    break;
                }
                break;  // one button per tap
            }
        }
    }

    // Draws one text run centred on (cx, cy), with the same soft shadow the
    // action-button labels use.  DrawTextPx's origin is the glyph top-left.
    void DrawTextCentered(const char* text, float cx, float cy, float emPx,
        const float color[4], bool drawShadow = true)
    {
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        float advanceSum = 0.0f;
        for (const char* p = text; *p != '\0';) {
            const unsigned int codepoint = rhi::SdfNextCodepoint(&p);
            const rhi::SdfGlyph* glyph = rhi::SdfFindGlyph(codepoint);
            advanceSum += (glyph != nullptr && glyph->w > 0.0f)
                ? glyph->advance : static_cast<float>(rhi::kSdfFontEm) * 0.30f;
        }
        const float textW = advanceSum * scale;
        const float textH = 45.0f * scale;
        const float x = cx - textW * 0.5f;
        const float y = cy - textH * 0.5f;
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.60f};
        if (drawShadow) {
            DrawTextPx(text, x + 2.0f, y + 2.0f, emPx, shadow);
        }
        DrawTextPx(text, x, y, emPx, color);
    }

    void DrawTextLeftAligned(const char* text, float leftX, float cy, float emPx,
        const float color[4], bool drawShadow = true)
    {
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        const float y = cy - 22.5f * scale;
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.55f};
        if (drawShadow) {
            DrawTextPx(text, leftX + 2.0f, y + 2.0f, emPx, shadow);
        }
        DrawTextPx(text, leftX, y, emPx, color);
    }

    void DrawMenuCircle(const MenuButtonPos& b, const float color[4])
    {
        glUseProgram(overlayProgram_);
        glUniform2f(overlayResolutionLocation_, static_cast<float>(width_),
            static_cast<float>(height_));
        glBindVertexArray(overlayVao_);
        glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
        UploadCircleFan(b.cx, b.cy, b.r);
        glUniform4f(overlayColorLocation_, color[0], color[1], color[2], color[3]);
        glDrawArrays(GL_TRIANGLE_FAN, 0, kOverlayFanCount);
    }

    // Rounded-rectangle fill as one non-overlapping triangle list: 3 straight
    // strips + 4 quarter-disc corner fans.  Non-overlap matters -- buttons are
    // semi-transparent, so any double-covered pixel would blend twice and show
    // as a seam.
    static constexpr int kRectCornerSegments = 12;

    void UploadRoundedRect(const MenuButtonPos& b)
    {
        const float x0 = b.cx - b.hw;
        const float x1 = b.cx + b.hw;
        const float y0 = b.cy - b.hh;
        const float y1 = b.cy + b.hh;
        const float r = b.r;
        float v[3 * (6 + 6 + 6 + 4 * kRectCornerSegments * 3)];
        int n = 0;
        const auto pushQuad = [&](float ax, float ay, float bx, float by) {
            v[n++] = ax; v[n++] = ay;  // triangle 1
            v[n++] = bx; v[n++] = ay;
            v[n++] = bx; v[n++] = by;
            v[n++] = ax; v[n++] = ay;  // triangle 2
            v[n++] = bx; v[n++] = by;
            v[n++] = ax; v[n++] = by;
        };
        const auto pushCorner = [&](float ccx, float ccy, float a0, float a1) {
            for (int i = 0; i < kRectCornerSegments; ++i) {
                const float t0 = a0 + (a1 - a0) * static_cast<float>(i) /
                    static_cast<float>(kRectCornerSegments);
                const float t1 = a0 + (a1 - a0) * static_cast<float>(i + 1) /
                    static_cast<float>(kRectCornerSegments);
                v[n++] = ccx; v[n++] = ccy;
                v[n++] = ccx + std::cos(t0) * r; v[n++] = ccy + std::sin(t0) * r;
                v[n++] = ccx + std::cos(t1) * r; v[n++] = ccy + std::sin(t1) * r;
            }
        };
        // Straight regions (window coords, y down).
        pushQuad(x0 + r, y0, x1 - r, y1);            // vertical strip
        pushQuad(x0, y0 + r, x0 + r, y1 - r);        // left strip
        pushQuad(x1 - r, y0 + r, x1, y1 - r);        // right strip
        // Quarter discs.
        pushCorner(x0 + r, y0 + r, 3.14159265f, 4.71238898f);   // top-left
        pushCorner(x1 - r, y0 + r, 4.71238898f, 6.28318531f);   // top-right
        pushCorner(x1 - r, y1 - r, 0.0f, 1.57079633f);          // bottom-right
        pushCorner(x0 + r, y1 - r, 1.57079633f, 3.14159265f);   // bottom-left
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(float)),
            v, GL_DYNAMIC_DRAW);
        rectIndexCount_ = n / 2;
    }

    void DrawMenuRect(const MenuButtonPos& b, const float color[4])
    {
        glUseProgram(overlayProgram_);
        glUniform2f(overlayResolutionLocation_, static_cast<float>(width_),
            static_cast<float>(height_));
        glBindVertexArray(overlayVao_);
        glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
        UploadRoundedRect(b);
        glUniform4f(overlayColorLocation_, color[0], color[1], color[2],
            color[3] * uiOpacity_);
        glDrawArrays(GL_TRIANGLES, 0, rectIndexCount_);
    }

    void DrawActionIcon(int index, float centerX, float centerY, float radius, bool held)
    {
        if (iconProgram_ == 0 || iconVao_ == 0 || iconVbo_ == 0 || index < 0 ||
            index >= rhi::CameraInput::kActionButtonCount || actionIconTextures_[index] == 0) {
            return;
        }
        // Match MikanEngine's iconSize = radius * 0.78f.  The source PNGs
        // already contain transparent padding, so this keeps the glyph clear
        // of the circular face while leaving the hit area unchanged.
        const float halfSize = radius * 0.39f;
        const float vertices[6][4] = {
            {centerX - halfSize, centerY - halfSize, 0.0f, 0.0f},
            {centerX - halfSize, centerY + halfSize, 0.0f, 1.0f},
            {centerX + halfSize, centerY + halfSize, 1.0f, 1.0f},
            {centerX - halfSize, centerY - halfSize, 0.0f, 0.0f},
            {centerX + halfSize, centerY + halfSize, 1.0f, 1.0f},
            {centerX + halfSize, centerY - halfSize, 1.0f, 0.0f},
        };
        glUseProgram(iconProgram_);
        glUniform2f(iconResolutionLocation_, static_cast<float>(width_),
            static_cast<float>(height_));
        glUniform4f(iconColorLocation_, held ? 1.0f : 0.94f, held ? 0.92f : 0.95f,
            held ? 0.68f : 0.97f, (held ? 0.98f : 0.92f) * uiOpacity_);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(iconSamplerLocation_, 0);
        glBindTexture(GL_TEXTURE_2D, actionIconTextures_[index]);
        glBindVertexArray(iconVao_);
        glBindBuffer(GL_ARRAY_BUFFER, iconVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Mikan hit feedback: brief full-screen red flash, alpha ramps 1 -> 0
    // over kHitFlashDuration and follows the global UI opacity.
    void DrawHitFlash()
    {
        if (playerHitFlashTimer_ <= 0.0f) {
            return;
        }
        const float flash = std::min(playerHitFlashTimer_ / kHitFlashDuration, 1.0f);
        const float flashColor[4] = {0.92f, 0.05f, 0.03f, 0.28f * flash * uiOpacity_};
        const MenuButtonPos fullscreen = {width_ * 0.5f, height_ * 0.5f,
            width_ * 0.5f + 1.0f, height_ * 0.5f + 1.0f, 0.0f};
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        DrawMenuRect(fullscreen, flashColor);
    }

    void DrawPlayerHealthBar()
    {
        if (overlayProgram_ == 0 || uiScreen_ != rhi::UiScreen::Gameplay) {
            return;
        }
        const float w = static_cast<float>(width_);
        const float h = static_cast<float>(height_);
        const float mn = w < h ? w : h;
        const float barHalfWidth = mn * 0.145f;
        const float barHalfHeight = mn * 0.018f;
        const float barCenterX = mn * 0.255f;
        const float barCenterY = h * 0.090f;
        const float radius = mn * 0.012f;
        const float ratio = std::max(0.0f, std::min(1.0f,
            playerMaxHealth_ > 0.0f ? playerHealth_ / playerMaxHealth_ : 0.0f));
        const MenuButtonPos frame = {barCenterX, barCenterY,
            barHalfWidth, barHalfHeight, radius};
        const float frameColor[4] = {0.035f, 0.045f, 0.060f, 0.86f};
        const float healthGreen[4] = {0.20f, 0.82f, 0.32f, 0.94f};
        const float healthAmber[4] = {0.96f, 0.64f, 0.16f, 0.94f};
        const float healthRed[4] = {0.92f, 0.18f, 0.18f, 0.94f};
        const float* fillColor = ratio > 0.5f ? healthGreen
            : (ratio > 0.25f ? healthAmber : healthRed);
        const float white[4] = {1.0f, 1.0f, 1.0f, 0.92f};

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        DrawMenuRect(frame, frameColor);
        if (ratio > 0.0f) {
            const float fillHalfWidth = std::max(0.0f, barHalfWidth - mn * 0.004f) * ratio;
            const MenuButtonPos fill = {
                barCenterX - barHalfWidth + mn * 0.004f + fillHalfWidth,
                barCenterY, fillHalfWidth, std::max(0.0f, barHalfHeight - mn * 0.004f),
                std::max(0.0f, radius - mn * 0.003f)};
            DrawMenuRect(fill, fillColor);
        }
        char label[32];
        std::snprintf(label, sizeof(label), "血量 %.0f/%.0f", playerHealth_, playerMaxHealth_);
        DrawTextCentered(label, barCenterX, barCenterY, mn * 0.030f, white);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }

    void DrawEnemyHealthBar()
    {
        if (overlayProgram_ == 0 || uiScreen_ != rhi::UiScreen::Gameplay || !enemyAlive_) {
            return;
        }
        const float w = static_cast<float>(width_);
        const float h = static_cast<float>(height_);
        const float mn = w < h ? w : h;
        const float localHead[3] = {0.0f, 1.28f, 0.0f};
        const float worldHead[3] = {
            enemyMatrix_.value[0] * localHead[0] + enemyMatrix_.value[4] * localHead[1]
                + enemyMatrix_.value[8] * localHead[2] + enemyMatrix_.value[12],
            enemyMatrix_.value[1] * localHead[0] + enemyMatrix_.value[5] * localHead[1]
                + enemyMatrix_.value[9] * localHead[2] + enemyMatrix_.value[13],
            enemyMatrix_.value[2] * localHead[0] + enemyMatrix_.value[6] * localHead[1]
                + enemyMatrix_.value[10] * localHead[2] + enemyMatrix_.value[14]};
        float centerX = 0.0f;
        float centerY = 0.0f;
        if (!ProjectWorldToScreen(viewProj_, worldHead[0], worldHead[1], worldHead[2],
                w, h, &centerX, &centerY)) {
            return;
        }
        const float barHalfWidth = mn * 0.090f;
        const float barHalfHeight = mn * 0.010f;
        const float barCenterY = centerY - mn * 0.035f;
        const float radius = mn * 0.007f;
        const float ratio = std::max(0.0f, std::min(1.0f,
            enemyMaxHealth_ > 0.0f ? enemyHealth_ / enemyMaxHealth_ : 0.0f));
        const MenuButtonPos frame = {centerX, barCenterY,
            barHalfWidth, barHalfHeight, radius};
        const float frameColor[4] = {0.035f, 0.045f, 0.060f, 0.90f};
        const float healthRed[4] = {0.92f, 0.18f, 0.18f, 0.96f};
        const float white[4] = {1.0f, 1.0f, 1.0f, 0.92f};
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        DrawMenuRect(frame, frameColor);
        if (ratio > 0.0f) {
            const float fillHalfWidth = std::max(0.0f, barHalfWidth - mn * 0.0025f) * ratio;
            const MenuButtonPos fill = {
                centerX - barHalfWidth + mn * 0.0025f + fillHalfWidth, barCenterY,
                fillHalfWidth, std::max(0.0f, barHalfHeight - mn * 0.0025f),
                std::max(0.0f, radius - mn * 0.0015f)};
            DrawMenuRect(fill, healthRed);
        }
        char label[32];
        std::snprintf(label, sizeof(label), "敌人 %.0f/%.0f", enemyHealth_, enemyMaxHealth_);
        DrawTextCentered(label, centerX, barCenterY - mn * 0.022f, mn * 0.020f, white);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }

    void DrawMenuOverlay()
    {
        if (overlayProgram_ == 0) {
            return;
        }
        // Same overlay state as DrawJoystickOverlay: the FXAA pass hands over
        // with blending disabled, so without this the dim wash paints opaque
        // (black screen) and glyph quads draw as solid boxes.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        const float w = static_cast<float>(width_);
        const float h = static_cast<float>(height_);
        const float mn = w < h ? w : h;
        const float white[4] = {1.0f, 1.0f, 1.0f, 0.92f};
        const float panel[4] = {0.30f, 0.32f, 0.36f, 0.55f};
        const float accent[4] = {0.20f, 0.48f, 0.66f, 1.0f};
        // Main-menu copy sits directly over the skybox; the settings branches
        // below add only a light wash so the animated environment remains visible.

        if (uiScreen_ == rhi::UiScreen::MainMenu) {
            DrawTextCentered("MIKAN", w * 0.5f, h * 0.20f, mn * 0.105f, white);
            DrawTextCentered("SDL3 鸿蒙运行时", w * 0.5f, h * 0.30f,
                mn * 0.030f, white);
            const MenuButtonPos start = MenuButtonLayout(0);
            DrawMenuRect(start, accent);
            DrawTextCentered("开始游戏", start.cx, start.cy,
                start.hh * 0.92f, white);
            const MenuButtonPos settings = MenuButtonLayout(1);
            DrawMenuRect(settings, panel);
            DrawTextCentered("设置", settings.cx, settings.cy,
                settings.hh * 0.85f, white);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            const MenuButtonPos dim = {w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f};
            const float dimColor[4] = {0.005f, 0.008f, 0.015f, 0.72f};
            const float danger[4] = {0.72f, 0.16f, 0.12f, 0.88f};
            DrawMenuRect(dim, dimColor);
            DrawTextCentered("你死了", w * 0.5f, h * 0.36f, mn * 0.085f, white);
            const MenuButtonPos retry = MenuButtonLayout(0);
            DrawMenuRect(retry, danger);
            DrawTextCentered("重试", retry.cx, retry.cy, retry.hh * 0.88f, white);
            const MenuButtonPos menu = MenuButtonLayout(5);
            DrawMenuRect(menu, panel);
            DrawTextCentered("菜单", menu.cx, menu.cy, menu.hh * 0.52f, white);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            // Same glass-card language as the settings sheet: light panel over
            // the frozen live scene, soft control pills, dark ink labels.
            const MenuButtonPos dim = {w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f};
            const float dimColor[4] = {0.015f, 0.035f, 0.055f, 0.20f};
            const MenuButtonPos sheet = {w * 0.5f, h * 0.50f, mn * 0.46f, mn * 0.40f,
                mn * 0.018f};
            const float sheetColor[4] = {0.94f, 0.97f, 0.99f, 0.84f};
            const float slotColor[4] = {0.72f, 0.82f, 0.90f, 0.78f};
            const float emptyColor[4] = {0.96f, 0.98f, 1.0f, 0.30f};
            const float activeColor[4] = {0.20f, 0.48f, 0.66f, 0.94f};
            const float ink[4] = {0.08f, 0.14f, 0.20f, 0.98f};
            const float secondaryInk[4] = {0.18f, 0.26f, 0.34f, 0.98f};
            const float labelX = w * 0.5f - mn * 0.40f;
            DrawMenuRect(dim, dimColor);
            DrawMenuRect(sheet, sheetColor);
            DrawTextLeftAligned("背包", labelX, h * 0.175f, mn * 0.050f, ink, false);
            DrawTextLeftAligned("装备", w * 0.5f - mn * 0.335f - mn * 0.068f,
                h * 0.245f, mn * 0.032f, secondaryInk, false);
            DrawTextLeftAligned("物品", w * 0.5f - mn * 0.135f,
                h * 0.245f, mn * 0.032f, secondaryInk, false);
            const MenuButtonPos close = MenuButtonLayout(0);
            DrawMenuRect(close, slotColor);
            DrawTextCentered("X", close.cx, close.cy, close.hh * 0.90f, ink, false);

            static const char* const kSlotNames[3] = {"武器", "头盔", "护甲"};
            for (int slot = 1; slot <= 3; ++slot) {
                const MenuButtonPos slotPos = MenuButtonLayout(slot);
                const int bagIndex = inventory_.equipped[slot - 1];
                const inventory::ItemDef* def = inventory_.DefOf(bagIndex);
                DrawMenuRect(slotPos, def != nullptr ? activeColor : emptyColor);
                if (def != nullptr) {
                    const MenuButtonPos icon = {slotPos.cx, slotPos.cy - mn * 0.022f,
                        mn * 0.030f, mn * 0.030f, mn * 0.008f};
                    DrawMenuRect(icon, def->color);
                    DrawTextCentered(def->name, slotPos.cx, slotPos.cy + mn * 0.040f,
                        mn * 0.024f, ink, false);
                } else {
                    DrawTextCentered("空", slotPos.cx, slotPos.cy, mn * 0.030f,
                        secondaryInk, false);
                }
                DrawTextCentered(kSlotNames[slot - 1], slotPos.cx,
                    slotPos.cy + mn * 0.095f, mn * 0.026f, secondaryInk, false);
            }

            for (int cell = 0; cell < inventory::kBagCapacity; ++cell) {
                const MenuButtonPos cellPos = MenuButtonLayout(10 + cell);
                const inventory::ItemDef* def = inventory_.DefOf(cell);
                DrawMenuRect(cellPos, def != nullptr ? slotColor : emptyColor);
                if (def != nullptr) {
                    const MenuButtonPos icon = {cellPos.cx, cellPos.cy - mn * 0.014f,
                        mn * 0.020f, mn * 0.020f, mn * 0.006f};
                    DrawMenuRect(icon, def->color);
                    DrawTextCentered(def->name, cellPos.cx, cellPos.cy + mn * 0.022f,
                        mn * 0.019f, ink, false);
                    if (inventory::kItemDefs[inventory_.bag[cell].defId].maxCount > 1) {
                        char countLabel[8];
                        snprintf(countLabel, sizeof(countLabel), "x%d",
                            inventory_.bag[cell].count);
                        DrawTextCentered(countLabel, cellPos.cx + mn * 0.024f,
                            cellPos.cy + mn * 0.036f, mn * 0.016f, secondaryInk, false);
                    }
                }
            }
            DrawTextCentered("轻点物品装备 轻点装备栏卸下", w * 0.5f, h * 0.82f,
                mn * 0.026f, secondaryInk, false);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplaySettings) {
            // Keep the live scene visible behind a dimmed, touch-friendly
            // sheet, matching MikanEngine's runtime settings overlay instead
            // of sending the player back through the main menu.
            const MenuButtonPos dim = {w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f};
            const float dimColor[4] = {0.015f, 0.035f, 0.055f, 0.20f};
            const MenuButtonPos sheet = {w * 0.5f, h * 0.50f, mn * 0.34f, mn * 0.34f,
                mn * 0.018f};
            const float sheetColor[4] = {0.94f, 0.97f, 0.99f, 0.84f};
            const float controlColor[4] = {0.72f, 0.82f, 0.90f, 0.78f};
            const float activeColor[4] = {0.20f, 0.48f, 0.66f, 0.94f};
            const float dividerColor[4] = {0.22f, 0.34f, 0.44f, 0.28f};
            const float labelColor[4] = {0.12f, 0.19f, 0.27f, 0.98f};
            const float ink[4] = {0.08f, 0.14f, 0.20f, 0.98f};
            const float labelX = w * 0.5f - mn * 0.27f;
            DrawMenuRect(dim, dimColor);
            DrawMenuRect(sheet, sheetColor);
            DrawTextLeftAligned("显示设置", labelX, h * 0.275f, mn * 0.050f, ink, false);
            const MenuButtonPos close = MenuButtonLayout(0);
            DrawMenuRect(close, controlColor);
            DrawTextCentered("X", close.cx, close.cy, close.hh * 0.90f, ink, false);

            const float dividerY[5] = {0.31f, 0.41f, 0.51f, 0.61f, 0.71f};
            for (float rowY : dividerY) {
                DrawMenuRect({w * 0.5f, h * rowY, mn * 0.27f, mn * 0.0012f, 0.0f},
                    dividerColor);
            }

            const MenuButtonPos fps = MenuButtonLayout(1);
            const float* fpsColor = settingFps_ ? activeColor : controlColor;
            const float* fpsInk = settingFps_ ? white : ink;
            DrawMenuRect(fps, fpsColor);
            DrawTextCentered(settingFps_ ? "开" : "关", fps.cx, fps.cy,
                fps.hh * 0.85f, fpsInk, false);
            DrawTextLeftAligned("调试面板", labelX, fps.cy, mn * 0.030f, labelColor, false);

            const MenuButtonPos bloom = MenuButtonLayout(2);
            const float* bloomColor = bloomEnabled_ ? activeColor : controlColor;
            const float* bloomInk = bloomEnabled_ ? white : ink;
            DrawMenuRect(bloom, bloomColor);
            DrawTextCentered(bloomEnabled_ ? "开" : "关", bloom.cx, bloom.cy,
                bloom.hh * 0.85f, bloomInk, false);
            DrawTextLeftAligned("泛光", labelX, bloom.cy, mn * 0.030f, labelColor, false);

            const MenuButtonPos res = MenuButtonLayout(3);
            DrawMenuRect(res, controlColor);
            char resLabel[8];
            snprintf(resLabel, sizeof(resLabel), "x%.2f", renderScale_);
            DrawTextCentered(resLabel, res.cx, res.cy, res.hh * 0.85f, ink, false);
            DrawTextLeftAligned("渲染分辨率", labelX, res.cy, mn * 0.030f, labelColor, false);

            const MenuButtonPos opacity = MenuButtonLayout(4);
            DrawMenuRect(opacity, controlColor);
            char opacityLabel[8];
            snprintf(opacityLabel, sizeof(opacityLabel), "%.0f%%", uiOpacity_ * 100.0f);
            DrawTextCentered(opacityLabel, opacity.cx, opacity.cy,
                opacity.hh * 0.85f, ink, false);
            DrawTextLeftAligned("UI透明度", labelX, opacity.cy, mn * 0.030f,
                labelColor, false);

            const MenuButtonPos title = MenuButtonLayout(5);
            DrawMenuRect(title, accent);
            DrawTextCentered("返回游戏标题", title.cx, title.cy, mn * 0.032f, white);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            return;
        }

        // Standalone settings screen: a light translucent glass card over the
        // same slowly orbiting skybox used by the main menu.
        const float settingsBackground[4] = {0.015f, 0.035f, 0.055f, 0.20f};
        const float settingsSheet[4] = {0.94f, 0.97f, 0.99f, 0.84f};
        const float rowColor[4] = {0.96f, 0.98f, 1.0f, 0.30f};
        const float controlColor[4] = {0.72f, 0.82f, 0.90f, 0.78f};
        const float activeColor[4] = {0.20f, 0.48f, 0.66f, 0.94f};
        const float ink[4] = {0.08f, 0.14f, 0.20f, 0.98f};
        const float secondaryInk[4] = {0.18f, 0.26f, 0.34f, 0.98f};
        const float dividerColor[4] = {0.22f, 0.34f, 0.44f, 0.28f};
        const float leftX = w * 0.5f - mn * 0.35f;
        DrawMenuRect({w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f},
            settingsBackground);
        DrawMenuRect({w * 0.5f, h * 0.5f, mn * 0.43f, mn * 0.39f, mn * 0.020f},
            settingsSheet);
        DrawTextLeftAligned("设置", leftX, h * 0.22f, mn * 0.050f, ink, false);
        const MenuButtonPos back = MenuButtonLayout(5);
        DrawMenuRect(back, rowColor);
        DrawTextCentered("X", back.cx, back.cy, back.hh * 0.82f, ink, false);
        DrawMenuRect({w * 0.5f, h * 0.30f, mn * 0.37f, mn * 0.001f, 0.0f},
            dividerColor);

        static const char* const kLabels[5] = {
            "调试面板", "视角灵敏度", "泛光", "渲染分辨率", "UI透明度"};
        for (int index = 0; index < 5; ++index) {
            const MenuButtonPos control = MenuButtonLayout(index);
            const MenuButtonPos row = {w * 0.5f, control.cy,
                mn * 0.40f, mn * 0.045f, mn * 0.008f};
            DrawMenuRect(row, rowColor);
            const bool isToggle = index == 0 || index == 2;
            const bool enabled = index == 0 ? settingFps_ : bloomEnabled_;
            const float* valueColor = isToggle && enabled ? activeColor : controlColor;
            const float* valueInk = isToggle && enabled ? white : ink;
            DrawMenuRect(control, valueColor);
            DrawTextLeftAligned(kLabels[index], leftX, control.cy, mn * 0.032f,
                secondaryInk, false);

            char value[16]{};
            if (index == 0) {
                snprintf(value, sizeof(value), "%s", settingFps_ ? "开" : "关");
            } else if (index == 1) {
                snprintf(value, sizeof(value), "%.1f", lookSensitivityScale_);
            } else if (index == 2) {
                snprintf(value, sizeof(value), "%s", bloomEnabled_ ? "开" : "关");
            } else if (index == 3) {
                snprintf(value, sizeof(value), "%.0f%%", renderScale_ * 100.0f);
            } else {
                snprintf(value, sizeof(value), "%.0f%%", uiOpacity_ * 100.0f);
            }
            DrawTextCentered(value, control.cx, control.cy, control.hh * 0.84f,
                valueInk, false);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }

    void DrawJoystickOverlay()
    {
        if (!cameraInput_.showOverlay || overlayProgram_ == 0) {
            return;
        }
        glUseProgram(overlayProgram_);
        glUniform2f(overlayResolutionLocation_, static_cast<float>(width_),
            static_cast<float>(height_));
        glBindVertexArray(overlayVao_);
        glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        UploadCircleFan(cameraInput_.stickBaseX, cameraInput_.stickBaseY, cameraInput_.stickRadius);
        glUniform4f(overlayColorLocation_, 0.30f, 0.32f, 0.36f, 0.40f * uiOpacity_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, kOverlayFanCount);
        UploadCircleFan(cameraInput_.stickKnobX, cameraInput_.stickKnobY, cameraInput_.knobRadius);
        if (cameraInput_.stickActive) {
            glUniform4f(overlayColorLocation_, 0.78f, 0.82f, 0.90f, 0.70f * uiOpacity_);
        } else {
            glUniform4f(overlayColorLocation_, 0.60f, 0.64f, 0.72f, 0.42f * uiOpacity_);
        }
        glDrawArrays(GL_TRIANGLE_FAN, 0, kOverlayFanCount);
        // Action buttons use the same MikanEngine order and anchors as the
        // touch layer: Attack, Jump, Sprint, Crouch.  One circle per button
        // with the same panel gray and base alpha as the 背包/设置 buttons,
        // so every on-screen control shares one opacity scale.
        for (int i = 0; i < rhi::CameraInput::kActionButtonCount; ++i) {
            glBindVertexArray(overlayVao_);
            glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
            UploadCircleFan(cameraInput_.actionButtonX[i], cameraInput_.actionButtonY[i],
                cameraInput_.actionButtonRadius + 5.0f);
            if (cameraInput_.actionButtonHeld[i]) {
                glUniform4f(overlayColorLocation_, 0.98f, 0.72f, 0.25f, 0.70f * uiOpacity_);
            } else {
                glUniform4f(overlayColorLocation_, 0.30f, 0.32f, 0.36f, 0.55f * uiOpacity_);
            }
            glDrawArrays(GL_TRIANGLE_FAN, 0, kOverlayFanCount);
            DrawActionIcon(i, cameraInput_.actionButtonX[i], cameraInput_.actionButtonY[i],
                cameraInput_.actionButtonRadius, cameraInput_.actionButtonHeld[i]);
            // DrawActionIcon switches programs; restore the flat overlay
            // before the next button updates its circle uniform/buffer.
            glUseProgram(overlayProgram_);
            glUniform2f(overlayResolutionLocation_, static_cast<float>(width_),
                static_cast<float>(height_));
            glBindVertexArray(overlayVao_);
            glBindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
        }
        DrawFpsText();
        if (settingFps_) {
            // Current animation state under the FPS counter.
            const float emPx = 58.0f;
            const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.65f};
            const float color[4] = {1.0f, 0.85f, 0.45f, 0.95f};
            const char* stateName = AnimStateHudName(animState_);
            DrawTextPx(stateName, 48.0f, 168.0f, emPx, shadow);
            DrawTextPx(stateName, 42.0f, 162.0f, emPx, color);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
        glUseProgram(0);
        LogGlErrors("draw_overlay");
    }

    // One draw call per string.  Vertices are window pixels (y down); UVs
    // address the ink sub-rect inside the glyph's atlas cell.
    void DrawTextPx(const char* text, float x, float y, float emPx, const float color[4])
    {
        if (textProgram_ == 0 || textAtlasTexture_ == 0 || text == nullptr) {
            return;
        }
        constexpr int kMaxQuads = 96;
        float vertices[kMaxQuads * 6 * 8];
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        const float atlasW = static_cast<float>(rhi::kSdfAtlasWidth);
        const float atlasH = static_cast<float>(rhi::kSdfAtlasHeight);
        const float pad = static_cast<float>(rhi::kSdfCellSize - rhi::kSdfFontEm) * 0.5f;
        float penX = x;
        int quadCount = 0;
        for (const char* p = text; *p != '\0' && quadCount < kMaxQuads;) {
            const unsigned int codepoint = rhi::SdfNextCodepoint(&p);
            const rhi::SdfGlyph* glyph = rhi::SdfFindGlyph(codepoint);
            if (glyph == nullptr || glyph->w <= 0.0f) {
                penX += emPx * 0.30f;  // unknown/space: coarse fixed advance
                continue;
            }
            const float x0 = penX + glyph->offsetX * scale;
            const float y0 = y + glyph->offsetY * scale;
            const float x1 = x0 + glyph->w * scale;
            const float y1 = y0 + glyph->h * scale;
            const float u0 = (static_cast<float>(glyph->cellX) + pad) / atlasW;
            const float v0 = (static_cast<float>(glyph->cellY) + pad) / atlasH;
            const float u1 = (static_cast<float>(glyph->cellX) + pad + glyph->w) / atlasW;
            const float v1 = (static_cast<float>(glyph->cellY) + pad + glyph->h) / atlasH;
            float* v = vertices + quadCount * 6 * 8;
            const float textAlpha = color[3] * uiOpacity_;
            const float quad[6][8] = {
                {x0, y0, u0, v0, color[0], color[1], color[2], textAlpha},
                {x0, y1, u0, v1, color[0], color[1], color[2], textAlpha},
                {x1, y1, u1, v1, color[0], color[1], color[2], textAlpha},
                {x0, y0, u0, v0, color[0], color[1], color[2], textAlpha},
                {x1, y1, u1, v1, color[0], color[1], color[2], textAlpha},
                {x1, y0, u1, v0, color[0], color[1], color[2], textAlpha},
            };
            for (int vertex = 0; vertex < 6; ++vertex) {
                for (int comp = 0; comp < 8; ++comp) {
                    v[vertex * 8 + comp] = quad[vertex][comp];
                }
            }
            penX += glyph->advance * scale;
            ++quadCount;
        }
        if (quadCount == 0) {
            return;
        }
        glUseProgram(textProgram_);
        glUniform2f(textResolutionLocation_, static_cast<float>(width_),
            static_cast<float>(height_));
        glUniform1f(textPxRangeLocation_, rhi::kSdfPxRange * scale);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(textSamplerLocation_, 0);
        glBindTexture(GL_TEXTURE_2D, textAtlasTexture_);
        glBindVertexArray(textVao_);
        glBindBuffer(GL_ARRAY_BUFFER, textVbo_);
        glBufferData(GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(quadCount) * 6 * 8 * sizeof(float), vertices,
            GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, quadCount * 6);
        glBindVertexArray(0);
        glUseProgram(0);
        LogGlErrors("draw_text");
    }

    void DrawFpsText()
    {
        // Settings toggle: the debug HUD (FPS + anim state text) is optional.
        if (!settingFps_ || fpsAvgMs_ <= 0.0f) {
            return;
        }
        char text[32];
        snprintf(text, sizeof(text), "FPS %.1f", 1000.0f / fpsAvgMs_);
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.65f};
        const float white[4] = {1.0f, 1.0f, 1.0f, 0.95f};
        constexpr float kFpsFontEm = 90.0f;  // 3x the original 30px size
        DrawTextPx(text, 46.0f, 38.0f, kFpsFontEm, shadow);
        DrawTextPx(text, 40.0f, 32.0f, kFpsFontEm, white);
    }

    bool DrawOnce() override
    {
        // Frame-time EMA for the FPS counter; updated before any early return
        // so skipped frames still keep the estimate moving.
        const Uint64 nowTicks = SDL_GetTicks();
        if (fpsLastTicks_ != 0) {
            const float dtMs = static_cast<float>(nowTicks - fpsLastTicks_);
            if (dtMs > 0.0f && dtMs < 1000.0f) {
                fpsAvgMs_ = fpsAvgMs_ <= 0.0f ? dtMs : fpsAvgMs_ * 0.9f + dtMs * 0.1f;
            }
        }
        fpsLastTicks_ = nowTicks;
        if (!sceneReady_ || context_ == nullptr || width_ == 0 || height_ == 0) {
            return false;
        }
        // Menu/settings taps: consume before anything else so a screen switch
        // (START/BACK) takes effect for this very frame's HUD selection.
        if (cameraInput_.menuTapEdge) {
            HandleMenuTap(cameraInput_.menuTapX, cameraInput_.menuTapY);
            cameraInput_.menuTapEdge = false;
        }
        if (!UpdateSceneUniforms()) {
            return false;
        }
        // Skeletal animation: sample + joint matrix upload before the
        // geometry pass consumes the skin UBO.
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            UpdateAnimation();
            UpdateEnemyAnimation();
        }
        const int characterInstanceCount = UploadCharacterInstanceData();

        // ---- single scene shadow map: depth-only, before the G-buffer ----
        // The light camera and receiver matrix are shared with Vulkan.  The
        // shadow pass includes the animated character and both static props;
        // only the key/direct term consumes this map in the lighting pass.
        const bool renderGameplayScene = uiScreen_ == rhi::UiScreen::Gameplay ||
            uiScreen_ == rhi::UiScreen::GameplaySettings ||
            uiScreen_ == rhi::UiScreen::GameplayRetry ||
            uiScreen_ == rhi::UiScreen::GameplayInventory;
        if (renderGameplayScene && shadowReady_ && shadowProgram_ != 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
            glViewport(0, 0, kShadowMapSize, kShadowMapSize);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(2.0f, 4.0f);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glClear(GL_DEPTH_BUFFER_BIT);
            glUseProgram(shadowProgram_);
            if (!meshes_.empty()) {
                DrawShadowCharacterBatch(meshes_, characterInstanceCount);
            }
            if (!helmetMeshes_.empty()) {
                const float helmetAngle = static_cast<float>(SDL_GetTicks() % 600000U)
                    * 0.001f * 0.8f + 0.5f;
                const Mat4 helmetTransform = Mat4Multiply(
                    Mat4Translation(physics::kHelmetCollider.centerX,
                        physics::kHelmetCollider.centerY,
                        physics::kHelmetCollider.centerZ),
                    Mat4Multiply(Mat4RotationY(helmetAngle),
                        Mat4Scale(physics::kHelmetCollider.renderScale)));
                DrawShadowMeshList(helmetMeshes_, false, helmetTransform);
            }
            if (!groundMeshes_.empty()) {
                const Mat4 groundTransform = Mat4Multiply(
                    Mat4Translation(physics::kGroundCollider.centerX,
                        physics::kGroundCollider.centerY,
                        physics::kGroundCollider.centerZ),
                    Mat4Scale3(physics::kGroundCollider.halfExtentX,
                        physics::kGroundCollider.halfExtentY,
                        physics::kGroundCollider.halfExtentZ));
                DrawShadowMeshList(groundMeshes_, false, groundTransform);
            }
            if (!groundMeshes_.empty() && sceneDefinition_ != nullptr) {
                for (const scene::Entity& entity : sceneDefinition_->entities) {
                    if (entity.importAsStaticCube && entity.castShadow) {
                        DrawShadowMeshList(groundMeshes_, false,
                            Mat4FromSceneEntity(*sceneDefinition_, entity));
                    }
                }
            }
            glUseProgram(0);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            LogGlErrors("shadow_pass");
        }

        // G-buffer runs at the global render resolution: created lazily once,
        // independent of the drawable size (see kRenderWidth/Height).
        if (gbufferFbo_ == 0) {
            if (!CreateGBufferTarget()) {
                return false;
            }
        }
        // Post-processing target (lighting writes here, FXAA reads it back).
        // Fixed resolution -- created once, no resize tracking.
        if (postFbo_ == 0) {
            if (!CreatePostTarget()) {
                return false;
            }
        }

        // ---- geometry pass: the model into the MRT G-buffer ----
        glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
        glViewport(0, 0, static_cast<GLsizei>(gbufferWidth_), static_cast<GLsizei>(gbufferHeight_));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (renderGameplayScene && !meshes_.empty()) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LESS);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
            glUseProgram(gbufferProgram_);
            glUniform3f(cameraPositionLocation_, camPosX_, camPosY_, camPosZ_);
            // Characters share one mesh/material residency and are submitted
            // as an animated instance batch. Static props switch the same
            // shader back to its per-draw SceneUniforms path below.
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(modelBaseColorSampler_, 0);
            glActiveTexture(GL_TEXTURE1);
            glUniform1i(modelMrSampler_, 1);
            glActiveTexture(GL_TEXTURE2);
            glUniform1i(modelNormalSampler_, 2);
            glActiveTexture(GL_TEXTURE3);
            glUniform1i(modelAoSampler_, 3);
            glActiveTexture(GL_TEXTURE4);
            glUniform1i(modelEmissiveSampler_, 4);
            DrawCharacterMeshBatch(meshes_, characterInstanceCount);

            // ---- static props: helmet + ground slab.  Same program,
            // skinning off, per-draw modelMvp/modelMatrix in the scene UBO
            // (offsets 128/192 = the two mat4 slots after cubeMvp/skyMvp).
            if (!helmetMeshes_.empty()) {
                // Turntable spin around its own base position: T * R(t) * S.
                // The 0.5 initial yaw from LoadHelmetModel becomes the phase.
                const float helmetAngle = static_cast<float>(SDL_GetTicks() % 600000U) * 0.001f * 0.8f + 0.5f;
                const Mat4 helmetSpin = Mat4Multiply(
                    Mat4Translation(physics::kHelmetCollider.centerX,
                        physics::kHelmetCollider.centerY,
                        physics::kHelmetCollider.centerZ),
                    Mat4Multiply(Mat4RotationY(helmetAngle),
                        Mat4Scale(physics::kHelmetCollider.renderScale)));
                DrawStaticMeshList(helmetMeshes_, helmetTextures_, helmetMaterials_, helmetSpin);
            }
            if (!groundMeshes_.empty()) {
                // Ground: the Base Model cube stretched into a slab.  Top
                // surface sits at the character's feet line (y = -0.94):
                // centre y = -0.94 - half thickness 0.2 = -1.14.
                const Mat4 groundTransform = Mat4Multiply(
                    Mat4Translation(physics::kGroundCollider.centerX,
                        physics::kGroundCollider.centerY,
                        physics::kGroundCollider.centerZ),
                    Mat4Scale3(physics::kGroundCollider.halfExtentX,
                        physics::kGroundCollider.halfExtentY,
                        physics::kGroundCollider.halfExtentZ));
                DrawStaticMeshList(groundMeshes_, groundTextures_, groundMaterials_,
                    groundTransform);
            }
            if (!groundMeshes_.empty() && sceneDefinition_ != nullptr) {
                for (const scene::Entity& entity : sceneDefinition_->entities) {
                    if (entity.importAsStaticCube) {
                        DrawStaticMeshList(groundMeshes_, groundTextures_, groundMaterials_,
                            Mat4FromSceneEntity(*sceneDefinition_, entity));
                    }
                }
            }

            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(0);
            glUseProgram(0);
            glDisable(GL_CULL_FACE);
            LogGlErrors("geometry_pass");
        }

        // ---- deferred lighting pass: G-buffer + IBL + sky -> post target ----
        // Rendered at the post target's (lower) resolution: all lighting
        // inputs are bound with normalized UVs, and the FXAA resolve
        // upscales to the screen.
        glBindFramebuffer(GL_FRAMEBUFFER, postFbo_);
        glViewport(0, 0, static_cast<GLsizei>(postWidth_), static_cast<GLsizei>(postHeight_));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(lightingProgram_);
        glUniformMatrix4fv(lightingInvViewProjLocation_, 1, GL_FALSE, invViewProj_.value);
        glUniform3f(lightingCameraPositionLocation_, camPosX_, camPosY_, camPosZ_);
        glUniform3f(lightingEmissiveFactorLocation_, lightingEmissive_[0], lightingEmissive_[1],
            lightingEmissive_[2]);
        glUniform1f(lightingMaxLodLocation_, skyboxMaxLod_);
        glUniform1i(lightingEnvPrefilteredLocation_, prefilteredTexture_ != 0 ? 1 : 0);
        glUniform1i(lightingUseBrdfLutLocation_, brdfLutTexture_ != 0 ? 1 : 0);
        glUniform1i(lightingUseShadowMapLocation_, shadowReady_ ? 1 : 0);
        glUniform1i(lightingDebugNormalLocation_, kDebugNormalMode);
        glUniform1i(lightingHdrOutputLocation_, hdrPipeline_ ? 1 : 0);
        glUniformMatrix4fv(lightingShadowMatrixLocation_, 1, GL_FALSE, shadowMatrix_.value);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(lightingAlbedoSampler_, 0);
        glBindTexture(GL_TEXTURE_2D, gbufferAlbedoTex_);
        glActiveTexture(GL_TEXTURE1);
        glUniform1i(lightingNormalSampler_, 1);
        glBindTexture(GL_TEXTURE_2D, gbufferNormalTex_);
        glActiveTexture(GL_TEXTURE2);
        glUniform1i(lightingMaterialSampler_, 2);
        glBindTexture(GL_TEXTURE_2D, gbufferMaterialTex_);
        glActiveTexture(GL_TEXTURE3);
        glUniform1i(lightingEmissiveSampler_, 3);
        glBindTexture(GL_TEXTURE_2D, gbufferEmissiveTex_);
        glActiveTexture(GL_TEXTURE4);
        glUniform1i(lightingDepthSampler_, 4);
        glBindTexture(GL_TEXTURE_2D, gbufferDepthTex_);
        glActiveTexture(GL_TEXTURE5);
        glUniform1i(lightingIrradianceSampler_, 5);
        glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceTexture_);
        glActiveTexture(GL_TEXTURE6);
        glUniform1i(lightingEnvSampler_, 6);
        // GGX-prefiltered chain when available; otherwise the raw skybox
        // mip chain is the (glossier) fallback.
        glBindTexture(GL_TEXTURE_CUBE_MAP,
            prefilteredTexture_ != 0 ? prefilteredTexture_ : skyboxTexture_);
        glActiveTexture(GL_TEXTURE7);
        glUniform1i(lightingBrdfLutSampler_, 7);
        glBindTexture(GL_TEXTURE_2D, brdfLutTexture_);
        glActiveTexture(GL_TEXTURE8);
        glUniform1i(lightingSkyboxSampler_, 8);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture_);
        glActiveTexture(GL_TEXTURE9);
        glUniform1i(lightingShadowSampler_, 9);
        glBindTexture(GL_TEXTURE_2D, shadowReady_ ? shadowDepthTexture_ : whiteTexture_);
        glBindVertexArray(fullscreenVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glUseProgram(0);
        LogGlErrors("lighting_pass");

        // ---- bloom chain (engine postprocess_chain_mobile order) ----
        // ds[0]: soft-knee threshold at half res; ds[1..5]: dual-kernel /2;
        // up[4..0]: dual-kernel x2 fused with the previous up level.  The
        // final up[0] (0.5x) is what the tonemap pass combines.
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(fullscreenVao_);
        if (bloomEnabled_) {
        glUseProgram(bloomThProgram_);
        if (hdrPipeline_) {
            glUniform2f(bloomThParams_, 1.0f, 0.5f);   // linear HDR: engine defaults
        } else {
            glUniform2f(bloomThParams_, 0.6f, 0.3f);   // gamma-space LDR approximation
        }
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(bloomThSampler_, 0);
        glBindTexture(GL_TEXTURE_2D, postColorTex_);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomDsFbo_[0]);
        glViewport(0, 0, bloomDsW_[0], bloomDsH_[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int i = 1; i < kBloomDsLevels; ++i) {
            glUseProgram(bloomDownProgram_);
            glUniform1i(bloomDownSampler_, 0);
            glBindTexture(GL_TEXTURE_2D, bloomDsTex_[i - 1]);
            glBindFramebuffer(GL_FRAMEBUFFER, bloomDsFbo_[i]);
            glViewport(0, 0, bloomDsW_[i], bloomDsH_[i]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        for (int i = kBloomUpLevels - 1; i >= 0; --i) {
            glUseProgram(bloomUpProgram_);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(bloomUpSampler_, 0);
            glBindTexture(GL_TEXTURE_2D, bloomDsTex_[i]);
            glActiveTexture(GL_TEXTURE1);
            glUniform1i(bloomUpPrevSampler_, 1);
            glBindTexture(GL_TEXTURE_2D,
                i == kBloomUpLevels - 1 ? bloomDsTex_[i + 1] : bloomUpTex_[i + 1]);
            glBindFramebuffer(GL_FRAMEBUFFER, bloomUpFbo_[i]);
            glViewport(0, 0, bloomDsW_[i], bloomDsH_[i]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(0);
        LogGlErrors("bloom_chain");
        }  // bloomEnabled_

        // ---- tonemap: composite + bloom -> AgX (HDR) or passthrough (LDR) ----
        glUseProgram(tonemapProgram_);
        glUniform1i(tonemapBloomSampler_, 0);
        // When the bloom chain is skipped, bloomUpTex_[0] has never been
        // rendered this frame -- its undefined float content can be NaN and
        // 0 * NaN poisons the composite, so bind a valid texture instead.
        glBindTexture(GL_TEXTURE_2D, bloomEnabled_ ? bloomUpTex_[0] : postColorTex_);
        glActiveTexture(GL_TEXTURE1);
        glUniform1i(tonemapCompSampler_, 1);
        glBindTexture(GL_TEXTURE_2D, postColorTex_);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(tonemapHdrMode_, hdrPipeline_ ? 1 : 0);
        // AgX exposure calibrated to the old look: this scene's linear values
        // were tuned for Reinhard * 1.8, which displays 0.18 mid grey at ~0.53
        // sRGB.  Inverting tonemap_agx (log midpoint -12.47..4.03, contrast
        // poly, ^2.4, sRGB EOTF) says 0.18 * 1.3 reproduces that display
        // level -- the engine's 7.0 assumes its sun*10 scene scale.
        glUniform1f(tonemapExposure_, 1.3f);
        glUniform1f(tonemapBloomStrength_, bloomEnabled_ ? kBloomStrength : 0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, tonemapFbo_);
        glViewport(0, 0, postWidth_, postHeight_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(0);
        glUseProgram(0);
        LogGlErrors("tonemap_pass");

        // ---- post-processing: FXAA resolves the LDR image to the screen ----
        // Runs in LDR/gamma space exactly like the engine chain (tonemap
        // happens in the lighting pass, FXAA only smooths edges).  The UI
        // overlay stays after this pass so text and the joystick keep their
        // crisp 1:1 pixels.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
        glUseProgram(fxaaProgram_);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(fxaaSampler_, 0);
        glBindTexture(GL_TEXTURE_2D, tonemapTex_);
        glBindVertexArray(fullscreenVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(0);
        glUseProgram(0);
        LogGlErrors("fxaa_pass");

        if (uiScreen_ == rhi::UiScreen::MainMenu ||
            uiScreen_ == rhi::UiScreen::Settings ||
            uiScreen_ == rhi::UiScreen::GameplaySettings ||
            uiScreen_ == rhi::UiScreen::GameplayRetry ||
            uiScreen_ == rhi::UiScreen::GameplayInventory) {
            DrawMenuOverlay();
        } else {
            DrawPlayerHealthBar();
            DrawEnemyHealthBar();
            DrawHitFlash();
            DrawJoystickOverlay();
            // One enlarged top-right SET button; title navigation is inside
            // the settings sheet so the gameplay corner stays uncluttered.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_DEPTH_TEST);
            const float mnF = static_cast<float>(width_ < height_ ? width_ : height_);
            const float panelF[4] = {0.30f, 0.32f, 0.36f, 0.55f};
            const float whiteF[4] = {1.0f, 1.0f, 1.0f, 0.92f};
            const MenuButtonPos setBtn = MenuButtonLayout(0);
            DrawMenuRect(setBtn, panelF);
            DrawTextCentered("设置", setBtn.cx, setBtn.cy, setBtn.hh * 0.58f,
                whiteF);
            const MenuButtonPos bagBtn = MenuButtonLayout(1);
            DrawMenuRect(bagBtn, panelF);
            DrawTextCentered("背包", bagBtn.cx, bagBtn.cy, bagBtn.hh * 0.52f,
                whiteF);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
        }

        SDL_GL_SwapWindow(window_);

        if (!firstFrameLogged_) {
            firstFrameLogged_ = true;
            SDL_Log("SDL3_GLES stage=scene_presented meshes=%zu skybox=%s", meshes_.size(),
                skyboxTexture_ != 0 ? "yes" : "no");
        }
        return true;
    }

    void Destroy() override
    {
        if (context_ == nullptr) {
            return;
        }
        for (const GlesMesh& mesh : meshes_) {
            if (mesh.vao != 0) {
                glDeleteVertexArrays(1, &mesh.vao);
            }
            if (mesh.vertexBuffer != 0) {
                glDeleteBuffers(1, &mesh.vertexBuffer);
            }
            if (mesh.indexBuffer != 0) {
                glDeleteBuffers(1, &mesh.indexBuffer);
            }
        }
        meshes_.clear();
        materials_.clear();
        if (whiteTexture_ != 0) {
            glDeleteTextures(1, &whiteTexture_);
            whiteTexture_ = 0;
        }
        if (neutralMrTexture_ != 0) {
            glDeleteTextures(1, &neutralMrTexture_);
            neutralMrTexture_ = 0;
        }
        if (!modelTextures_.empty()) {
            glDeleteTextures(static_cast<GLsizei>(modelTextures_.size()), modelTextures_.data());
            modelTextures_.clear();
        }
        if (!helmetTextures_.empty()) {
            glDeleteTextures(static_cast<GLsizei>(helmetTextures_.size()), helmetTextures_.data());
            helmetTextures_.clear();
        }
        for (const GlesMesh& mesh : helmetMeshes_) {
            if (mesh.vao != 0) {
                glDeleteVertexArrays(1, &mesh.vao);
            }
            if (mesh.vertexBuffer != 0) {
                glDeleteBuffers(1, &mesh.vertexBuffer);
            }
            if (mesh.indexBuffer != 0) {
                glDeleteBuffers(1, &mesh.indexBuffer);
            }
        }
        helmetMeshes_.clear();
        if (!groundTextures_.empty()) {
            glDeleteTextures(static_cast<GLsizei>(groundTextures_.size()), groundTextures_.data());
            groundTextures_.clear();
        }
        for (const GlesMesh& mesh : groundMeshes_) {
            if (mesh.vao != 0) {
                glDeleteVertexArrays(1, &mesh.vao);
            }
            if (mesh.vertexBuffer != 0) {
                glDeleteBuffers(1, &mesh.vertexBuffer);
            }
            if (mesh.indexBuffer != 0) {
                glDeleteBuffers(1, &mesh.indexBuffer);
            }
        }
        groundMeshes_.clear();
        // Alias check must run before skyboxTexture_ is deleted: on IBL
        // creation failure irradianceTexture_ points at the skybox texture.
        const bool irradianceAliasesSkybox = irradianceTexture_ != 0 && irradianceTexture_ == skyboxTexture_;
        if (irradianceTexture_ != 0 && !irradianceAliasesSkybox) {
            glDeleteTextures(1, &irradianceTexture_);
        }
        irradianceTexture_ = 0;
        if (prefilteredTexture_ != 0) {
            glDeleteTextures(1, &prefilteredTexture_);
            prefilteredTexture_ = 0;
        }
        if (brdfLutTexture_ != 0) {
            glDeleteTextures(1, &brdfLutTexture_);
            brdfLutTexture_ = 0;
        }
        if (overlayVbo_ != 0) {
            glDeleteBuffers(1, &overlayVbo_);
            overlayVbo_ = 0;
        }
        if (overlayVao_ != 0) {
            glDeleteVertexArrays(1, &overlayVao_);
            overlayVao_ = 0;
        }
        if (overlayProgram_ != 0) {
            glDeleteProgram(overlayProgram_);
            overlayProgram_ = 0;
        }
        for (GLuint& texture : actionIconTextures_) {
            if (texture != 0) {
                glDeleteTextures(1, &texture);
                texture = 0;
            }
        }
        if (iconVbo_ != 0) {
            glDeleteBuffers(1, &iconVbo_);
            iconVbo_ = 0;
        }
        if (iconVao_ != 0) {
            glDeleteVertexArrays(1, &iconVao_);
            iconVao_ = 0;
        }
        if (iconProgram_ != 0) {
            glDeleteProgram(iconProgram_);
            iconProgram_ = 0;
        }
        if (textVbo_ != 0) {
            glDeleteBuffers(1, &textVbo_);
            textVbo_ = 0;
        }
        if (textVao_ != 0) {
            glDeleteVertexArrays(1, &textVao_);
            textVao_ = 0;
        }
        if (textAtlasTexture_ != 0) {
            glDeleteTextures(1, &textAtlasTexture_);
            textAtlasTexture_ = 0;
        }
        if (textProgram_ != 0) {
            glDeleteProgram(textProgram_);
            textProgram_ = 0;
        }
        if (skyboxTexture_ != 0) {
            glDeleteTextures(1, &skyboxTexture_);
            skyboxTexture_ = 0;
        }
        if (skyboxVao_ != 0) {
            glDeleteVertexArrays(1, &skyboxVao_);
            skyboxVao_ = 0;
        }
        if (fullscreenVao_ != 0) {
            glDeleteVertexArrays(1, &fullscreenVao_);
            fullscreenVao_ = 0;
        }
        if (characterInstanceVbo_ != 0) {
            glDeleteBuffers(1, &characterInstanceVbo_);
            characterInstanceVbo_ = 0;
        }
        DestroyShadowMapTarget();
        DestroyGBufferTarget();
        if (uniformBuffer_ != 0) {
            glDeleteBuffers(1, &uniformBuffer_);
            uniformBuffer_ = 0;
        }
        if (jointBuffer_ != 0) {
            glDeleteBuffers(1, &jointBuffer_);
            jointBuffer_ = 0;
        }
        if (enemyJointBuffer_ != 0) {
            glDeleteBuffers(1, &enemyJointBuffer_);
            enemyJointBuffer_ = 0;
        }
        if (irradianceProgram_ != 0) {
            glDeleteProgram(irradianceProgram_);
            irradianceProgram_ = 0;
        }
        if (prefilterProgram_ != 0) {
            glDeleteProgram(prefilterProgram_);
            prefilterProgram_ = 0;
        }
        if (gbufferProgram_ != 0) {
            glDeleteProgram(gbufferProgram_);
            gbufferProgram_ = 0;
        }
        if (shadowProgram_ != 0) {
            glDeleteProgram(shadowProgram_);
            shadowProgram_ = 0;
        }
        if (lightingProgram_ != 0) {
            glDeleteProgram(lightingProgram_);
            lightingProgram_ = 0;
        }
        if (fxaaProgram_ != 0) {
            glDeleteProgram(fxaaProgram_);
            fxaaProgram_ = 0;
        }
        if (bloomThProgram_ != 0) {
            glDeleteProgram(bloomThProgram_);
            bloomThProgram_ = 0;
        }
        if (bloomDownProgram_ != 0) {
            glDeleteProgram(bloomDownProgram_);
            bloomDownProgram_ = 0;
        }
        if (bloomUpProgram_ != 0) {
            glDeleteProgram(bloomUpProgram_);
            bloomUpProgram_ = 0;
        }
        if (tonemapProgram_ != 0) {
            glDeleteProgram(tonemapProgram_);
            tonemapProgram_ = 0;
        }
        DestroyGBufferTarget();
        DestroyPostTarget();
        SDL_GL_DestroyContext(context_);
        context_ = nullptr;
        sceneReady_ = false;
    }

    // Mikan-style retry: restore every gameplay-owned value instead of only
    // healing the player.  This keeps the GLES retry path deterministic and
    // gives Vulkan the same reset contract below.
    void ResetGameplayScene()
    {
        playerX_ = 0.0f;
        playerY_ = 0.0f;
        playerZ_ = 0.0f;
        playerYaw_ = 0.0f;
        playerMoving_ = false;
        playerSprinting_ = false;
        crouchEnabled_ = false;
        playerHealth_ = playerMaxHealth_;
        playerAttackCooldown_ = 0.0f;
        playerAttackInvulnerability_ = 0.0f;
        playerHitInvulnerability_ = 0.0f;
        playerHitLockTimer_ = 0.0f;
        playerHitFlashTimer_ = 0.0f;
        playerAttackAcceptedThisFrame_ = false;

        enemyX_ = 1.35f;
        enemyY_ = 0.0f;
        enemyZ_ = -0.65f;
        enemyYaw_ = 0.0f;
        enemyHealth_ = enemyMaxHealth_;
        enemyAttackCooldown_ = 0.0f;
        enemyHitInvulnerability_ = 0.0f;
        enemyHitLockTimer_ = 0.0f;
        enemyAlive_ = true;
        enemyVisible_ = true;
        enemyDeathPlaying_ = false;
        enemyDeathTime_ = 0.0f;
        enemyDissolveTimer_ = -1.0f;
        enemyDissolveAmount_ = 0.0f;
        enemyMoving_ = false;
        enemyAttackRequested_ = false;
        enemyDesiredVelocityX_ = 0.0f;
        enemyDesiredVelocityZ_ = 0.0f;

        camYaw_ = 0.0f;
        camPitch_ = 0.0f;
        camDistance_ = kCameraDistance;
        camPanX_ = 0.0f;
        camPanY_ = 0.0f;
        camPanZ_ = 0.0f;
        camPosX_ = 0.0f;
        camPosY_ = 0.0f;
        camPosZ_ = kCameraDistance;
        moveLastTicks_ = SDL_GetTicks();

        animState_ = AnimState::Idle;
        animStateTime_ = 0.0f;
        animClipIndex_ = idleClip_ >= 0 ? idleClip_ : SelectAnimationClip();
        animLoop_ = true;
        animTime_ = 0.0f;
        animLastTicks_ = SDL_GetTicks();
        enemyAnimState_ = AnimState::Idle;
        enemyAnimStateTime_ = 0.0f;
        enemyAnimClipIndex_ = enemyIdleClip_ >= 0 ? enemyIdleClip_ : SelectAnimationClip();
        enemyAnimLoop_ = true;
        enemyAnimTime_ = 0.0f;
        enemyAnimLastTicks_ = SDL_GetTicks();

        cameraInput_ = rhi::CameraInput{};
        if (physicsWorld_ != nullptr && physicsWorld_->IsReady()) {
            physicsWorld_->Reset(
                playerX_, playerY_ - physics::kPlayerVisualOriginOffset, playerZ_,
                enemyX_, enemyY_ - (physics::kPlayerCapsuleHalfHeight
                    + physics::kPlayerCapsuleRadius)
                    - physics::kGameplayCharacterPadding,
                enemyZ_);
            physicsReset_ = true;
        } else {
            physicsReset_ = false;
        }
        uiScreen_ = rhi::UiScreen::Gameplay;
    }

private:
    bool CreatePrograms()
    {
        irradianceProgram_ = LinkProgram(kConvolutionVertexSource, kIrradianceFragmentSource, "irradiance");
        prefilterProgram_ = LinkProgram(kConvolutionVertexSource, kPrefilterFragmentSource, "prefilter");
        gbufferProgram_ = LinkProgram(kModelVertexSource, kGBufferFragmentSource, "gbuffer");
        shadowProgram_ = LinkProgram(kShadowVertexSource, kShadowFragmentSource, "shadow");
        lightingProgram_ = LinkProgram(kFullscreenVertexSource, kLightingFragmentSource, "lighting");
        fxaaProgram_ = LinkProgram(kFullscreenVertexSource, kFxaaFragmentSource, "fxaa");
        bloomThProgram_ = LinkProgram(kFullscreenVertexSource, kBloomThresholdFragmentSource, "bloom_th");
        bloomDownProgram_ = LinkProgram(kFullscreenVertexSource, kBloomDownFragmentSource, "bloom_down");
        bloomUpProgram_ = LinkProgram(kFullscreenVertexSource, kBloomUpFragmentSource, "bloom_up");
        tonemapProgram_ = LinkProgram(kFullscreenVertexSource, kTonemapFragmentSource, "tonemap");
        if (irradianceProgram_ == 0 || prefilterProgram_ == 0 || gbufferProgram_ == 0
            || lightingProgram_ == 0 || fxaaProgram_ == 0 || bloomThProgram_ == 0
            || bloomDownProgram_ == 0 || bloomUpProgram_ == 0 || tonemapProgram_ == 0) {
            return false;
        }
        BindSceneUniformBlock(gbufferProgram_, "gbuffer");
        // Skin joint matrices live in their own UBO on binding point 1
        // (see CreateUniformBuffer).  GLSL ES 3.00 has no binding qualifier
        // on blocks, so it has to be wired from the API like SceneUniforms.
        {
            const GLuint skinBlockIndex = glGetUniformBlockIndex(gbufferProgram_, "SkinUniforms");
            if (skinBlockIndex != GL_INVALID_INDEX) {
                glUniformBlockBinding(gbufferProgram_, skinBlockIndex, 1);
            }
            const GLuint enemySkinBlockIndex = glGetUniformBlockIndex(gbufferProgram_, "EnemySkinUniforms");
            if (enemySkinBlockIndex != GL_INVALID_INDEX) {
                glUniformBlockBinding(gbufferProgram_, enemySkinBlockIndex, 2);
            }
            if (shadowProgram_ != 0) {
                const GLuint shadowSkinBlockIndex = glGetUniformBlockIndex(shadowProgram_, "SkinUniforms");
                if (shadowSkinBlockIndex != GL_INVALID_INDEX) {
                    glUniformBlockBinding(shadowProgram_, shadowSkinBlockIndex, 1);
                }
                const GLuint shadowEnemySkinBlockIndex = glGetUniformBlockIndex(
                    shadowProgram_, "EnemySkinUniforms");
                if (shadowEnemySkinBlockIndex != GL_INVALID_INDEX) {
                    glUniformBlockBinding(shadowProgram_, shadowEnemySkinBlockIndex, 2);
                }
            }
        }
        skinningOnLocation_ = glGetUniformLocation(gbufferProgram_, "skinningOn");
        instancingOnLocation_ = glGetUniformLocation(gbufferProgram_, "instancingOn");
        enemyDissolveLocation_ = glGetUniformLocation(gbufferProgram_, "enemyDissolve");
        shadowMvpLocation_ = shadowProgram_ != 0 ? glGetUniformLocation(shadowProgram_, "shadowMvp") : -1;
        shadowSkinningOnLocation_ = shadowProgram_ != 0
            ? glGetUniformLocation(shadowProgram_, "skinningOn") : -1;
        shadowInstancingOnLocation_ = shadowProgram_ != 0
            ? glGetUniformLocation(shadowProgram_, "instancingOn") : -1;
        shadowEnemyDissolveLocation_ = shadowProgram_ != 0
            ? glGetUniformLocation(shadowProgram_, "enemyDissolve") : -1;
        irradianceSampler_ = glGetUniformLocation(irradianceProgram_, "skyboxTexture");
        prefilterSampler_ = glGetUniformLocation(prefilterProgram_, "skyboxTexture");
        irradianceFaceLocation_ = glGetUniformLocation(irradianceProgram_, "cubeFace");
        prefilterFaceLocation_ = glGetUniformLocation(prefilterProgram_, "cubeFace");
        prefilterRoughnessLocation_ = glGetUniformLocation(prefilterProgram_, "roughness");
        // Geometry pass (MRT writes).  Member names keep the model* prefix.
        modelBaseColorSampler_ = glGetUniformLocation(gbufferProgram_, "baseColorTexture");
        modelMrSampler_ = glGetUniformLocation(gbufferProgram_, "metallicRoughnessTexture");
        modelNormalSampler_ = glGetUniformLocation(gbufferProgram_, "normalTexture");
        modelAoSampler_ = glGetUniformLocation(gbufferProgram_, "aoTexture");
        modelEmissiveSampler_ = glGetUniformLocation(gbufferProgram_, "emissiveTexture");
        cameraPositionLocation_ = glGetUniformLocation(gbufferProgram_, "cameraPosition");
        materialParamsLocation_ = glGetUniformLocation(gbufferProgram_, "materialParams");
        materialBaseColorFactorLocation_ = glGetUniformLocation(gbufferProgram_, "materialBaseColorFactor");
        hasNormalMapLocation_ = glGetUniformLocation(gbufferProgram_, "hasNormalMap");
        // Lighting pass (G-buffer reads + IBL + sky).
        lightingAlbedoSampler_ = glGetUniformLocation(lightingProgram_, "albedoTexture");
        lightingNormalSampler_ = glGetUniformLocation(lightingProgram_, "normalTexture");
        lightingMaterialSampler_ = glGetUniformLocation(lightingProgram_, "materialTexture");
        lightingEmissiveSampler_ = glGetUniformLocation(lightingProgram_, "emissiveTexture");
        lightingDepthSampler_ = glGetUniformLocation(lightingProgram_, "depthTexture");
        lightingIrradianceSampler_ = glGetUniformLocation(lightingProgram_, "irradianceTexture");
        lightingEnvSampler_ = glGetUniformLocation(lightingProgram_, "envTexture");
        lightingBrdfLutSampler_ = glGetUniformLocation(lightingProgram_, "brdfLutTexture");
        lightingSkyboxSampler_ = glGetUniformLocation(lightingProgram_, "skyboxTexture");
        lightingShadowSampler_ = glGetUniformLocation(lightingProgram_, "shadowMap");
        lightingInvViewProjLocation_ = glGetUniformLocation(lightingProgram_, "invViewProj");
        lightingShadowMatrixLocation_ = glGetUniformLocation(lightingProgram_, "shadowMatrix");
        lightingCameraPositionLocation_ = glGetUniformLocation(lightingProgram_, "cameraPosition");
        lightingEmissiveFactorLocation_ = glGetUniformLocation(lightingProgram_, "emissiveFactor");
        lightingMaxLodLocation_ = glGetUniformLocation(lightingProgram_, "skyboxMaxLod");
        lightingEnvPrefilteredLocation_ = glGetUniformLocation(lightingProgram_, "envPrefiltered");
        lightingUseBrdfLutLocation_ = glGetUniformLocation(lightingProgram_, "useBrdfLut");
        lightingUseShadowMapLocation_ = glGetUniformLocation(lightingProgram_, "useShadowMap");
        lightingDebugNormalLocation_ = glGetUniformLocation(lightingProgram_, "debugNormal");
        lightingHdrOutputLocation_ = glGetUniformLocation(lightingProgram_, "hdrOutput");
        // Post-processing chain (FXAA).
        fxaaSampler_ = glGetUniformLocation(fxaaProgram_, "colorTex");
        // Bloom + AgX tonemap chain.
        bloomThSampler_ = glGetUniformLocation(bloomThProgram_, "inputTex");
        bloomThParams_ = glGetUniformLocation(bloomThProgram_, "uBloomParams");
        bloomDownSampler_ = glGetUniformLocation(bloomDownProgram_, "inputTex");
        bloomUpSampler_ = glGetUniformLocation(bloomUpProgram_, "inputTex");
        bloomUpPrevSampler_ = glGetUniformLocation(bloomUpProgram_, "prevTex");
        tonemapBloomSampler_ = glGetUniformLocation(tonemapProgram_, "inputTex");
        tonemapCompSampler_ = glGetUniformLocation(tonemapProgram_, "bloomTex");
        tonemapHdrMode_ = glGetUniformLocation(tonemapProgram_, "hdrMode");
        tonemapExposure_ = glGetUniformLocation(tonemapProgram_, "agxExposure");
        tonemapBloomStrength_ = glGetUniformLocation(tonemapProgram_, "bloomStrength");

        // The full-screen triangle has no attributes, but GLES 3 still requires
        // a bound vertex array object for every draw.  One for the convolution
        // passes (kept name), one for the lighting pass.
        glGenVertexArrays(1, &skyboxVao_);
        glGenVertexArrays(1, &fullscreenVao_);
        LogGlErrors("create_programs");
        return true;
    }

    bool CreateUniformBuffer()
    {
        glGenBuffers(1, &uniformBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneUniforms), nullptr, GL_DYNAMIC_DRAW);
        // Binding point 0, matching BindSceneUniformBlock above.
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniformBuffer_);
        // Joint matrix UBO: 256 mat4 = 16 KiB (ES 3.0 minimum block size),
        // permanently bound to binding point 1 -- only the first jointCount
        // slots are refreshed each frame by UpdateAnimation.
        glGenBuffers(1, &jointBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, jointBuffer_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(Mat4) * kMaxJoints, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, jointBuffer_);
        glGenBuffers(1, &enemyJointBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, enemyJointBuffer_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(Mat4) * kMaxJoints, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, enemyJointBuffer_);
        // Player animation restores the binding before the first draw; the
        // enemy animation restores it before its own draw.
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        LogGlErrors("create_uniform_buffer");
        return uniformBuffer_ != 0 && jointBuffer_ != 0 && enemyJointBuffer_ != 0;
    }

    void DestroyShadowMapTarget()
    {
        if (shadowFbo_ != 0) {
            glDeleteFramebuffers(1, &shadowFbo_);
            shadowFbo_ = 0;
        }
        if (shadowDepthTexture_ != 0) {
            glDeleteTextures(1, &shadowDepthTexture_);
            shadowDepthTexture_ = 0;
        }
        shadowReady_ = false;
    }

    bool CreateShadowMapTarget()
    {
        DestroyShadowMapTarget();
        glGenTextures(1, &shadowDepthTexture_);
        glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowMapSize, kShadowMapSize,
            0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &shadowFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
            shadowDepthTexture_, 0);
        const GLenum noColor = GL_NONE;
        glDrawBuffers(1, &noColor);
        glReadBuffer(GL_NONE);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!complete) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=shadow_fbo_incomplete");
            DestroyShadowMapTarget();
            return false;
        }
        shadowReady_ = true;
        SDL_Log("SDL3_GLES stage=shadow_target_ready size=%dx%d", kShadowMapSize, kShadowMapSize);
        return true;
    }

    // MRT G-buffer, attachment layout following MikanEngine's RenderTarget:
    //   0 albedo (RGBA8), 1 octahedral normal (RGBA8), 2 material (RGBA8),
    //   3 emissive (RGBA8; engine slot 3 is the TAA motion vector, skipped).
    // RGBA8 everywhere because the DGLES bridge gives no float/SNORM renderable
    // guarantee -- same Adreno-compat pragmatism as the engine's format chain.
    void DestroyGBufferTarget()
    {
        if (gbufferFbo_ != 0) {
            glDeleteFramebuffers(1, &gbufferFbo_);
            gbufferFbo_ = 0;
        }
        const GLuint textures[5] = {gbufferAlbedoTex_, gbufferNormalTex_, gbufferMaterialTex_,
            gbufferEmissiveTex_, gbufferDepthTex_};
        glDeleteTextures(5, textures);
        gbufferAlbedoTex_ = 0;
        gbufferNormalTex_ = 0;
        gbufferMaterialTex_ = 0;
        gbufferEmissiveTex_ = 0;
        gbufferDepthTex_ = 0;
        gbufferWidth_ = 0;
        gbufferHeight_ = 0;
    }

    bool CreateGBufferTarget()
    {
        DestroyGBufferTarget();
        // Global render resolution (kRenderWidth x kRenderHeight scaled by
        // renderScale_), not the drawable size: every consumer reads it with
        // normalized UVs, so a scale change just means destroy + lazy rebuild.
        const GLsizei w = std::max<GLsizei>(
            static_cast<GLsizei>(static_cast<float>(kRenderWidth) * renderScale_), 1);
        const GLsizei h = std::max<GLsizei>(
            static_cast<GLsizei>(static_cast<float>(kRenderHeight) * renderScale_), 1);
        if (w <= 0 || h <= 0) {
            return false;
        }

        const auto makeColorTexture = [&](GLuint& texture) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        };
        makeColorTexture(gbufferAlbedoTex_);
        makeColorTexture(gbufferNormalTex_);
        makeColorTexture(gbufferMaterialTex_);
        makeColorTexture(gbufferEmissiveTex_);

        glGenTextures(1, &gbufferDepthTex_);
        glBindTexture(GL_TEXTURE_2D, gbufferDepthTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT,
            GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &gbufferFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, gbufferFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            gbufferAlbedoTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
            gbufferNormalTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D,
            gbufferMaterialTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D,
            gbufferEmissiveTex_, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
            gbufferDepthTex_, 0);
        // Draw-buffers state is per framebuffer object in ES 3.0, so this sticks
        // to the G-buffer target and never touches the default framebuffer.
        const GLenum drawBuffers[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, drawBuffers);

        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!complete) {
            // DEPTH_COMPONENT24 not renderable on this driver: fall back to a
            // packed depth+stencil attachment.
            glBindTexture(GL_TEXTURE_2D, gbufferDepthTex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                GL_UNSIGNED_INT_24_8, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                gbufferDepthTex_, 0);
            complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (!complete) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=gbuffer_incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            DestroyGBufferTarget();
            return false;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gbufferWidth_ = static_cast<uint32_t>(w);
        gbufferHeight_ = static_cast<uint32_t>(h);
        LogGlErrors("create_gbuffer");
        SDL_Log("SDL3_GLES stage=gbuffer_ready size=%dx%d attachments=4", w, h);
        return true;
    }

    // Single-attachment LDR target feeding the post-processing chain.  The
    // lighting pass already output tonemapped + gamma-encoded colour, which is
    // the space FXAA wants.  Linear filtering is required: FXAA samples at
    // sub-pixel offsets along the detected edge.
    bool CreatePostTarget()
    {
        DestroyPostTarget();
        // Same scaled render resolution as the G-buffer (kRenderWidth x
        // kRenderHeight times renderScale_): independent of the drawable size,
        // rebuilt via destroy + lazy create on a scale change.
        const GLsizei w = std::max<GLsizei>(
            static_cast<GLsizei>(static_cast<float>(kRenderWidth) * renderScale_), 1);
        const GLsizei h = std::max<GLsizei>(
            static_cast<GLsizei>(static_cast<float>(kRenderHeight) * renderScale_), 1);

        // HDR probe: the engine composite is a linear HDR float image.  RGBA16F
        // is only color-renderable with EXT_color_buffer_float -- probe it by
        // actually attaching and checking completeness, then fall back to the
        // legacy LDR path (inline Reinhard + gamma, approximate bloom) when the
        // driver refuses.
        const auto makeTarget = [](GLuint& fbo, GLuint& tex, GLsizei tw, GLsizei th,
                                   GLenum internalFormat) -> bool {
            // Valid ES 3.0 upload format/type combo per internal format; data
            // is nullptr so only the combo's validity matters.
            GLenum uploadFormat = GL_RGBA;
            GLenum uploadType = GL_UNSIGNED_BYTE;
            if (internalFormat == GL_RGBA16F) {
                uploadFormat = GL_RGBA;
                uploadType = GL_HALF_FLOAT;
            } else if (internalFormat == GL_R11F_G11F_B10F) {
                uploadFormat = GL_RGB;
                uploadType = GL_UNSIGNED_INT_10F_11F_11F_REV;
            }
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, tw, th, 0, uploadFormat,
                uploadType, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return ok;
        };

        GLenum hdrFormat = GL_RGBA8;
        hdrPipeline_ = false;
        for (const GLenum candidate : kHdrFormats) {
            if (makeTarget(postFbo_, postColorTex_, w, h, candidate)) {
                hdrFormat = candidate;
                hdrPipeline_ = true;
                break;
            }
            if (postFbo_ != 0) {
                glDeleteFramebuffers(1, &postFbo_);
                postFbo_ = 0;
            }
            if (postColorTex_ != 0) {
                glDeleteTextures(1, &postColorTex_);
                postColorTex_ = 0;
            }
        }
        if (!hdrPipeline_) {
            if (!makeTarget(postFbo_, postColorTex_, w, h, GL_RGBA8)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=post_incomplete");
                DestroyPostTarget();
                return false;
            }
            hdrFormat = GL_RGBA8;
        }
        const GLenum bloomFormat = hdrFormat;

        // Bloom mip chain (engine postprocess_chain_mobile.json):
        //   ds[0] = half res (soft-knee threshold), ds[1..5] = dual-kernel /2
        //   up[4..0] = dual-kernel x2 fused with the previous up level.
        GLsizei cw = w / 2;
        GLsizei ch = h / 2;
        for (int i = 0; i < kBloomDsLevels; ++i) {
            bloomDsW_[i] = cw;
            bloomDsH_[i] = ch;
            if (!makeTarget(bloomDsFbo_[i], bloomDsTex_[i], cw, ch, bloomFormat)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=bloom_incomplete level=%d", i);
                DestroyPostTarget();
                return false;
            }
            cw = std::max(cw / 2, 1);
            ch = std::max(ch / 2, 1);
        }
        for (int i = kBloomUpLevels - 1; i >= 0; --i) {
            if (!makeTarget(bloomUpFbo_[i], bloomUpTex_[i], bloomDsW_[i], bloomDsH_[i],
                    bloomFormat)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=bloom_up_incomplete level=%d", i);
                DestroyPostTarget();
                return false;
            }
        }
        if (!makeTarget(tonemapFbo_, tonemapTex_, w, h, GL_RGBA8)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=tonemap_incomplete");
            DestroyPostTarget();
            return false;
        }

        postWidth_ = static_cast<uint32_t>(w);
        postHeight_ = static_cast<uint32_t>(h);
        LogGlErrors("create_post");
        const char* fmtName = hdrFormat == GL_R11F_G11F_B10F ? "r11g11b10f"
            : (hdrFormat == GL_RGBA16F ? "rgba16f" : "rgba8-ldr-fallback");
        SDL_Log("SDL3_GLES stage=post_chain_ready size=%dx%d fxaa=on hdr=%s bloom=%s", w, h,
            fmtName, bloomEnabled_ ? "6+5" : "off");
        return true;
    }

    void DestroyPostTarget()
    {
        if (postFbo_ != 0) {
            glDeleteFramebuffers(1, &postFbo_);
            postFbo_ = 0;
        }
        if (postColorTex_ != 0) {
            glDeleteTextures(1, &postColorTex_);
            postColorTex_ = 0;
        }
        for (int i = 0; i < kBloomDsLevels; ++i) {
            if (bloomDsFbo_[i] != 0) {
                glDeleteFramebuffers(1, &bloomDsFbo_[i]);
                bloomDsFbo_[i] = 0;
            }
            if (bloomDsTex_[i] != 0) {
                glDeleteTextures(1, &bloomDsTex_[i]);
                bloomDsTex_[i] = 0;
            }
        }
        for (int i = 0; i < kBloomUpLevels; ++i) {
            if (bloomUpFbo_[i] != 0) {
                glDeleteFramebuffers(1, &bloomUpFbo_[i]);
                bloomUpFbo_[i] = 0;
            }
            if (bloomUpTex_[i] != 0) {
                glDeleteTextures(1, &bloomUpTex_[i]);
                bloomUpTex_[i] = 0;
            }
        }
        if (tonemapFbo_ != 0) {
            glDeleteFramebuffers(1, &tonemapFbo_);
            tonemapFbo_ = 0;
        }
        if (tonemapTex_ != 0) {
            glDeleteTextures(1, &tonemapTex_);
            tonemapTex_ = 0;
        }
        postWidth_ = 0;
        postHeight_ = 0;
    }

    bool LoadSkybox()
    {
        const char* const paths[6] = {
            "skybox/right.jpg",
            "skybox/left.jpg",
            "skybox/top.jpg",
            "skybox/bottom.jpg",
            "skybox/front.jpg",
            "skybox/back.jpg",
        };

        glGenTextures(1, &skyboxTexture_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture_);

        int expectedWidth = 0;
        int expectedHeight = 0;
        bool success = true;
        for (int face = 0; face < 6 && success; ++face) {
            void* encoded = nullptr;
            size_t encodedSize = 0;
            if (!OHOS_ReadRawFile(paths[face], &encoded, &encodedSize) || encoded == nullptr ||
                encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=skybox_rawfile_failed path=%s",
                    paths[face]);
                if (encoded != nullptr) {
                    OHOS_FreeRawFile(encoded);
                }
                success = false;
                break;
            }
            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
                static_cast<int>(encodedSize), &width, &height, &channels, 4);
            OHOS_FreeRawFile(encoded);
            if (pixels == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=skybox_decode_failed path=%s",
                    paths[face]);
                success = false;
                break;
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            if (face == 0) {
                expectedWidth = width;
                expectedHeight = height;
            } else if (width != expectedWidth || height != expectedHeight) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=skybox_size_mismatch path=%s",
                    paths[face]);
                success = false;
            }
        }

        if (success) {
            // Mip chain doubles as the specular IBL prefilter: the PBR shader
            // samples roughness * maxLod so blurry reflections read from the
            // tiny mips.  min filter goes mipmap-linear accordingly.
            glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            skyboxSize_ = expectedWidth;
            // Mip count of the skybox chain: roughness maps linearly into it.
            skyboxMaxLod_ = skyboxSize_ > 0 ? log2f(static_cast<float>(skyboxSize_)) : 0.0f;
            LogGlErrors("load_skybox");
            SDL_Log("SDL3_GLES stage=skybox_ready faces=6 size=%dx%d mips=yes", expectedWidth,
                expectedHeight);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        return success;
    }

    // Convolves the loaded skybox into a 32x32 irradiance cubemap via FBO
    // offscreen passes, one face at a time.  RGBA16F is preferred (ES 3.2
    // makes it renderable) but the DGLES bridge may not expose a float
    // renderable -- fall back to RGBA8, which loses precision on HDR-ish
    // skies but is always complete.
    bool CreateIrradianceMap()
    {
        if (skyboxTexture_ == 0 || irradianceProgram_ == 0) {
            return false;
        }
        constexpr GLsizei kIrradianceSize = 32;

        glGenTextures(1, &irradianceTexture_);
        bool allocated = false;
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        for (int attempt = 0; attempt < 2 && !allocated; ++attempt) {
            const GLint internalFormat = attempt == 0 ? GL_RGBA16F : GL_RGBA8;
            const GLenum pixelType = attempt == 0 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
            glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceTexture_);
            for (int face = 0; face < 6; ++face) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, internalFormat,
                    kIrradianceSize, kIrradianceSize, 0, GL_RGBA, pixelType, nullptr);
            }
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_CUBE_MAP_POSITIVE_X, irradianceTexture_, 0);
            allocated = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            if (!allocated) {
                SDL_Log("SDL3_GLES stage=irradiance_format_fallback internal=%#x", internalFormat);
            }
        }
        if (!allocated) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &irradianceTexture_);
            irradianceTexture_ = 0;
            return false;
        }

        glUseProgram(irradianceProgram_);
        glUniform1i(irradianceSampler_, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture_);
        glBindVertexArray(skyboxVao_);
        glViewport(0, 0, kIrradianceSize, kIrradianceSize);
        glDisable(GL_DEPTH_TEST);
        for (int face = 0; face < 6; ++face) {
            glUniform1i(irradianceFaceLocation_, face);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, irradianceTexture_, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glBindVertexArray(0);
        glUseProgram(0);
        LogGlErrors("create_irradiance_map");
        SDL_Log("SDL3_GLES stage=irradiance_ready size=%d maxlod=%.1f", kIrradianceSize,
            skyboxMaxLod_);
        return true;
    }

    // GGX-importance-sampled specular prefilter: renders each mip level of a
    // small cubemap with a matching roughness (level i -> i / (levels-1)), so
    // the model shader's textureLod(R, roughness * maxLevel) hits a real GGX
    // convolution instead of the far-too-sharp naive mip blur.
    // Uploads the offline BRDF integration LUT shipped in the HAP rawfile.
    // Row 0 of the PNG is roughness 0 (verified against the analytic fit:
    // (NoV=1, rough=0) -> (0.969, 0.000)), so with GL's unflipped upload
    // v = roughness maps directly -- sample vec2(NoV, roughness).
    bool CreateBrdfLut()
    {
        void* encoded = nullptr;
        size_t encodedSize = 0;
        if (!OHOS_ReadRawFile("BrdfLut.png", &encoded, &encodedSize) || encoded == nullptr ||
            encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=brdf_lut_rawfile_failed");
            if (encoded != nullptr) {
                OHOS_FreeRawFile(encoded);
            }
            return false;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
            static_cast<int>(encodedSize), &width, &height, &channels, 4);
        OHOS_FreeRawFile(encoded);
        if (pixels == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=brdf_lut_decode_failed");
            return false;
        }
        glGenTextures(1, &brdfLutTexture_);
        glBindTexture(GL_TEXTURE_2D, brdfLutTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(pixels);
        LogGlErrors("create_brdf_lut");
        return brdfLutTexture_ != 0;
    }

    bool CreatePrefilteredMap()
    {
        if (skyboxTexture_ == 0 || prefilterProgram_ == 0) {
            return false;
        }
        constexpr GLsizei kPrefilterSize = 128;
        constexpr int kPrefilterLevels = 6;

        glGenTextures(1, &prefilteredTexture_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, prefilteredTexture_);
        // Allocate every level up front so the texture stays mipmap-complete
        // while each individual level is rendered into.
        for (int level = 0; level < kPrefilterLevels; ++level) {
            const GLsizei size = kPrefilterSize >> level;
            for (int face = 0; face < 6; ++face) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, level, GL_RGBA8,
                    size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        // Without MAX_LEVEL the completeness rule demands the full chain down to
        // 1x1 (8 levels for 128); with only 6 levels allocated the driver may
        // deem the texture mipmap-INCOMPLETE and return black for every sample,
        // textureLod(..., 0.0) included.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, kPrefilterLevels - 1);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glUseProgram(prefilterProgram_);
        glUniform1i(prefilterSampler_, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture_);
        glBindVertexArray(skyboxVao_);
        glDisable(GL_DEPTH_TEST);
        for (int level = 0; level < kPrefilterLevels; ++level) {
            glUniform1f(prefilterRoughnessLocation_,
                static_cast<float>(level) / static_cast<float>(kPrefilterLevels - 1));
            const GLsizei size = kPrefilterSize >> level;
            glViewport(0, 0, size, size);
            for (int face = 0; face < 6; ++face) {
                glUniform1i(prefilterFaceLocation_, face);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, prefilteredTexture_, level);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glUseProgram(0);
        LogGlErrors("create_prefilter");
        // The model shader maps roughness into this map's level range now, not
        // the skybox's full mip chain.
        skyboxMaxLod_ = static_cast<float>(kPrefilterLevels - 1);
        SDL_Log("SDL3_GLES stage=prefilter_ready levels=%d size=%d", kPrefilterLevels,
            kPrefilterSize);
        return true;
    }

    // Uploads one RGBA8 image.  Samples never come from disk twice: the loader
    // already decoded them.
    GLuint CreateTexture2D(const std::uint8_t* rgba, uint32_t width, uint32_t height)
    {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
            0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // REPEAT, not CLAMP_TO_EDGE: glTF's implicit sampler wraps, and
        // DamagedHelmet ships UVs in v [1,2] that rely on exactly that.  Clamping
        // pins every fragment to the edge row and smears it into streaks.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        return texture;
    }

    void CreateFallbackTexture()
    {
        // Sampling an unbound texture is undefined behaviour, so every draw gets
        // a real texture: this one for meshes that resolve to no base colour.
        const std::uint8_t white[4] = {255, 255, 255, 255};
        whiteTexture_ = CreateTexture2D(white, 1, 1);
        // Neutral metallicRoughness fallback: G = roughness 1.0, B = metallic 0.0
        // (a white texel here would turn every untextured mesh into a rough
        // mirror, the exact opposite of the white albedo fallback's intent).
        const std::uint8_t neutralMr[4] = {255, 255, 0, 255};
        neutralMrTexture_ = CreateTexture2D(neutralMr, 1, 1);
    }

    void LoadModel()
    {
        ohos_model::Model model;
        std::string error;
        if (!ohos_model::LoadModelFromRawFile(kModelPath, model, error)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=model_load_failed error=%s",
                error.c_str());
            return;
        }

        // Kept so a mesh can be resolved to its base colour image at draw time.
        materials_ = model.materials;

        // Skeletal animation data (cgltf-independent copies owned by the
        // renderer; the model struct is function-local).
        skinningOn_ = model.skinned;
        animNodes_ = std::move(model.nodes);
        skin_ = std::move(model.skin);
        clips_ = std::move(model.animations);
        modelNormalise_ = Mat4{};
        for (int c = 0; c < 16; ++c) {
            modelNormalise_.value[c] = model.normaliseMatrix[c];
        }
        walkClip_ = FindClipIndex("Walk_Loop");
        sprintClip_ = FindClipIndex("Sprint_Loop");
        idleClip_ = FindClipIndex("Idle_Loop");
        crouchClip_ = FindClipIndex("Crouch_Idle_Loop");
        attackClip_ = FindClipIndex("Punch_Jab");
        jumpStartClip_ = FindClipIndex("Jump_Start");
        jumpLoopClip_ = FindClipIndex("Jump_Loop");
        jumpLandClip_ = FindClipIndex("Jump_Land");
        deathClip_ = FindClipIndex("Death01");
        // MikanEngine third-person prototype (CesiumWalk.cpp PlayerWalkScript):
        // the hit reaction plays Hit_Chest as a one-shot under a short
        // control lock with a red screen flash.
        hitClip_ = FindClipIndex("Hit_Chest");
        // The state machine owns clip selection: start standing (Idle).
        animState_ = AnimState::Idle;
        animClipIndex_ = idleClip_ >= 0 ? idleClip_ : SelectAnimationClip();
        animStateTime_ = 0.0f;
        animLoop_ = true;
        animTime_ = 0.0f;
        animLastTicks_ = SDL_GetTicks();

        // Keep a second CPU animation state and joint UBO for the enemy.  The
        // mesh/material residency is intentionally shared, but the skeleton
        // sampling is not: an enemy must not be a second draw of the player's
        // current pose.
        enemyAnimNodes_ = animNodes_;
        enemySkin_ = skin_;
        enemyClips_ = clips_;
        enemyModelNormalise_ = modelNormalise_;
        enemyWalkClip_ = walkClip_;
        enemySprintClip_ = sprintClip_;
        enemyIdleClip_ = idleClip_;
        enemyCrouchClip_ = crouchClip_;
        enemyAttackClip_ = attackClip_;
        enemyJumpStartClip_ = jumpStartClip_;
        enemyJumpLoopClip_ = jumpLoopClip_;
        enemyJumpLandClip_ = jumpLandClip_;
        enemyDeathClip_ = deathClip_;
        enemyHitClip_ = hitClip_;
        enemyAnimState_ = AnimState::Idle;
        enemyAnimClipIndex_ = enemyIdleClip_ >= 0 ? enemyIdleClip_ : SelectAnimationClip();
        enemyAnimStateTime_ = 0.0f;
        enemyAnimLoop_ = true;
        enemyAnimTime_ = 0.0f;
        enemyAnimLastTicks_ = SDL_GetTicks();

        modelTextures_.reserve(model.images.size());
        for (const ohos_model::Image& image : model.images) {
            if (image.rgba.empty() || image.width == 0 || image.height == 0) {
                modelTextures_.push_back(0);
                continue;
            }
            modelTextures_.push_back(CreateTexture2D(image.rgba.data(), image.width, image.height));
        }

        meshes_.reserve(model.meshes.size());
        for (const ohos_model::Mesh& source : model.meshes) {
            if (source.vertices.empty() || source.indices.empty()) {
                continue;
            }
            // Quaternius' AnimationLibrary carries an "M_Joints" shell as a
            // second primitive INSIDE the body: it fills the gaps between the
            // body segments at the joints (skip it and wrists/waist/knees show
            // holes).  Its own factor is a debug purple, so render it with the
            // first real body material instead of skipping.
            int materialIndex = source.materialIndex;
            if (materialIndex >= 0
                && static_cast<size_t>(materialIndex) < model.materials.size()) {
                const std::string& name = model.materials[materialIndex].name;
                std::string lower = name;
                for (char& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lower.find("joint") != std::string::npos) {
                    for (size_t m = 0; m < model.materials.size(); ++m) {
                        std::string other = model.materials[m].name;
                        for (char& c : other) {
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        }
                        if (other.find("joint") == std::string::npos) {
                            materialIndex = static_cast<int>(m);
                            SDL_Log("SDL3_GLES stage=joint_mesh_recolored from=%s to=%s",
                                name.c_str(), model.materials[m].name.c_str());
                            break;
                        }
                    }
                }
            }
            GlesMesh mesh;
            mesh.indexCount = static_cast<GLuint>(source.indices.size());
            mesh.indexType = source.indexBytes == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            mesh.materialIndex = static_cast<int32_t>(materialIndex);

            glGenVertexArrays(1, &mesh.vao);
            glBindVertexArray(mesh.vao);

            glGenBuffers(1, &mesh.vertexBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vertexBuffer);
            glBufferData(GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(source.vertices.size() * sizeof(ohos_model::Vertex)),
                source.vertices.data(), GL_STATIC_DRAW);

            const GLsizei stride = static_cast<GLsizei>(sizeof(ohos_model::Vertex));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, position)));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, normal)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, uv)));
            if (skinningOn_) {
                // JOINTS_0 as raw u8 (shader casts to int) and WEIGHTS_0 as
                // normalised u8 (driver converts to 0..1 floats).
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                    reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, joints)));
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                    reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, weights)));
            }

            glGenBuffers(1, &mesh.indexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indexBuffer);
            if (source.indexBytes == 4) {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(source.indices.size() * sizeof(std::uint32_t)),
                    source.indices.data(), GL_STATIC_DRAW);
            } else {
                std::vector<std::uint16_t> narrow;
                narrow.reserve(source.indices.size());
                for (std::uint32_t index : source.indices) {
                    narrow.push_back(static_cast<std::uint16_t>(index));
                }
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(narrow.size() * sizeof(std::uint16_t)), narrow.data(),
                    GL_STATIC_DRAW);
            }

            glBindVertexArray(0);
            meshes_.push_back(mesh);
        }
        CreateCharacterInstanceBuffer();
        LogGlErrors("load_model");
        SDL_Log("SDL3_GLES stage=model_ready meshes=%zu textures=%zu triangles=%u",
            meshes_.size(), modelTextures_.size(), model.triangleCount);
        if (skinningOn_) {
            const char* clipName = animClipIndex_ >= 0
                && animClipIndex_ < static_cast<int>(clips_.size())
                ? clips_[animClipIndex_].name.c_str() : "?";
            const float clipDuration = animClipIndex_ >= 0
                && animClipIndex_ < static_cast<int>(clips_.size())
                ? clips_[animClipIndex_].duration : 0.0f;
            SDL_Log("SDL3_GLES stage=animation_ready joints=%zu clips=%zu clip=%s duration=%.2f",
                skin_.JointCount(), clips_.size(), clipName, clipDuration);
        }

        LoadHelmetModel();
        LoadGroundModel();
    }

    void CreateCharacterInstanceBuffer()
    {
        if (meshes_.empty() || characterInstanceVbo_ != 0) {
            return;
        }
        glGenBuffers(1, &characterInstanceVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, characterInstanceVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(CharacterInstanceData) * 2, nullptr,
            GL_DYNAMIC_DRAW);
        for (const GlesMesh& mesh : meshes_) {
            glBindVertexArray(mesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, characterInstanceVbo_);
            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(CharacterInstanceData));
            for (GLuint column = 0; column < 4; ++column) {
                const GLuint location = 5 + column;
                glEnableVertexAttribArray(location);
                glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<const void*>(sizeof(float) * 4 * column));
                glVertexAttribDivisor(location, 1);
            }
            glEnableVertexAttribArray(9);
            glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(sizeof(Mat4)));
            glVertexAttribDivisor(9, 1);
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        LogGlErrors("create_character_instance_buffer");
    }

    // Static prop: the DamagedHelmet glTF next to the skinned character.  Goes
    // through the same loader (its static path bakes node transforms and the
    // normalisation into the vertices) but owns independent meshes/textures.
    void LoadHelmetModel()
    {
        ohos_model::Model model;
        std::string error;
        if (!ohos_model::LoadModelFromRawFile(kHelmetPath, model, error)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=helmet_load_failed error=%s",
                error.c_str());
            return;
        }
        if (model.skinned) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=helmet_load_failed error=unexpected_skin");
            return;
        }

        helmetMaterials_ = model.materials;
        LoadStaticTextures(model, helmetTextures_);
        BuildStaticMeshes(model, helmetMeshes_);
        LogGlErrors("load_helmet");
        // Loader normalises to extent 2 centred at the origin; the draw call
        // parks it to the character's left at the same ground line (feet ~
        // y=-0.94, half height) and spins it per frame around that spot.
        SDL_Log("SDL3_GLES stage=helmet_ready meshes=%zu textures=%zu",
            helmetMeshes_.size(), helmetTextures_.size());
    }

    // Shared static-model plumbing (helmet + ground cube): texture pool and
    // VAO/EBO creation over an already-parsed non-skinned model.
    void LoadStaticTextures(const ohos_model::Model& model, std::vector<GLuint>& out)
    {
        out.reserve(model.images.size());
        for (const ohos_model::Image& image : model.images) {
            if (image.rgba.empty() || image.width == 0 || image.height == 0) {
                out.push_back(0);
                continue;
            }
            out.push_back(CreateTexture2D(image.rgba.data(), image.width, image.height));
        }
    }

    void BuildStaticMeshes(const ohos_model::Model& model, std::vector<GlesMesh>& out)
    {
        out.reserve(model.meshes.size());
        for (const ohos_model::Mesh& source : model.meshes) {
            if (source.vertices.empty() || source.indices.empty()) {
                continue;
            }
            GlesMesh mesh;
            mesh.indexCount = static_cast<GLuint>(source.indices.size());
            mesh.indexType = source.indexBytes == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            mesh.materialIndex = source.materialIndex;

            glGenVertexArrays(1, &mesh.vao);
            glBindVertexArray(mesh.vao);

            glGenBuffers(1, &mesh.vertexBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, mesh.vertexBuffer);
            glBufferData(GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(source.vertices.size() * sizeof(ohos_model::Vertex)),
                source.vertices.data(), GL_STATIC_DRAW);

            const GLsizei stride = static_cast<GLsizei>(sizeof(ohos_model::Vertex));
            // Locations 0/1/2 only: no joints/weights streams for static
            // geometry (the vertex shader's skinningOn gate is 0 anyway, but
            // leaving the generic attribs unbound keeps the intent clear).
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, position)));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, normal)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offsetof(ohos_model::Vertex, uv)));

            glGenBuffers(1, &mesh.indexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indexBuffer);
            if (source.indexBytes == 4) {
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(source.indices.size() * sizeof(std::uint32_t)),
                    source.indices.data(), GL_STATIC_DRAW);
            } else {
                std::vector<std::uint16_t> narrow;
                narrow.reserve(source.indices.size());
                for (std::uint32_t index : source.indices) {
                    narrow.push_back(static_cast<std::uint16_t>(index));
                }
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(narrow.size() * sizeof(std::uint16_t)), narrow.data(),
                    GL_STATIC_DRAW);
            }

            glBindVertexArray(0);
            out.push_back(mesh);
        }
    }

    // Ground: the Base Model cube.glb stretched into a slab via the per-draw
    // transform (see the geometry pass).  The loader's normalisation is a no-op
    // for it (extent already 2, centred), so [-1,1]^3 is what gets scaled.
    void LoadGroundModel()
    {
        ohos_model::Model model;
        std::string error;
        if (!ohos_model::LoadModelFromRawFile(kCubePath, model, error)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=ground_load_failed error=%s",
                error.c_str());
            return;
        }
        if (model.skinned) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_GLES stage=ground_load_failed error=unexpected_skin");
            return;
        }
        groundMaterials_ = model.materials;
        LoadStaticTextures(model, groundTextures_);
        BuildStaticMeshes(model, groundMeshes_);
        LogGlErrors("load_ground");
        SDL_Log("SDL3_GLES stage=ground_ready meshes=%zu textures=%zu",
            groundMeshes_.size(), groundTextures_.size());
    }

    // Picks the clip to play: Walk_Loop reads best on a static camera, with
    // Dance/Idle fallbacks; anything else falls back to the first clip.
    int SelectAnimationClip() const
    {
        static constexpr const char* kPreferences[] = {"Walk_Loop", "Dance_Loop", "Idle_Loop"};
        for (const char* preference : kPreferences) {
            for (size_t i = 0; i < clips_.size(); ++i) {
                if (clips_[i].name == preference) {
                    return static_cast<int>(i);
                }
            }
        }
        return clips_.empty() ? -1 : 0;
    }

    int FindClipIndex(const char* name) const
    {
        for (size_t i = 0; i < clips_.size(); ++i) {
            if (clips_[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // ---- animation state machine --------------------------------------------
    // Ported from MikanEngine's PlayerWalkScript (CesiumWalk.cpp): an enum
    // state + EnterAnimationState(stateTime reset) + a priority if/else
    // transition chain + clip mapping with change detection.  Hard cuts, no
    // crossfade -- exactly the engine's behaviour (it relies on Loop clips
    // whose first/last frames match).  MikanEngine's "script system" is pure
    // C++ (IScriptBehaviour), so there was nothing to port beyond this
    // pattern; the three on-screen buttons replace its keyboard/touch input.
    enum class AnimState { Idle, Walk, Sprint, Attack, JumpStart, JumpLoop, JumpLand, Crouch, Hit, Death };

    static const char* AnimStateHudName(AnimState state)
    {
        switch (state) {
        case AnimState::Idle: return "IDLE";
        case AnimState::Walk: return "WALK";
        case AnimState::Sprint: return "RUN";
        case AnimState::Attack: return "ATTACK";
        case AnimState::JumpStart:
        case AnimState::JumpLoop: return "JUMP";
        case AnimState::JumpLand: return "LAND";
        case AnimState::Crouch: return "CROUCH";
        case AnimState::Hit: return "HIT";
        case AnimState::Death: return "DEAD";
        }
        return "WALK";
    }

    // Grounded loop state the one-shot states return to: crouch is a level
    // toggle, and the walk/sprint/idle split follows the joystick plus the
    // held RUN button (MikanEngine's groundedState with crouching/moving
    // booleans, extended with a sprint level).
    AnimState GroundedState() const
    {
        if (crouchEnabled_) {
            return AnimState::Crouch;
        }
        if (!playerMoving_) {
            return AnimState::Idle;
        }
        return playerSprinting_ ? AnimState::Sprint : AnimState::Walk;
    }

    float ClipDurationOr(int clipIndex, float fallback) const
    {
        return clipIndex >= 0 && clipIndex < static_cast<int>(clips_.size())
            && clips_[clipIndex].duration > 0.0f
            ? clips_[clipIndex].duration : fallback;
    }

    void EnterAnimState(AnimState next)
    {
        if (animState_ == next) {
            return;
        }
        int targetClip = walkClip_;
        bool targetLoop = true;
        switch (next) {
        case AnimState::Idle: targetClip = idleClip_; targetLoop = true; break;
        case AnimState::Walk: targetClip = walkClip_; targetLoop = true; break;
        case AnimState::Sprint: targetClip = sprintClip_; targetLoop = true; break;
        case AnimState::Crouch: targetClip = crouchClip_; targetLoop = true; break;
        case AnimState::Attack: targetClip = attackClip_; targetLoop = false; break;
        case AnimState::JumpStart: targetClip = jumpStartClip_; targetLoop = false; break;
        case AnimState::JumpLoop: targetClip = jumpLoopClip_; targetLoop = true; break;
        case AnimState::JumpLand: targetClip = jumpLandClip_; targetLoop = false; break;
        case AnimState::Death: targetClip = deathClip_; targetLoop = false; break;
        case AnimState::Hit: targetClip = hitClip_; targetLoop = false; break;
        }
        if (targetClip < 0) {
            // Clip missing from this asset: fall back to the base loop.
            next = AnimState::Walk;
            targetClip = walkClip_;
            targetLoop = true;
        }
        animState_ = next;
        animStateTime_ = 0.0f;
        if (animClipIndex_ != targetClip) {
            // MikanEngine change-detection bridge: PlayAnimation = hard cut,
            // new clip from frame 0.
            animClipIndex_ = targetClip;
            animTime_ = 0.0f;
        }
        animLoop_ = targetLoop;
    }

    void UpdateAnimStateMachine(float dt)
    {
        animStateTime_ += dt;

        // Consume button edges: Attack > Jump > Dance, mirroring the script's
        // intent sampling order (attack can interrupt anything; jumping only
        // from a grounded loop state).
        const int pressed = cameraInput_.actionButtonPressedMask;
        cameraInput_.actionButtonPressedMask = 0;
        // Mikan hit-stun: while the control lock is active the Hit one-shot
        // overrides everything and all action edges are swallowed.
        if (playerHitLockTimer_ > 0.0f) {
            if (animState_ != AnimState::Hit) {
                EnterAnimState(AnimState::Hit);
            }
            return;
        }
        // UpdateEnemyAi admits the attack edge only when the shared combat
        // cooldown is ready.  Reuse that decision here so the animation and
        // the hit query cannot disagree when the button is tapped rapidly.
        if (playerAttackAcceptedThisFrame_) {
            EnterAnimState(AnimState::Attack);
        }
        if (pressed & 2) {
            if (animState_ == GroundedState()) {
                EnterAnimState(AnimState::JumpStart);
            }
        }
        if (pressed & 8) {
            // Crouch is a level toggle: mid attack/jump only the flag flips,
            // and the one-shot's return lands in the new grounded state.
            crouchEnabled_ = !crouchEnabled_;
            if (animState_ == AnimState::Walk || animState_ == AnimState::Idle
                || animState_ == AnimState::Crouch) {
                EnterAnimState(GroundedState());
            }
        }

        // Movement drives the grounded loop: Walk/Sprint while the stick
        // pushes (Sprint only while the RUN button is held), Idle standing
        // (playerMoving_/playerSprinting_ were computed in
        // UpdateSceneUniforms earlier this frame).
        if (animState_ == AnimState::Walk || animState_ == AnimState::Idle
            || animState_ == AnimState::Sprint) {
            const AnimState grounded = GroundedState();
            if (animState_ != grounded) {
                EnterAnimState(grounded);
            }
        }

        // One-shot states self-transition on their state timer (the script's
        // m_AnimationStateTime pattern); grounded loop states are stable.
        switch (animState_) {
        case AnimState::Attack:
            if (animStateTime_ >= ClipDurationOr(attackClip_, 0.87f)) {
                EnterAnimState(GroundedState());
            }
            break;
        case AnimState::JumpStart:
            // The engine's jumpStartDuration/landDuration: it cuts these clips
            // well before their full length for a snappier feel.
            if (animStateTime_ >= 0.35f) {
                EnterAnimState(AnimState::JumpLoop);
            }
            break;
        case AnimState::JumpLoop:
            // In-place demo: fixed airtime before landing (the engine uses a
            // vertical-velocity threshold it has no use for here).
            if (animStateTime_ >= 0.5f) {
                EnterAnimState(AnimState::JumpLand);
            }
            break;
        case AnimState::JumpLand:
            if (animStateTime_ >= 0.45f) {
                EnterAnimState(GroundedState());
            }
            break;
        case AnimState::Idle:
        case AnimState::Walk:
        case AnimState::Sprint:
        case AnimState::Crouch:
            break;
        case AnimState::Hit:
            // Lock expired this frame: snap back to the grounded loop.
            if (playerHitLockTimer_ <= 0.0f) {
                EnterAnimState(GroundedState());
            }
            break;
        case AnimState::Death:
            break;
        }
    }

    // Samples one animation channel at `time` (LINEAR/slerp or STEP hold;
    // clamped to the first/last key outside the range).
    void SampleChannel(const ohos_model::AnimationChannel& channel, float time, float* out) const
    {
        const std::size_t frames = channel.times.size();
        const std::size_t comps = channel.path == 1 ? 4u : 3u;
        if (frames == 0) {
            out[0] = out[1] = out[2] = 0.0f;
            out[3] = 1.0f;
            return;
        }
        if (time <= channel.times[0]) {
            for (std::size_t c = 0; c < comps; ++c) {
                out[c] = channel.values[c];
            }
            return;
        }
        if (time >= channel.times[frames - 1]) {
            for (std::size_t c = 0; c < comps; ++c) {
                out[c] = channel.values[(frames - 1) * comps + c];
            }
            return;
        }
        std::size_t i = 0;
        while (i + 1 < frames && channel.times[i + 1] <= time) {
            ++i;
        }
        if (channel.interpolation == 1 || channel.times[i + 1] <= channel.times[i]) {
            for (std::size_t c = 0; c < comps; ++c) {
                out[c] = channel.values[i * comps + c];
            }
            return;
        }
        const float span = channel.times[i + 1] - channel.times[i];
        const float f = (time - channel.times[i]) / span;
        if (comps == 4u) {
            SlerpQuat(&channel.values[i * 4], &channel.values[(i + 1) * 4], f, out);
        } else {
            for (std::size_t c = 0; c < 3; ++c) {
                out[c] = channel.values[i * 3 + c]
                    + (channel.values[(i + 1) * 3 + c] - channel.values[i * 3 + c]) * f;
            }
        }
    }

    // Per-frame animation: advance time, sample channels into per-node TRS,
    // compose locals, propagate globals parent-first, build skin matrices
    // (joint = normalise * global * inverseBind) and upload the UBO.
    void UpdateAnimation()
    {
        if (!skinningOn_ || animClipIndex_ < 0
            || animClipIndex_ >= static_cast<int>(clips_.size())) {
            return;
        }
        const Uint64 nowTicks = SDL_GetTicks();
        float dt = 0.0f;
        if (animLastTicks_ != 0) {
            dt = static_cast<float>(nowTicks - animLastTicks_) * 0.001f;
            // Clamp long stalls (debugger pauses, emulator hiccups) so the
            // character does not teleport mid-clip.
            dt = dt < 0.0f ? 0.0f : (dt > 0.1f ? 0.1f : dt);
        }
        animLastTicks_ = nowTicks;

        UpdateAnimStateMachine(dt);

        const ohos_model::AnimationClip& clip = clips_[animClipIndex_];
        if (clip.duration > 0.0f) {
            if (animLoop_) {
                animTime_ = std::fmod(animTime_ + dt, clip.duration);
            } else {
                // One-shot clips freeze on their final frame until the state
                // machine switches away (MikanEngine AdvanceAnimationState).
                animTime_ = animTime_ + dt < clip.duration ? animTime_ + dt : clip.duration;
            }
        }

        const std::size_t nodeCount = animNodes_.size();
        // 1) per-node TRS: defaults, then animated overrides.
        nodeTranslation_.assign(nodeCount * 3, 0.0f);
        nodeRotation_.assign(nodeCount * 4, 0.0f);
        nodeScale_.assign(nodeCount * 3, 1.0f);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                nodeTranslation_[i * 3 + c] = animNodes_[i].translation[c];
                nodeScale_[i * 3 + c] = animNodes_[i].scale[c];
            }
            for (int c = 0; c < 4; ++c) {
                nodeRotation_[i * 4 + c] = animNodes_[i].rotation[c];
            }
        }
        float sampled[4];
        for (const ohos_model::AnimationChannel& channel : clip.channels) {
            if (channel.nodeIndex >= nodeCount) {
                continue;
            }
            SampleChannel(channel, animTime_, sampled);
            const std::size_t base = channel.nodeIndex * (channel.path == 1 ? 4u : 3u);
            float* target = channel.path == 0 ? &nodeTranslation_[base]
                : (channel.path == 1 ? &nodeRotation_[base] : &nodeScale_[base]);
            const std::size_t comps = channel.path == 1 ? 4u : 3u;
            for (std::size_t c = 0; c < comps; ++c) {
                target[c] = sampled[c];
            }
        }

        // 2) locals -> globals.  Parents precede children in animNodes_ order,
        // so a single forward pass suffices (glTF node graphs are trees).
        nodeGlobals_.assign(nodeCount * 16, 0.0f);
        std::vector<float> local(16);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            ComposeTrs(&nodeTranslation_[i * 3], &nodeRotation_[i * 4],
                &nodeScale_[i * 3], local.data());
            const std::int32_t parent = animNodes_[i].parent;
            float* global = &nodeGlobals_[i * 16];
            if (parent < 0
                || static_cast<std::size_t>(parent) >= nodeCount) {
                for (int c = 0; c < 16; ++c) {
                    global[c] = local[c];
                }
            } else {
                Mat4Multiply16(&nodeGlobals_[static_cast<std::size_t>(parent) * 16],
                    local.data(), global);
            }
        }

        // 3) skin matrices in normalised model space.
        const std::size_t jointCount = std::min(skin_.JointCount(),
            static_cast<std::size_t>(kMaxJoints));
        jointMatrices_.assign(kMaxJoints * 16, 0.0f);
        std::vector<float> skinMatrix(16);
        for (std::size_t j = 0; j < jointCount; ++j) {
            float* out = &jointMatrices_[j * 16];
            if (animNodes_.empty()) {
                continue;
            }
            const float* global = &nodeGlobals_[static_cast<std::size_t>(skin_.jointNodes[j]) * 16];
            const float* inverseBind = &skin_.inverseBindMatrices[j * 16];
            Mat4Multiply16(global, inverseBind, skinMatrix.data());
            Mat4Multiply16(modelNormalise_.value, skinMatrix.data(), out);
        }

        // 4) upload.
        glBindBuffer(GL_UNIFORM_BUFFER, jointBuffer_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0,
            static_cast<GLsizeiptr>(jointMatrices_.size() * sizeof(float)),
            jointMatrices_.data());
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        LogGlErrors("update_animation");
    }

    AnimState EnemyGroundedState() const
    {
        return enemyMoving_ ? AnimState::Walk : AnimState::Idle;
    }

    float EnemyClipDurationOr(int clipIndex, float fallback) const
    {
        return clipIndex >= 0 && clipIndex < static_cast<int>(enemyClips_.size())
            && enemyClips_[clipIndex].duration > 0.0f
            ? enemyClips_[clipIndex].duration : fallback;
    }

    void EnterEnemyAnimState(AnimState next)
    {
        if (enemyAnimState_ == next) {
            return;
        }
        int targetClip = enemyWalkClip_;
        bool targetLoop = true;
        switch (next) {
        case AnimState::Idle: targetClip = enemyIdleClip_; targetLoop = true; break;
        case AnimState::Walk: targetClip = enemyWalkClip_; targetLoop = true; break;
        case AnimState::Sprint: targetClip = enemySprintClip_; targetLoop = true; break;
        case AnimState::Crouch: targetClip = enemyCrouchClip_; targetLoop = true; break;
        case AnimState::Attack: targetClip = enemyAttackClip_; targetLoop = false; break;
        case AnimState::JumpStart: targetClip = enemyJumpStartClip_; targetLoop = false; break;
        case AnimState::JumpLoop: targetClip = enemyJumpLoopClip_; targetLoop = true; break;
        case AnimState::JumpLand: targetClip = enemyJumpLandClip_; targetLoop = false; break;
        case AnimState::Death: targetClip = enemyDeathClip_; targetLoop = false; break;
        case AnimState::Hit: targetClip = enemyHitClip_; targetLoop = false; break;
        }
        if (targetClip < 0) {
            next = AnimState::Walk;
            targetClip = enemyWalkClip_ >= 0 ? enemyWalkClip_ : enemyIdleClip_;
            targetLoop = true;
        }
        enemyAnimState_ = next;
        enemyAnimStateTime_ = 0.0f;
        if (enemyAnimClipIndex_ != targetClip) {
            enemyAnimClipIndex_ = targetClip;
            enemyAnimTime_ = 0.0f;
        }
        enemyAnimLoop_ = targetLoop;
    }

    void UpdateEnemyAnimStateMachine(float dt)
    {
        enemyAnimStateTime_ += dt;
        if (enemyDeathPlaying_) {
            if (enemyAnimState_ != AnimState::Death) {
                EnterEnemyAnimState(AnimState::Death);
            }
            return;
        }
        // Mikan PuppetEnemyScript: hit-stun holds the Hit one-shot and the
        // AI cannot override it until the lock expires.
        if (enemyHitLockTimer_ > 0.0f) {
            if (enemyAnimState_ != AnimState::Hit) {
                EnterEnemyAnimState(AnimState::Hit);
            }
            return;
        }
        if (enemyAnimState_ == AnimState::Hit) {
            // Lock expired this frame: snap back to the grounded loop.
            EnterEnemyAnimState(EnemyGroundedState());
        }
        if (enemyAttackRequested_) {
            EnterEnemyAnimState(AnimState::Attack);
            enemyAttackRequested_ = false;
        }
        if (enemyAnimState_ == AnimState::Walk || enemyAnimState_ == AnimState::Idle) {
            const AnimState grounded = EnemyGroundedState();
            if (enemyAnimState_ != grounded) {
                EnterEnemyAnimState(grounded);
            }
        }
        if (enemyAnimState_ == AnimState::Attack &&
            enemyAnimStateTime_ >= EnemyClipDurationOr(enemyAttackClip_, 0.87f)) {
            EnterEnemyAnimState(EnemyGroundedState());
        }
    }

    void UpdateEnemyAnimation()
    {
        if (!skinningOn_ || enemyJointBuffer_ == 0 || !enemyVisible_ || enemyAnimClipIndex_ < 0
            || enemyAnimClipIndex_ >= static_cast<int>(enemyClips_.size())) {
            return;
        }
        const Uint64 nowTicks = SDL_GetTicks();
        float dt = 0.0f;
        if (enemyAnimLastTicks_ != 0) {
            dt = static_cast<float>(nowTicks - enemyAnimLastTicks_) * 0.001f;
            dt = dt < 0.0f ? 0.0f : (dt > 0.1f ? 0.1f : dt);
        }
        enemyAnimLastTicks_ = nowTicks;
        if (enemyDissolveTimer_ >= 0.0f) {
            // Corpse dissolve in progress: advance the burn, keep the death
            // pose palette frozen, and never let the state machine touch the
            // animation (the enemy must not stand back up mid-dissolve).
            enemyDissolveTimer_ += dt;
            enemyDissolveAmount_ = std::min(
                enemyDissolveTimer_ / kEnemyDissolveDuration, 1.0f);
            if (enemyDissolveTimer_ >= kEnemyDissolveDuration) {
                enemyDissolveTimer_ = -1.0f;
                enemyDissolveAmount_ = 0.0f;
                enemyVisible_ = false;
                SDL_Log("SDL3_GLES stage=enemy_dissolved");
            }
            return;
        }
        if (enemyDeathPlaying_) {
            enemyDeathTime_ += dt;
            const float deathClipDuration = EnemyClipDurationOr(enemyDeathClip_, 1.35f);
            // Keep the final death pose on screen long enough to read even if
            // the source clip is very short or missing on a reduced asset.
            const float deathVisibleDuration = std::max(2.20f, deathClipDuration + 0.85f);
            if (enemyDeathTime_ >= deathVisibleDuration) {
                enemyDeathPlaying_ = false;
                // Hand the corpse to the dissolve phase instead of popping it
                // out of the scene; UpdateDrawInstanceData keeps drawing it
                // while enemyVisible_ stays true.
                enemyDissolveTimer_ = 0.0f;
                enemyDissolveAmount_ = 0.0f;
                SDL_Log("SDL3_GLES stage=enemy_dissolve_start death_duration=%.2f",
                    enemyDeathTime_);
                return;
            }
        }
        UpdateEnemyAnimStateMachine(dt);

        const ohos_model::AnimationClip& clip = enemyClips_[enemyAnimClipIndex_];
        if (clip.duration > 0.0f) {
            if (enemyAnimLoop_) {
                enemyAnimTime_ = std::fmod(enemyAnimTime_ + dt, clip.duration);
            } else {
                enemyAnimTime_ = std::min(enemyAnimTime_ + dt, clip.duration);
            }
        }

        const std::size_t nodeCount = enemyAnimNodes_.size();
        enemyNodeTranslation_.assign(nodeCount * 3, 0.0f);
        enemyNodeRotation_.assign(nodeCount * 4, 0.0f);
        enemyNodeScale_.assign(nodeCount * 3, 1.0f);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                enemyNodeTranslation_[i * 3 + c] = enemyAnimNodes_[i].translation[c];
                enemyNodeScale_[i * 3 + c] = enemyAnimNodes_[i].scale[c];
            }
            for (int c = 0; c < 4; ++c) {
                enemyNodeRotation_[i * 4 + c] = enemyAnimNodes_[i].rotation[c];
            }
        }
        float sampled[4];
        for (const ohos_model::AnimationChannel& channel : clip.channels) {
            if (channel.nodeIndex >= nodeCount) {
                continue;
            }
            SampleChannel(channel, enemyAnimTime_, sampled);
            const std::size_t base = channel.nodeIndex * (channel.path == 1 ? 4u : 3u);
            float* target = channel.path == 0 ? &enemyNodeTranslation_[base]
                : (channel.path == 1 ? &enemyNodeRotation_[base] : &enemyNodeScale_[base]);
            const std::size_t components = channel.path == 1 ? 4u : 3u;
            for (std::size_t c = 0; c < components; ++c) {
                target[c] = sampled[c];
            }
        }

        enemyNodeGlobals_.assign(nodeCount * 16, 0.0f);
        std::vector<float> local(16);
        for (std::size_t i = 0; i < nodeCount; ++i) {
            ComposeTrs(&enemyNodeTranslation_[i * 3], &enemyNodeRotation_[i * 4],
                &enemyNodeScale_[i * 3], local.data());
            const std::int32_t parent = enemyAnimNodes_[i].parent;
            float* global = &enemyNodeGlobals_[i * 16];
            if (parent < 0 || static_cast<std::size_t>(parent) >= nodeCount) {
                for (int c = 0; c < 16; ++c) {
                    global[c] = local[c];
                }
            } else {
                Mat4Multiply16(&enemyNodeGlobals_[static_cast<std::size_t>(parent) * 16],
                    local.data(), global);
            }
        }

        const std::size_t jointCount = std::min(enemySkin_.JointCount(),
            static_cast<std::size_t>(kMaxJoints));
        enemyJointMatrices_.assign(kMaxJoints * 16, 0.0f);
        std::vector<float> skinMatrix(16);
        for (std::size_t j = 0; j < jointCount; ++j) {
            if (j >= enemySkin_.jointNodes.size() ||
                enemySkin_.inverseBindMatrices.size() < (j + 1) * 16) {
                continue;
            }
            const std::size_t nodeIndex = static_cast<std::size_t>(enemySkin_.jointNodes[j]);
            if (nodeIndex >= nodeCount) {
                continue;
            }
            float* out = &enemyJointMatrices_[j * 16];
            Mat4Multiply16(&enemyNodeGlobals_[nodeIndex * 16],
                &enemySkin_.inverseBindMatrices[j * 16], skinMatrix.data());
            Mat4Multiply16(enemyModelNormalise_.value, skinMatrix.data(), out);
        }

        glBindBuffer(GL_UNIFORM_BUFFER, enemyJointBuffer_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0,
            static_cast<GLsizeiptr>(enemyJointMatrices_.size() * sizeof(float)),
            enemyJointMatrices_.data());
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, enemyJointBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    const ohos_model::Material* ResolveMaterial(int32_t materialIndex) const
    {
        if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= materials_.size()) {
            static const ohos_model::Material kFallback{};
            return &kFallback;
        }
        return &materials_[static_cast<size_t>(materialIndex)];
    }

    const ohos_model::Material* ResolveHelmetMaterial(int32_t materialIndex) const
    {
        return ResolveStaticMaterial(helmetMaterials_, materialIndex);
    }

    static const ohos_model::Material* ResolveStaticMaterial(
        const std::vector<ohos_model::Material>& materials, int32_t materialIndex)
    {
        if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= materials.size()) {
            static const ohos_model::Material kFallback{};
            return &kFallback;
        }
        return &materials[static_cast<size_t>(materialIndex)];
    }

    int UploadCharacterInstanceData()
    {
        if (characterInstanceVbo_ == 0) {
            return 0;
        }
        CharacterInstanceData instances[2]{};
        instances[0].model = playerMatrix_;
        instances[0].color[0] = 1.0f;
        instances[0].color[1] = 1.0f;
        instances[0].color[2] = 1.0f;
        // The player keeps the source material color; alpha is marker
        // strength, not material opacity.
        instances[0].color[3] = 0.0f;
        instances[1].model = enemyMatrix_;
        // Enemy marker is fixed red; it no longer uses the previous
        // orange/blue time interpolation.
        instances[1].color[0] = 1.0f;
        instances[1].color[1] = 0.0f;
        instances[1].color[2] = 0.0f;
        instances[1].color[3] = 1.0f;
        glBindBuffer(GL_ARRAY_BUFFER, characterInstanceVbo_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(instances), instances);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return enemyVisible_ ? 2 : 1;
    }

    void DrawShadowCharacterBatch(const std::vector<GlesMesh>& meshes, int instanceCount)
    {
        if (meshes.empty() || instanceCount <= 0 || characterInstanceVbo_ == 0) {
            return;
        }
        glUniform1i(shadowSkinningOnLocation_, 1);
        glUniform1i(shadowInstancingOnLocation_, 1);
        glUniform1f(shadowEnemyDissolveLocation_, enemyDissolveAmount_);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, jointBuffer_);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, enemyJointBuffer_);
        glUniformMatrix4fv(shadowMvpLocation_, 1, GL_FALSE, shadowMatrix_.value);
        for (const GlesMesh& mesh : meshes) {
            if (mesh.indexCount == 0) {
                continue;
            }
            glBindVertexArray(mesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                mesh.indexType, nullptr, instanceCount);
        }
        glBindVertexArray(0);
    }

    void DrawCharacterMeshBatch(const std::vector<GlesMesh>& meshes, int instanceCount)
    {
        if (meshes.empty() || instanceCount <= 0 || characterInstanceVbo_ == 0) {
            return;
        }
        glUniform1i(skinningOnLocation_, 1);
        glUniform1i(instancingOnLocation_, 1);
        glUniform1f(enemyDissolveLocation_, enemyDissolveAmount_);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, jointBuffer_);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, enemyJointBuffer_);
        for (const GlesMesh& mesh : meshes) {
            if (mesh.indexCount == 0) {
                continue;
            }
            const ohos_model::Material* material = ResolveMaterial(mesh.materialIndex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(modelTextures_, material->baseColorImage));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(modelTextures_, material->metallicRoughnessImage,
                    neutralMrTexture_));
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, TextureForImageIn(modelTextures_, material->normalImage));
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, TextureForImageIn(modelTextures_, material->aoImage));
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(modelTextures_, material->emissiveImage));

            glUniform4fv(materialBaseColorFactorLocation_, 1, material->baseColorFactor);
            const float materialParams[4] = {material->metallicFactor,
                material->roughnessFactor, material->normalScale, 0.0f};
            glUniform4fv(materialParamsLocation_, 1, materialParams);
            glUniform1i(hasNormalMapLocation_, material->normalImage >= 0 ? 1 : 0);
            lightingEmissive_[0] = material->emissiveFactor[0];
            lightingEmissive_[1] = material->emissiveFactor[1];
            lightingEmissive_[2] = material->emissiveFactor[2];

            glBindVertexArray(mesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                mesh.indexType, nullptr, instanceCount);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    void DrawShadowMeshList(const std::vector<GlesMesh>& meshes, bool skinned,
        const Mat4& world, GLuint skinBuffer = 0)
    {
        glUniform1i(shadowSkinningOnLocation_, skinned ? 1 : 0);
        glUniform1i(shadowInstancingOnLocation_, 0);
        glUniform1f(shadowEnemyDissolveLocation_, 0.0f);
        if (skinned && skinBuffer != 0) {
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, skinBuffer);
        }
        const Mat4 shadowMvp = Mat4Multiply(shadowMatrix_, world);
        glUniformMatrix4fv(shadowMvpLocation_, 1, GL_FALSE, shadowMvp.value);
        for (const GlesMesh& mesh : meshes) {
            if (mesh.indexCount == 0) {
                continue;
            }
            glBindVertexArray(mesh.vao);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                mesh.indexType, nullptr);
        }
        glBindVertexArray(0);
    }

    // Object draw: uploads the world transform into the per-draw scene UBO
    // slots (offsets 128/192), then binds each mesh's material/textures and
    // draws.  The optional skinning flag lets the enemy reuse the player's
    // already resident animated mesh without duplicating GPU resources.
    void DrawStaticMeshList(const std::vector<GlesMesh>& meshes,
        const std::vector<GLuint>& textures,
        const std::vector<ohos_model::Material>& materials,
        const Mat4& world, bool skinned = false, GLuint skinBuffer = 0)
    {
        glUniform1i(skinningOnLocation_, skinned ? 1 : 0);
        glUniform1i(instancingOnLocation_, 0);
        glUniform1f(enemyDissolveLocation_, 0.0f);
        if (skinned && skinBuffer != 0) {
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, skinBuffer);
        }
        const Mat4 mvp = Mat4Multiply(viewProj_, world);
        glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer_);
        glBufferSubData(GL_UNIFORM_BUFFER, 128, sizeof(Mat4), mvp.value);
        glBufferSubData(GL_UNIFORM_BUFFER, 192, sizeof(Mat4), world.value);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        for (const GlesMesh& mesh : meshes) {
            if (mesh.indexCount == 0) {
                continue;
            }
            const ohos_model::Material* material =
                ResolveStaticMaterial(materials, mesh.materialIndex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(textures, material->baseColorImage));
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(textures, material->metallicRoughnessImage,
                    neutralMrTexture_));
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(textures, material->normalImage));
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(textures, material->aoImage));
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D,
                TextureForImageIn(textures, material->emissiveImage));

            glUniform4fv(materialBaseColorFactorLocation_, 1, material->baseColorFactor);
            const float materialParams[4] = {material->metallicFactor,
                material->roughnessFactor, material->normalScale, 0.0f};
            glUniform4fv(materialParamsLocation_, 1, materialParams);
            glUniform1i(hasNormalMapLocation_, material->normalImage >= 0 ? 1 : 0);

            glBindVertexArray(mesh.vao);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount),
                mesh.indexType, nullptr);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    // Sampling an unbound texture is undefined, so every missing image falls
    // back to a neutral bound texture (white, or the caller-provided one --
    // metallicRoughness passes the neutral dielectric instead).
    GLuint TextureForImage(int32_t imageIndex, GLuint fallback = 0) const
    {
        return TextureForImageIn(modelTextures_, imageIndex, fallback);
    }

    // TextureForImage against an arbitrary texture pool (helmet prop has its own).
    GLuint TextureForImageIn(const std::vector<GLuint>& textures, int32_t imageIndex,
        GLuint fallback = 0) const
    {
        const GLuint neutral = fallback != 0 ? fallback : whiteTexture_;
        if (imageIndex < 0 || static_cast<size_t>(imageIndex) >= textures.size()) {
            return neutral;
        }
        const GLuint texture = textures[static_cast<size_t>(imageIndex)];
        return texture != 0 ? texture : neutral;
    }

    void UpdateEnemyAi(float dt, int pressed)
    {
        const auto TickCombatTimer = [dt](float& timer) {
            if (timer > 0.0f) {
                timer = std::max(0.0f, timer - dt);
            }
        };
        TickCombatTimer(enemyAttackCooldown_);
        TickCombatTimer(playerAttackCooldown_);
        TickCombatTimer(playerHitLockTimer_);
        TickCombatTimer(playerHitFlashTimer_);
        TickCombatTimer(enemyHitLockTimer_);
        TickCombatTimer(playerAttackInvulnerability_);
        TickCombatTimer(playerHitInvulnerability_);
        TickCombatTimer(enemyHitInvulnerability_);
        playerAttackAcceptedThisFrame_ = false;
        enemyDesiredVelocityX_ = 0.0f;
        enemyDesiredVelocityZ_ = 0.0f;

        // The attack animation and the damage query share one admission edge.
        // The minimum also prevents a reduced/malformed asset from making the
        // player attack faster than the intended gameplay cadence.
        if ((pressed & 1) != 0 && playerAttackCooldown_ <= 0.0f &&
            playerHitLockTimer_ <= 0.0f && playerHealth_ > 0.0f) {
            const float clipDuration = ClipDurationOr(attackClip_, 0.87f);
            playerAttackCooldown_ = std::max(kMinimumPlayerAttackCooldown, clipDuration);
            playerAttackInvulnerability_ = kPlayerAttackInvulnerability;
            playerAttackAcceptedThisFrame_ = true;
        }

        float toEnemyX = enemyX_ - playerX_;
        float toEnemyZ = enemyZ_ - playerZ_;
        float distance = std::sqrt(toEnemyX * toEnemyX + toEnemyZ * toEnemyZ);
        if (playerAttackAcceptedThisFrame_ && enemyAlive_ && distance <= 1.75f &&
            enemyHitInvulnerability_ <= 0.0f) {
            enemyHealth_ = std::max(0.0f, enemyHealth_ - 25.0f);
            AudioManager::GetInstance().PlayAudio("hit", 0.55f);
            enemyHitInvulnerability_ = kHitInvulnerability;
            if (enemyHealth_ <= 0.0f) {
                enemyAlive_ = false;
                enemyMoving_ = false;
                enemyAttackRequested_ = false;
                enemyDeathPlaying_ = true;
                enemyVisible_ = true;
                enemyDeathTime_ = 0.0f;
                SDL_Log("SDL3_GLES stage=enemy_defeated");
            } else {
                enemyHitLockTimer_ = kPlayerHitDuration;
                SDL_Log("SDL3_GLES stage=enemy_hit health=%.0f/%.0f",
                    enemyHealth_, enemyMaxHealth_);
            }
        }
        if (!enemyAlive_) {
            return;
        }
        if (enemyHitLockTimer_ > 0.0f) {
            // Mikan PuppetEnemyScript: during hit-stun the AI neither moves
            // nor attacks, so the Hit one-shot cannot be overridden.
            return;
        }

        toEnemyX = playerX_ - enemyX_;
        toEnemyZ = playerZ_ - enemyZ_;
        distance = std::sqrt(toEnemyX * toEnemyX + toEnemyZ * toEnemyZ);
        if (distance > 1.0e-4f) {
            enemyYaw_ = std::atan2(toEnemyX, toEnemyZ);
        }
        enemyMoving_ = false;
        if (distance > 1.45f) {
            const float invDistance = 1.0f / std::max(distance, 1.0e-4f);
            constexpr float kEnemySpeed = 0.55f;
            enemyDesiredVelocityX_ = toEnemyX * invDistance * kEnemySpeed;
            enemyDesiredVelocityZ_ = toEnemyZ * invDistance * kEnemySpeed;
            if (physicsWorld_ == nullptr || !physicsWorld_->IsReady()) {
                enemyX_ += enemyDesiredVelocityX_ * dt;
                enemyZ_ += enemyDesiredVelocityZ_ * dt;
                constexpr float kGroundBound = 8.5f;
                enemyX_ = std::max(-kGroundBound, std::min(kGroundBound, enemyX_));
                enemyZ_ = std::max(-kGroundBound, std::min(kGroundBound, enemyZ_));
            }
            enemyMoving_ = true;
        } else if (enemyAttackCooldown_ <= 0.0f) {
            constexpr float kEnemyAttackCooldown = 1.15f;
            enemyAttackCooldown_ = kEnemyAttackCooldown;
            enemyAttackRequested_ = true;
            if (playerAttackInvulnerability_ <= 0.0f &&
                playerHitInvulnerability_ <= 0.0f) {
                playerHealth_ = std::max(0.0f, playerHealth_ - 8.0f);
                AudioManager::GetInstance().PlayAudio("hit",
                    playerHealth_ <= 0.0f ? 0.95f : 0.70f);
                playerHitInvulnerability_ = kHitInvulnerability;
                playerHitLockTimer_ = kPlayerHitDuration;
                playerHitFlashTimer_ = kHitFlashDuration;
                SDL_Log("SDL3_GLES stage=enemy_attack player_health=%.0f/%.0f",
                    playerHealth_, playerMaxHealth_);
                if (playerHealth_ <= 0.0f) {
                    playerMoving_ = false;
                    playerSprinting_ = false;
                    enemyMoving_ = false;
                    enemyDesiredVelocityX_ = 0.0f;
                    enemyDesiredVelocityZ_ = 0.0f;
                    enemyAttackRequested_ = false;
                    cameraInput_.actionButtonPressedMask = 0;
                    uiScreen_ = rhi::UiScreen::GameplayRetry;
                    SDL_Log("SDL3_GLES stage=player_defeated screen=retry");
                    return;
                }
            } else {
                SDL_Log("SDL3_GLES stage=enemy_attack_blocked reason=player_iframe");
            }
        }
    }

    bool UpdateSceneUniforms()
    {
        if (uniformBuffer_ == 0 || height_ == 0) {
            return false;
        }
        const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
        float fieldOfViewDegrees = 60.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
        if (sceneDefinition_ != nullptr && sceneDefinition_->mainCamera.present) {
            fieldOfViewDegrees = sceneDefinition_->mainCamera.fieldOfViewDegrees;
            nearPlane = sceneDefinition_->mainCamera.nearPlane;
            farPlane = sceneDefinition_->mainCamera.farPlane;
        }
        const Mat4 projection = Mat4PerspectiveGl(
            fieldOfViewDegrees * 3.14159265358979323846f / 180.0f,
            aspect, nearPlane, farPlane);
        // Touch camera.  On-device rounds settled the signs empirically: with
        // raw (unflipped) finger deltas, yaw answers drag right by turning the
        // view right only with "+", while pitch wants "+ drag down" (MikanEngine's
        // ThirdPersonCameraSystem uses the mirrored pair because its input
        // pipeline differs; do not "re-align" this without a real-finger test).
        constexpr float kLookSensitivity = 0.0055f;   // radians per pixel
        constexpr float kZoomWorldPerPixel = 0.004f;  // pinch spread -> units
        // Menu/settings backdrop: slow cinematic orbit while the menus are up.
        // dt from the frame-time EMA (the real dt is only defined further down
        // in the player-movement block).
        if (uiScreen_ == rhi::UiScreen::MainMenu ||
            uiScreen_ == rhi::UiScreen::Settings) {
            const float orbitDt =
                fpsAvgMs_ > 0.0f && fpsAvgMs_ < 1000.0f ? fpsAvgMs_ * 0.001f : 0.016f;
            camYaw_ += orbitDt * 0.12f;
        }
        const bool gameplayInput = uiScreen_ == rhi::UiScreen::Gameplay;
        if (gameplayInput) {
            camYaw_ += cameraInput_.lookDeltaX * kLookSensitivity * lookSensitivityScale_;
            camPitch_ += cameraInput_.lookDeltaY * kLookSensitivity * lookSensitivityScale_;
        }
        camPitch_ = camPitch_ > 1.35f ? 1.35f : (camPitch_ < -1.35f ? -1.35f : camPitch_);
        if (gameplayInput) {
            camDistance_ -= cameraInput_.zoomDelta * kZoomWorldPerPixel;
        }
        camDistance_ = camDistance_ < 1.2f ? 1.2f : (camDistance_ > 6.0f ? 6.0f : camDistance_);
        // Zoom diagnostics (rate-limited): confirms on-device whether pinch
        // spread reaches the renderer and how far the distance actually moves.
        if (gameplayInput && std::fabs(cameraInput_.zoomDelta) > 0.5f) {
            const Uint64 zoomLogTicks = SDL_GetTicks();
            if (zoomLogTicks - zoomLastLogTicks_ > 500) {
                zoomLastLogTicks_ = zoomLogTicks;
                SDL_Log("SDL3_GLES stage=zoom delta=%.1f dist=%.2f",
                    cameraInput_.zoomDelta, camDistance_);
            }
        }
        // Camera-to-world rotation = inverse of the view rotation below.
        const Mat4 camToWorld =
            Mat4Multiply(Mat4RotationY(-camYaw_), Mat4RotationX(-camPitch_));

        // ---- third-person player movement (joystick, camera-relative) ----
        float dt = 0.016f;
        if (moveLastTicks_ != 0) {
            const float dtMs = static_cast<float>(SDL_GetTicks() - moveLastTicks_);
            if (dtMs > 0.0f && dtMs < 100.0f) {
                dt = dtMs * 0.001f;
            }
        }
        moveLastTicks_ = SDL_GetTicks();
        float stickMag = gameplayInput ? std::sqrt(cameraInput_.moveX * cameraInput_.moveX
            + cameraInput_.moveY * cameraInput_.moveY) : 0.0f;
        if (stickMag > 1.0f) {
            stickMag = 1.0f;
        }
        const bool physicsReady = physicsWorld_ != nullptr && physicsWorld_->IsReady();
        if (physicsReady && gameplayInput && !physicsReset_) {
            physicsWorld_->Reset(
                playerX_, playerY_ - physics::kPlayerVisualOriginOffset, playerZ_,
                enemyX_, enemyY_ - (physics::kPlayerCapsuleHalfHeight
                    + physics::kPlayerCapsuleRadius)
                    - physics::kGameplayCharacterPadding,
                enemyZ_);
            physicsReset_ = true;
        }
        float desiredVelocityX = 0.0f;
        float desiredVelocityZ = 0.0f;
        bool hasMovementDirection = false;
        // Mikan damageLocked: a hit-stunned player cannot steer, jump or
        // sprint until the lock expires.
        const bool playerLocked = playerHitLockTimer_ > 0.0f;
        if (stickMag > 0.15f && !playerLocked) {
            // Ground-plane camera basis: camToWorld's z column points from the
            // target back to the camera, so forward (into the screen) is its
            // negation; the x column is right.  Stick y is screen-down.
            float fx = -camToWorld.value[8];
            float fz = -camToWorld.value[10];
            const float forwardLen = std::sqrt(fx * fx + fz * fz);
            if (forwardLen > 1e-4f) {
                fx /= forwardLen;
                fz /= forwardLen;
            } else {
                fx = 0.0f;
                fz = -1.0f;
            }
            float dirX = camToWorld.value[0] * cameraInput_.moveX + fx * -cameraInput_.moveY;
            float dirZ = camToWorld.value[2] * cameraInput_.moveX + fz * -cameraInput_.moveY;
            const float dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (dirLen > 1e-4f) {
                dirX /= dirLen;
                dirZ /= dirLen;
                constexpr float kWalkSpeed = 1.3f;    // world units/s at full stick
                constexpr float kSprintSpeed = 2.6f;  // RUN held: 2x walk
                playerSprinting_ = cameraInput_.actionButtonHeld[2] && !crouchEnabled_;
                const float speed = playerSprinting_ ? kSprintSpeed : kWalkSpeed;
                desiredVelocityX = dirX * speed * stickMag;
                desiredVelocityZ = dirZ * speed * stickMag;
                hasMovementDirection = true;
                if (!physicsReady) {
                    playerX_ += desiredVelocityX * dt;
                    playerZ_ += desiredVelocityZ * dt;
                    // Keep the character on the 20x20 ground slab (half extent
                    // 10, a margin for the character's ~0.5 footprint).
                    constexpr float kGroundBound = 8.5f;
                    playerX_ = playerX_ > kGroundBound ? kGroundBound
                        : (playerX_ < -kGroundBound ? -kGroundBound : playerX_);
                    playerZ_ = playerZ_ > kGroundBound ? kGroundBound
                        : (playerZ_ < -kGroundBound ? -kGroundBound : playerZ_);
                    playerY_ = 0.0f;
                }
                // Face the movement direction: the model-space front is +Z, so
                // yaw = atan2(x, z).  Shortest-arc capped turn for smoothness.
                const float targetYaw = std::atan2(dirX, dirZ);
                float diff = targetYaw - playerYaw_;
                constexpr float kPi = 3.14159265358979323846f;
                while (diff > kPi) {
                    diff -= 2.0f * kPi;
                }
                while (diff < -kPi) {
                    diff += 2.0f * kPi;
                }
                constexpr float kTurnSpeed = 12.0f;  // rad/s cap
                const float maxStep = kTurnSpeed * dt;
                playerYaw_ += diff > maxStep ? maxStep : (diff < -maxStep ? -maxStep : diff);
            }
        }
        if (physicsReady && gameplayInput) {
            physicsWorld_->MovePlayer(playerLocked ? 0.0f : desiredVelocityX,
                playerLocked ? 0.0f : desiredVelocityZ, dt,
                !playerLocked && (cameraInput_.actionButtonPressedMask & 2) != 0);
            const physics::CharacterState state = physicsWorld_->GetPlayerState();
            playerX_ = state.x;
            playerY_ = state.y + physics::kPlayerVisualOriginOffset;
            playerZ_ = state.z;
        } else if (!physicsReady && gameplayInput && !hasMovementDirection) {
            playerY_ = 0.0f;
        }
        playerMoving_ = stickMag > 0.15f && !playerLocked;

        // The enemy is an independent actor: it approaches the player, takes
        // attack damage, and periodically strikes back when in range.  The
        // action mask is consumed later by the player's animation state
        // machine, so both render paths observe the same attack edge.
        if (gameplayInput) {
            UpdateEnemyAi(dt, cameraInput_.actionButtonPressedMask);
            if (uiScreen_ == rhi::UiScreen::Gameplay && physicsReady) {
                physicsWorld_->MoveEnemy(enemyDesiredVelocityX_, enemyDesiredVelocityZ_, dt);
                const physics::CharacterState state = physicsWorld_->GetEnemyState();
                enemyX_ = state.x;
                enemyY_ = state.y + (physics::kPlayerCapsuleHalfHeight
                    + physics::kPlayerCapsuleRadius)
                    + physics::kGameplayCharacterPadding;
                enemyZ_ = state.z;
                enemyMoving_ = enemyAlive_ &&
                    std::sqrt(state.velocityX * state.velocityX + state.velocityZ * state.velocityZ) > 0.05f;
            }
        }

        // Camera soft-follows the player in all axes so jumps move the orbit
        // target too (exponential, frame-rate independent).
        if (gameplayInput) {
            const float followLerp = 1.0f - std::exp(-8.0f * dt);
            camPanX_ += (playerX_ - camPanX_) * followLerp;
            camPanY_ += (playerY_ - camPanY_) * followLerp;
            camPanZ_ += (playerZ_ - camPanZ_) * followLerp;
        }

        const Mat4 view = Mat4Multiply(Mat4Translation(0.0f, 0.0f, -camDistance_),
            Mat4Multiply(Mat4RotationX(camPitch_),
                Mat4Multiply(Mat4RotationY(camYaw_),
                    Mat4Translation(-camPanX_, -camPanY_, -camPanZ_))));
        // World-space camera position = pan + camToWorld * (0, 0, distance);
        // column-major columns 2 and 3 are the z basis and translation.
        camPosX_ = camPanX_ + camToWorld.value[8] * camDistance_ + camToWorld.value[12];
        camPosY_ = camPanY_ + camToWorld.value[9] * camDistance_ + camToWorld.value[13];
        camPosZ_ = camPanZ_ + camToWorld.value[10] * camDistance_ + camToWorld.value[14];
        // Player world transform: stand on the ground line at the moved
        // position, facing the last movement direction.
        const Mat4 modelFacing = Mat4Multiply(
            Mat4Translation(playerX_, playerY_, playerZ_), Mat4RotationY(playerYaw_));
        playerMatrix_ = modelFacing;
        enemyMatrix_ = Mat4Multiply(Mat4Translation(enemyX_, enemyY_, enemyZ_),
            Mat4RotationY(enemyYaw_));
        shadowMatrix_ = Mat4Multiply(Mat4ShadowProjection(), Mat4ShadowView());

        SceneUniforms uniforms{};
        uniforms.skyMvp = projection;
        uniforms.modelMvp = Mat4Multiply(projection, Mat4Multiply(view, modelFacing));
        uniforms.modelMatrix = modelFacing;
        // The deferred lighting pass reconstructs world positions from depth, so
        // it needs the inverse of proj*view -- model transform excluded, since
        // the G-buffer geometry already carries it via modelMatrix.
        const Mat4 viewProj = Mat4Multiply(projection, view);
        uniforms.viewProj = viewProj;
        invViewProj_ = Mat4Invert(viewProj);
        // Static props (helmet) need the same viewProj for their per-draw
        // modelMvp upload inside the geometry pass.
        viewProj_ = viewProj;

        glBindBuffer(GL_UNIFORM_BUFFER, uniformBuffer_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(uniforms), &uniforms);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        return true;
    }

    static constexpr const char* kModelPath =
        "models/Quaternius/AnimationLibrary_Standard.glb";
    static constexpr const char* kHelmetPath =
        "models/DamagedHelmet/glTF/DamagedHelmet.gltf";
    static constexpr const char* kCubePath =
        "models/BaseModel/cube.glb";
    // Skin UBO capacity: 256 mat4 = 16 KiB, the ES 3.0 minimum
    // MAX_UNIFORM_BLOCK_SIZE (matches the shader's SkinUniforms array).
    static constexpr int kMaxJoints = 256;

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool sceneReady_ = false;
    bool firstFrameLogged_ = false;

    GLuint irradianceProgram_ = 0;
    GLuint prefilterProgram_ = 0;
    GLuint gbufferProgram_ = 0;
    GLuint shadowProgram_ = 0;
    GLuint lightingProgram_ = 0;
    GLuint shadowFbo_ = 0;
    GLuint shadowDepthTexture_ = 0;
    bool shadowReady_ = false;
    // Deferred MRT G-buffer target (see CreateGBufferTarget for the layout).
    GLuint gbufferFbo_ = 0;
    GLuint gbufferAlbedoTex_ = 0;
    GLuint gbufferNormalTex_ = 0;
    GLuint gbufferMaterialTex_ = 0;
    GLuint gbufferEmissiveTex_ = 0;
    GLuint gbufferDepthTex_ = 0;
    uint32_t gbufferWidth_ = 0;
    uint32_t gbufferHeight_ = 0;
    // Post-processing chain: lighting writes postColorTex_, FXAA resolves it
    // to the default framebuffer (see CreatePostTarget).
    GLuint fxaaProgram_ = 0;
    GLint fxaaSampler_ = -1;
    GLuint postFbo_ = 0;
    GLuint postColorTex_ = 0;
    uint32_t postWidth_ = 0;
    uint32_t postHeight_ = 0;
    // Bloom + AgX tonemap chain (MikanEngine postprocess_chain_mobile port).
    // ds[0] carries the soft-knee threshold output, ds[1..5] the dual-kernel
    // downsamples; up[i] has ds[i]'s size and fuses ds[i] with up[i+1].
    static constexpr int kBloomDsLevels = 6;
    static constexpr int kBloomUpLevels = 5;
    bool hdrPipeline_ = false;
    GLint lightingHdrOutputLocation_ = -1;
    GLuint bloomThProgram_ = 0;
    GLuint bloomDownProgram_ = 0;
    GLuint bloomUpProgram_ = 0;
    GLuint tonemapProgram_ = 0;
    GLint bloomThSampler_ = -1;
    GLint bloomThParams_ = -1;
    GLint bloomDownSampler_ = -1;
    GLint bloomUpSampler_ = -1;
    GLint bloomUpPrevSampler_ = -1;
    GLint tonemapBloomSampler_ = -1;
    GLint tonemapCompSampler_ = -1;
    GLint tonemapHdrMode_ = -1;
    GLint tonemapExposure_ = -1;
    GLint tonemapBloomStrength_ = -1;
    GLuint bloomDsTex_[kBloomDsLevels] = {};
    GLuint bloomDsFbo_[kBloomDsLevels] = {};
    GLuint bloomUpTex_[kBloomUpLevels] = {};
    GLuint bloomUpFbo_[kBloomUpLevels] = {};
    GLsizei bloomDsW_[kBloomDsLevels] = {};
    GLsizei bloomDsH_[kBloomDsLevels] = {};
    GLuint tonemapTex_ = 0;
    GLuint tonemapFbo_ = 0;
    // Inverse proj*view for depth -> world reconstruction, refreshed per frame.
    Mat4 invViewProj_;
    Mat4 viewProj_;  // proj*view, kept for per-prop modelMvp uploads
    Mat4 playerMatrix_;
    Mat4 enemyMatrix_;
    Mat4 shadowMatrix_;
    // Emissive factor captured from the geometry pass materials.
    float lightingEmissive_[3] = {1.0f, 1.0f, 1.0f};
    // Vertex array for the lighting full-screen triangle.
    GLuint fullscreenVao_ = 0;
    GLint lightingAlbedoSampler_ = -1;
    GLint lightingNormalSampler_ = -1;
    GLint lightingMaterialSampler_ = -1;
    GLint lightingEmissiveSampler_ = -1;
    GLint lightingDepthSampler_ = -1;
    GLint lightingIrradianceSampler_ = -1;
    GLint lightingEnvSampler_ = -1;
    GLint lightingBrdfLutSampler_ = -1;
    GLint lightingSkyboxSampler_ = -1;
    GLint lightingShadowSampler_ = -1;
    GLint lightingInvViewProjLocation_ = -1;
    GLint lightingShadowMatrixLocation_ = -1;
    GLint lightingCameraPositionLocation_ = -1;
    GLint lightingEmissiveFactorLocation_ = -1;
    GLint lightingMaxLodLocation_ = -1;
    GLint lightingEnvPrefilteredLocation_ = -1;
    GLint lightingUseBrdfLutLocation_ = -1;
    GLint lightingUseShadowMapLocation_ = -1;
    GLint lightingDebugNormalLocation_ = -1;
    GLint irradianceSampler_ = -1;
    GLint irradianceFaceLocation_ = -1;
    GLint prefilterFaceLocation_ = -1;
    GLint prefilterSampler_ = -1;
    GLint prefilterRoughnessLocation_ = -1;
    GLint modelBaseColorSampler_ = -1;
    GLint modelMrSampler_ = -1;
    GLint modelNormalSampler_ = -1;
    GLint modelAoSampler_ = -1;
    GLint modelEmissiveSampler_ = -1;
    GLint cameraPositionLocation_ = -1;
    GLint materialParamsLocation_ = -1;
    GLint materialBaseColorFactorLocation_ = -1;
    GLint hasNormalMapLocation_ = -1;
    GLint skyboxMaxLodLocation_ = -1;
    float skyboxMaxLod_ = 0.0f;
    int skyboxSize_ = 0;
    GLuint uniformBuffer_ = 0;
    // Skeletal animation state (glTF skin + clips owned by the renderer).
    GLuint jointBuffer_ = 0;
    GLuint enemyJointBuffer_ = 0;
    GLuint characterInstanceVbo_ = 0;
    GLint skinningOnLocation_ = -1;
    GLint instancingOnLocation_ = -1;
    GLint enemyDissolveLocation_ = -1;
    GLint shadowMvpLocation_ = -1;
    GLint shadowSkinningOnLocation_ = -1;
    GLint shadowInstancingOnLocation_ = -1;
    GLint shadowEnemyDissolveLocation_ = -1;
    bool skinningOn_ = false;
    std::vector<ohos_model::Node> animNodes_;
    ohos_model::Skin skin_;
    std::vector<ohos_model::AnimationClip> clips_;
    int animClipIndex_ = -1;
    float animTime_ = 0.0f;
    Uint64 animLastTicks_ = 0;
    // Animation state machine (see UpdateAnimStateMachine; ported from
    // MikanEngine's PlayerWalkScript).
    AnimState animState_ = AnimState::Idle;
    float animStateTime_ = 0.0f;
    bool animLoop_ = true;
    int walkClip_ = -1;
    int sprintClip_ = -1;
    int idleClip_ = -1;
    int crouchClip_ = -1;
    bool crouchEnabled_ = false;
    int attackClip_ = -1;
    int jumpStartClip_ = -1;
    int jumpLoopClip_ = -1;
    int jumpLandClip_ = -1;
    int deathClip_ = -1;
    int hitClip_ = -1;
    Mat4 modelNormalise_;
    // Per-frame scratch (avoid reallocating every frame).
    std::vector<float> nodeTranslation_;
    std::vector<float> nodeRotation_;
    std::vector<float> nodeScale_;
    std::vector<float> nodeGlobals_;
    std::vector<float> jointMatrices_;
    std::vector<ohos_model::Node> enemyAnimNodes_;
    ohos_model::Skin enemySkin_;
    std::vector<ohos_model::AnimationClip> enemyClips_;
    int enemyAnimClipIndex_ = -1;
    float enemyAnimTime_ = 0.0f;
    Uint64 enemyAnimLastTicks_ = 0;
    AnimState enemyAnimState_ = AnimState::Idle;
    float enemyAnimStateTime_ = 0.0f;
    bool enemyAnimLoop_ = true;
    int enemyWalkClip_ = -1;
    int enemySprintClip_ = -1;
    int enemyIdleClip_ = -1;
    int enemyCrouchClip_ = -1;
    int enemyAttackClip_ = -1;
    int enemyJumpStartClip_ = -1;
    int enemyJumpLoopClip_ = -1;
    int enemyJumpLandClip_ = -1;
    int enemyDeathClip_ = -1;
    int enemyHitClip_ = -1;
    Mat4 enemyModelNormalise_;
    std::vector<float> enemyNodeTranslation_;
    std::vector<float> enemyNodeRotation_;
    std::vector<float> enemyNodeScale_;
    std::vector<float> enemyNodeGlobals_;
    std::vector<float> enemyJointMatrices_;
    GLuint skyboxVao_ = 0;
    GLuint skyboxTexture_ = 0;
    // 32x32 cosine-convolved skybox; on creation failure it aliases
    // skyboxTexture_, so only delete it when it differs.
    GLuint irradianceTexture_ = 0;
    GLuint prefilteredTexture_ = 0;
    GLuint brdfLutTexture_ = 0;
    // Touch camera state; updated from cameraInput_ every UpdateSceneUniforms.
    rhi::CameraInput cameraInput_{};
    const scene::Definition* sceneDefinition_ = nullptr;
    physics::JoltGameplayPhysics* physicsWorld_ = nullptr;
    bool physicsReset_ = false;
    float camYaw_ = 0.0f;
    float camPitch_ = 0.0f;
    float camDistance_ = kCameraDistance;
    float camPanX_ = 0.0f;
    float camPanY_ = 0.0f;
    float camPanZ_ = 0.0f;
    // Third-person player state: ground position, facing yaw and the move
    // flag the animation state machine reads (see UpdateSceneUniforms).
    float playerX_ = 0.0f;
    float playerY_ = 0.0f;
    float playerZ_ = 0.0f;
    float playerYaw_ = 0.0f;
    bool playerMoving_ = false;
    float playerMaxHealth_ = 100.0f;
    float playerHealth_ = 100.0f;
    float playerAttackCooldown_ = 0.0f;
    float playerAttackInvulnerability_ = 0.0f;
    float playerHitInvulnerability_ = 0.0f;
    // Mikan hit-stun port: brief control lock + red screen flash.
    float playerHitLockTimer_ = 0.0f;
    float playerHitFlashTimer_ = 0.0f;
    bool playerAttackAcceptedThisFrame_ = false;
    float enemyX_ = 1.35f;
    float enemyY_ = 0.0f;
    float enemyZ_ = -0.65f;
    float enemyYaw_ = 0.0f;
    float enemyMaxHealth_ = 100.0f;
    float enemyHealth_ = 100.0f;
    float enemyAttackCooldown_ = 0.0f;
    float enemyHitInvulnerability_ = 0.0f;
    // Mikan PuppetEnemyScript hit-stun: AI cannot override the Hit one-shot.
    float enemyHitLockTimer_ = 0.0f;
    bool enemyAlive_ = true;
    bool enemyVisible_ = true;
    bool enemyDeathPlaying_ = false;
    float enemyDeathTime_ = 0.0f;
    // Death dissolve: after the death pose has been read, the corpse burns
    // away over kEnemyDissolveDuration instead of popping out of the scene.
    // Timer < 0 means inactive; enemyDissolveAmount_ feeds the shaders.
    static constexpr float kEnemyDissolveDuration = 1.2f;
    float enemyDissolveTimer_ = -1.0f;
    float enemyDissolveAmount_ = 0.0f;
    bool enemyMoving_ = false;
    bool enemyAttackRequested_ = false;
    float enemyDesiredVelocityX_ = 0.0f;
    float enemyDesiredVelocityZ_ = 0.0f;
    // Top-level app screen + menu-owned settings.  The renderer owns this
    // state; the main loop feeds it back to the touch layer via
    // IRenderer::CurrentScreen so input routing follows the screen.
    rhi::UiScreen uiScreen_ = rhi::UiScreen::MainMenu;
    bool settingFps_ = false;             // debug HUD (FPS + anim state text)
    int sensIndex_ = 1;                   // index into kSensitivityChoices

    // Render-resolution multiplier on top of kRenderWidth/Height (1080p base).
    // Applied by destroying the scene targets; the gameplay path lazily
    // rebuilds them at the new scale (menus never touch the targets).
    bool bloomEnabled_ = true;            // runtime bloom chain toggle
    float renderScale_ = 1.0f;            // index into kScaleChoices
    inventory::State inventory_;          // bag grid + equipment slots
    int resIndex_ = 0;
    static constexpr float kScaleChoices[4] = {1.0f, 0.75f, 0.5f, 0.25f};
    float lookSensitivityScale_ = 1.0f;
    static constexpr float kSensitivityChoices[3] = {0.5f, 1.0f, 2.0f};
    // Global UI alpha multiplier: every overlay primitive scales its fill
    // alpha by this factor, so one setting tunes the whole interface while
    // each element keeps its relative hierarchy.
    float uiOpacity_ = 0.80f;             // default 80%
    int uiOpacityIndex_ = 1;              // index into kOpacityChoices
    static constexpr float kOpacityChoices[5] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
    // Held RUN button (action button 2): read in UpdateSceneUniforms to scale
    // the walk speed and by the animation state machine to pick Sprint_Loop.
    bool playerSprinting_ = false;
    Uint64 moveLastTicks_ = 0;
    Uint64 zoomLastLogTicks_ = 0;
    float camPosX_ = 0.0f;
    float camPosY_ = 0.0f;
    float camPosZ_ = kCameraDistance;
    GLuint overlayProgram_ = 0;
    GLuint overlayVao_ = 0;
    GLuint overlayVbo_ = 0;
    GLint overlayResolutionLocation_ = -1;
    GLint overlayColorLocation_ = -1;
    static constexpr int kOverlaySegments = 48;
    int rectIndexCount_ = 0;  // vertices of the last UploadRoundedRect
    static constexpr int kOverlayFanCount = kOverlaySegments + 2;
    GLuint iconProgram_ = 0;
    GLuint iconVao_ = 0;
    GLuint iconVbo_ = 0;
    GLint iconResolutionLocation_ = -1;
    GLint iconSamplerLocation_ = -1;
    GLint iconColorLocation_ = -1;
    GLuint actionIconTextures_[rhi::CameraInput::kActionButtonCount] = {};
    // SDF text overlay state.
    GLuint textProgram_ = 0;
    GLuint textVao_ = 0;
    GLuint textVbo_ = 0;
    GLint textResolutionLocation_ = -1;
    GLint textPxRangeLocation_ = -1;
    GLint textSamplerLocation_ = -1;
    GLuint textAtlasTexture_ = 0;
    Uint64 fpsLastTicks_ = 0;
    float fpsAvgMs_ = 0.0f;
    // 1x1 opaque white, used whenever a mesh has no resolvable base colour
    // texture: sampling an unbound texture is undefined, this is not.
    GLuint whiteTexture_ = 0;
    // 1x1 neutral metallicRoughness (roughness 1, metallic 0).
    GLuint neutralMrTexture_ = 0;
    // Shared by UpdateSceneUniforms (view translation) and DrawOnce (the PBR
    // cameraPosition uniform): the view is a pure -z translation of this.
    static constexpr float kCameraDistance = 2.8f;
    static constexpr float kMinimumPlayerAttackCooldown = 0.75f;
    static constexpr float kPlayerAttackInvulnerability = 0.20f;
    static constexpr float kHitInvulnerability = 0.45f;
    // Mikan PlayerWalkScript hit-stun constants (hitDuration / flash 0.18 s).
    static constexpr float kPlayerHitDuration = 0.333f;
    static constexpr float kHitFlashDuration = 0.18f;

    std::vector<GlesMesh> meshes_;
    std::vector<GLuint> modelTextures_;
    std::vector<ohos_model::Material> materials_;
    // Static helmet prop (LoadHelmetModel).
    std::vector<GlesMesh> helmetMeshes_;
    std::vector<GLuint> helmetTextures_;
    std::vector<ohos_model::Material> helmetMaterials_;
    // Ground slab (stretched Base Model cube) static geometry.
    std::vector<GlesMesh> groundMeshes_;
    std::vector<GLuint> groundTextures_;
    std::vector<ohos_model::Material> groundMaterials_;
};

} // namespace

namespace rhi {

IRenderer* CreateGlesRenderer()
{
    return new GlesRenderer();
}

} // namespace rhi
