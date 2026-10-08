#define LOG_DOMAIN 0xD001530
#define LOG_TAG "NativeVulkan"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>

#ifndef VK_USE_PLATFORM_OHOS
#define VK_USE_PLATFORM_OHOS 1
#endif
#include <vulkan/vulkan.h>

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_log.h"
#include "SDL3/SDL_stdinc.h"
#include "SDL3/SDL_timer.h"
#include "SDL3/SDL_video.h"
#include "SDL3/SDL_vulkan.h"

#include "inventory.h"
#include "audio/audio_manager.h"
#include "model_loader.h"
#include "physics/jolt_gameplay_physics.h"
#include "rhi/rhi.h"
#include "rhi/sdf_font_metrics.h"
#include "scene/scene_definition.h"
#include "scene_shaders_spv_vulkan.h"

#define STBI_NO_STDIO
// The skybox is six JPEGs, and the glTF sample textures are PNGs.  Both
// decoders have to stay enabled: each STBI_ONLY_x only suppresses the
// matching STBI_NO_x, so listing them together keeps exactly these two.
// The implementation itself lives in stb_image_impl.cpp so this renderer is
// not the sole owner of the decoder symbols.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

extern "C" int OHOS_ReadRawFile(const char* path, void** data, size_t* size);
extern "C" void OHOS_FreeRawFile(void* data);

namespace {

using native_vulkan_scene::kSceneCubeFragSpv;
using native_vulkan_scene::kSceneCubeFragSpvWordCount;
using native_vulkan_scene::kSceneCubeVertSpv;
using native_vulkan_scene::kSceneCubeVertSpvWordCount;
using native_vulkan_scene::kSceneModelFragSpv;
using native_vulkan_scene::kSceneModelFragSpvWordCount;
using native_vulkan_scene::kSceneModelVertSpv;
using native_vulkan_scene::kSceneModelVertSpvWordCount;
using native_vulkan_scene::kSceneShadowFragSpv;
using native_vulkan_scene::kSceneShadowFragSpvWordCount;
using native_vulkan_scene::kSceneShadowVertSpv;
using native_vulkan_scene::kSceneShadowVertSpvWordCount;
using native_vulkan_scene::kSceneOverlayFragSpv;
using native_vulkan_scene::kSceneOverlayFragSpvWordCount;
using native_vulkan_scene::kSceneOverlayVertSpv;
using native_vulkan_scene::kSceneOverlayVertSpvWordCount;
using native_vulkan_scene::kSceneIconFragSpv;
using native_vulkan_scene::kSceneIconFragSpvWordCount;
using native_vulkan_scene::kSceneIconVertSpv;
using native_vulkan_scene::kSceneIconVertSpvWordCount;
using native_vulkan_scene::kScenePostFragSpv;
using native_vulkan_scene::kScenePostFragSpvWordCount;
using native_vulkan_scene::kScenePostVertSpv;
using native_vulkan_scene::kScenePostVertSpvWordCount;
using native_vulkan_scene::kSceneBloomThresholdFragSpv;
using native_vulkan_scene::kSceneBloomThresholdFragSpvWordCount;
using native_vulkan_scene::kSceneBloomDownFragSpv;
using native_vulkan_scene::kSceneBloomDownFragSpvWordCount;
using native_vulkan_scene::kSceneBloomUpFragSpv;
using native_vulkan_scene::kSceneBloomUpFragSpvWordCount;
using native_vulkan_scene::kSceneSkyboxFragSpv;
using native_vulkan_scene::kSceneSkyboxFragSpvWordCount;
using native_vulkan_scene::kSceneSkyboxVertSpv;
using native_vulkan_scene::kSceneSkyboxVertSpvWordCount;
using native_vulkan_scene::kSceneIblVertSpv;
using native_vulkan_scene::kSceneIblVertSpvWordCount;
using native_vulkan_scene::kSceneIblIrradianceFragSpv;
using native_vulkan_scene::kSceneIblIrradianceFragSpvWordCount;
using native_vulkan_scene::kSceneIblPrefilterFragSpv;
using native_vulkan_scene::kSceneIblPrefilterFragSpvWordCount;
using native_vulkan_scene::kSceneTextFragSpv;
using native_vulkan_scene::kSceneTextFragSpvWordCount;
using native_vulkan_scene::kSceneTextVertSpv;
using native_vulkan_scene::kSceneTextVertSpvWordCount;

const char* VkResultName(VkResult result)
{
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_EVENT_SET:
            return "VK_EVENT_SET";
        case VK_EVENT_RESET:
            return "VK_EVENT_RESET";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
            return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR:
            return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        default:
            return "VK_RESULT_UNKNOWN";
    }
}

void LogVkError(const char* stage, VkResult result)
{
    // SDL_Log reaches hilog (tag A00000/SDL/APP); OH_LOG_* does not in this setup.
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_VULKAN stage=%s result=%d (%s)",
        stage, static_cast<int>(result), VkResultName(result));
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

uint32_t ClampExtent(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    return std::max(minimum, std::min(value, maximum));
}

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

Mat4 Mat4FromArray(const float* values)
{
    Mat4 result{};
    if (values != nullptr) {
        std::memcpy(result.value, values, sizeof(result.value));
    }
    return result;
}

Mat4 Mat4ComposeTrs(const float translation[3], const float rotation[4], const float scale[3])
{
    Mat4 result{};
    const float x = rotation[0];
    const float y = rotation[1];
    const float z = rotation[2];
    const float w = rotation[3];
    result.value[0] = (1.0f - 2.0f * (y * y + z * z)) * scale[0];
    result.value[1] = (2.0f * (x * y + z * w)) * scale[0];
    result.value[2] = (2.0f * (x * z - y * w)) * scale[0];
    result.value[4] = (2.0f * (x * y - z * w)) * scale[1];
    result.value[5] = (1.0f - 2.0f * (x * x + z * z)) * scale[1];
    result.value[6] = (2.0f * (y * z + x * w)) * scale[1];
    result.value[8] = (2.0f * (x * z + y * w)) * scale[2];
    result.value[9] = (2.0f * (y * z - x * w)) * scale[2];
    result.value[10] = (1.0f - 2.0f * (x * x + y * y)) * scale[2];
    result.value[12] = translation[0];
    result.value[13] = translation[1];
    result.value[14] = translation[2];
    result.value[15] = 1.0f;
    return result;
}

void Mat4TransformPoint(const Mat4& matrix, const float input[3], float output[3])
{
    output[0] = matrix.value[0] * input[0] + matrix.value[4] * input[1]
        + matrix.value[8] * input[2] + matrix.value[12];
    output[1] = matrix.value[1] * input[0] + matrix.value[5] * input[1]
        + matrix.value[9] * input[2] + matrix.value[13];
    output[2] = matrix.value[2] * input[0] + matrix.value[6] * input[1]
        + matrix.value[10] * input[2] + matrix.value[14];
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
    // The Vulkan projection flips its y basis so NDC -1 is the top edge;
    // overlay vertices also use top-left pixel coordinates.
    *screenY = (ndcY * 0.5f + 0.5f) * height;
    return true;
}

void Mat4TransformVector(const Mat4& matrix, const float input[3], float output[3])
{
    output[0] = matrix.value[0] * input[0] + matrix.value[4] * input[1]
        + matrix.value[8] * input[2];
    output[1] = matrix.value[1] * input[0] + matrix.value[5] * input[1]
        + matrix.value[9] * input[2];
    output[2] = matrix.value[2] * input[0] + matrix.value[6] * input[1]
        + matrix.value[10] * input[2];
}

Mat4 Mat4Perspective(float fieldOfViewRadians, float aspect, float nearPlane, float farPlane)
{
    Mat4 result{};
    const float focalLength = 1.0f / std::tan(fieldOfViewRadians * 0.5f);
    result.value[0] = focalLength / aspect;
    // Vulkan's positive-height viewport maps NDC y=-1 to the top edge,
    // whereas the GLES projection maps NDC y=-1 to the bottom edge.  Negate
    // the projection's y basis so model geometry keeps the GL world-up
    // convention; UI shaders use their own top-left pixel mapping.
    result.value[5] = -focalLength;
    result.value[10] = farPlane / (nearPlane - farPlane);
    result.value[11] = -1.0f;
    result.value[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

// One stable directional-light camera shared by the Vulkan scene shadow pass
// and its receiver.  The light points down toward the scene from above and
// slightly toward +X/+Z, matching the direct-light vector in scene_model.frag.
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
    // A Z-up reference avoids the near-parallel Y-up singularity for this
    // deliberately top-down light.
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
    // Vulkan NDC depth is [0, 1].  The shadow receiver remaps x/y only;
    // z is already in the depth texture's comparison range.
    result.value[10] = -1.0f / (kFarPlane - kNearPlane);
    result.value[14] = -kNearPlane / (kFarPlane - kNearPlane);
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

Mat4 Mat4Scale(float factor)
{
    Mat4 result = Mat4Identity();
    result.value[0] = factor;
    result.value[5] = factor;
    result.value[10] = factor;
    return result;
}

Mat4 Mat4Scale3(float sx, float sy, float sz)
{
    Mat4 result = Mat4Identity();
    result.value[0] = sx;
    result.value[5] = sy;
    result.value[10] = sz;
    return result;
}

struct SceneUniforms {
    Mat4 cubeMvp;
    Mat4 skyMvp;
    // glTF model: the full mvp for the rasterizer plus the object transform on
    // its own so the vertex stage can rebuild normals after optional GPU
    // skinning.  The animated normalisation lives in the joint matrices.
    Mat4 modelMvp;
    Mat4 modelMatrix;
    // The forward Vulkan material pass uses the same camera-relative lighting
    // contract as the GLES deferred pass.  Keeping this in the per-image UBO
    // avoids growing the 128-byte object push-constant range.
    float cameraPosition[4] = {};
    Mat4 cameraToWorld;
    Mat4 shadowMvp;
    Mat4 viewProj;
};

static_assert(sizeof(SceneUniforms) == sizeof(float) * 116,
    "Scene UBO must contain camera data, shadow matrix and view projection");

// Vertex layout of the model pipeline.  Separate from CubeVertex because glTF
// meshes carry a NORMAL stream and no per-vertex colour: 3 + 3 + 2 floats.
// The trailing skin slots stay byte-identical to the loader so the Vulkan
// vertex shader can apply the same GPU LBS path as GLES.
struct ModelVertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
    std::uint8_t joints[4];
    std::uint8_t weights[4];
};

static_assert(sizeof(ModelVertex) == 40, "ModelVertex must stay tightly packed (8 floats + 8 u8 skin)");
// The loader's CPU vertex and this GPU vertex are copied with memcpy, so the
// two layouts have to agree exactly.
static_assert(sizeof(ohos_model::Vertex) == sizeof(ModelVertex), "model vertex layouts must match");
static_assert(offsetof(ohos_model::Vertex, uv) == offsetof(ModelVertex, u), "model vertex layouts must match");

// Vulkan bakes the material factors into a separate vertex stream.  Quaternius'
// character intentionally has no image textures, so dropping the factor would
// turn the orange mannequin into a grey mesh.
struct ModelVertexGpu {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
    // Per-material factors are duplicated per primitive so Vulkan can keep
    // one compact descriptor set per material while matching GLES' material
    // inputs without adding another dynamic UBO.
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float emissiveR;
    float emissiveG;
    float emissiveB;
    float aoStrength;
    float materialPadding;
    std::uint32_t joints[4];
    float weights[4];
};

static_assert(sizeof(ModelVertexGpu) == sizeof(float) * 28,
    "ModelVertexGpu must stay tightly packed");

// Which vertex input layout a pipeline should declare.
enum class VertexLayout {
    kNone,   // full-screen triangle, no vertex buffer
    kCube,   // CubeVertex: pos4 + colour3 + uv2
    kModel,  // ModelVertex: pos3 + normal3 + uv2
    kOverlay, // window-pixel positions plus premultiplied UI colour
    kIcon,   // window-pixel positions, icon UVs and tint colour
    kText,    // window-pixel positions, atlas UVs, colour and SDF range
};

struct OverlayVertex {
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

struct IconVertex {
    float x;
    float y;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
};

struct TextVertex {
    float x;
    float y;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
    float screenPxRange;
};

static_assert(sizeof(OverlayVertex) == sizeof(float) * 6, "overlay vertex must stay tightly packed");
static_assert(sizeof(IconVertex) == sizeof(float) * 8, "icon vertex must stay tightly packed");
static_assert(sizeof(TextVertex) == sizeof(float) * 9, "text vertex must stay tightly packed");

// The Vulkan proxy is sensitive to per-frame allocations and map/unmap churn,
// so each swapchain image owns one reusable UI upload buffer.  The text region
// is aligned well beyond the Vulkan vertex-offset requirement and leaves ample
// room for the menu, HUD and touch controls.
constexpr VkDeviceSize kUiIconOffset = 64 * 1024;
constexpr VkDeviceSize kUiTextOffset = 128 * 1024;
constexpr VkDeviceSize kUiVertexBufferSize = 512 * 1024;
constexpr int kOverlaySegments = 48;

struct UiPushConstants {
    float width;
    float height;
    float unusedPxRange;
    float unusedPadding;
};

static_assert(sizeof(UiPushConstants) == 16, "UI push constants must match std430 layout");

struct PostProcessPushConstants {
    float exposure;
    float bloomStrength;
    float texelSizeX;
    float texelSizeY;
};

static_assert(sizeof(PostProcessPushConstants) == 16,
    "post-process push constants must match std430 layout");

struct BufferResource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags memoryFlags = 0;
    void* mapped = nullptr;
};

struct TextureResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t layerCount = 1;
    uint32_t mipLevels = 1;
};

// GPU residency for one loaded glTF mesh.
struct ModelMeshResource {
    BufferResource vertexBuffer;
    BufferResource indexBuffer;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
};

// One glTF material, resolved to an index into the loaded texture list.
struct ModelMaterialResource {
    int32_t baseColorTexture = -1;
    int32_t metallicRoughnessTexture = -1;
    int32_t normalTexture = -1;
    int32_t aoTexture = -1;
    int32_t emissiveTexture = -1;
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};
    float aoStrength = 1.0f;
};

// Keep the Vulkan player state machine in lockstep with the GLES renderer.
// These are hard-cut states by design: the source animation clips already
// carry the same loop/one-shot semantics as the reference path.
enum class ModelAnimationState : std::uint8_t {
    Idle,
    Walk,
    Sprint,
    Attack,
    JumpStart,
    JumpLoop,
    JumpLand,
    Crouch,
    Hit,
    Death,
};

struct ModelGpuAsset {
    std::vector<ModelMeshResource> meshes;
    std::vector<ModelMaterialResource> materials;
    std::vector<TextureResource> textures;
    // The player keeps the loader's skeleton/clip data so the per-frame joint
    // buffer can follow the GLES animation sampler.  Static assets use the
    // renderer-wide identity buffer instead.
    BufferResource jointBuffer;
    std::vector<ohos_model::Node> animationNodes;
    ohos_model::Skin animationSkin;
    std::vector<ohos_model::AnimationClip> animationClips;
    Mat4 animationNormalise = Mat4Identity();
    std::vector<Mat4> jointMatrices;
    int animationClipIndex = -1;
    int idleClip = -1;
    int walkClip = -1;
    int sprintClip = -1;
    int crouchClip = -1;
    int attackClip = -1;
    int jumpStartClip = -1;
    int jumpLoopClip = -1;
    int jumpLandClip = -1;
    int deathClip = -1;
    int hitClip = -1;
    float animationTime = 0.0f;
    float animationStateTime = 0.0f;
    Uint64 animationLastTicks = 0;
    ModelAnimationState animationState = ModelAnimationState::Idle;
    bool animationLoop = true;
    bool skinned = false;
    bool animationLogged = false;
    // One descriptor set per (swapchain image x material), rebuilt whenever
    // the swapchain changes.  All three scene assets share the same layout.
    std::vector<VkDescriptorSet> materialSets;
    bool loaded = false;
};

constexpr std::size_t kMaxJoints = 256;
constexpr uint32_t kShadowMapSize = 1024;
constexpr int kBloomDsLevels = 6;
constexpr int kBloomUpLevels = 5;
constexpr bool kEnableVulkanSkyboxMipmaps = true;

struct ModelPushConstants {
    Mat4 mvp;
    Mat4 model;
    // Enemy death dissolve (0 = intact); the shaders gate it on the skin
    // index so only the enemy instance of the character batch can burn away.
    float dissolve;
    float pad[3];
};

static_assert(sizeof(ModelPushConstants) == sizeof(float) * 36,
    "model push constants must contain two mat4 values plus the dissolve");

// Mikan-style instance stream: the mesh/material stays shared while each
// character contributes only its world transform, marker color and palette
// selector. The two palettes are supplied through descriptor bindings 7/12.
struct CharacterInstanceData {
    Mat4 model;
    // RGB is a gameplay marker color; alpha is marker blend strength.
    float color[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    std::uint32_t skinIndex = 0;
    std::uint32_t padding[3] = {};
};

static_assert(sizeof(CharacterInstanceData) == sizeof(float) * 24,
    "character instance data must be tightly packed");

struct CubeSourceVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
    float u;
    float v;
};

struct CubeVertex {
    float x;
    float y;
    float z;
    float w;
    float r;
    float g;
    float b;
    float u;
    float v;
};

// The cube is transformed on the CPU into clip space.  This keeps the
// sample independent of uniform-buffer reads on the OHOS emulator while the
// actual rasterization, texturing and presentation remain Vulkan operations.
constexpr CubeSourceVertex kCubeSourceVertices[] = {
    // Front (+Z), red.
    {-0.5f, -0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 0.0f, 1.0f},
    { 0.5f, -0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 1.0f, 1.0f},
    { 0.5f,  0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 1.0f, 0.0f},
    {-0.5f, -0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 0.0f, 1.0f},
    { 0.5f,  0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 1.0f, 0.0f},
    {-0.5f,  0.5f,  0.5f, 1.0f, 0.15f, 0.15f, 0.0f, 0.0f},

    // Back (-Z), blue.
    { 0.5f, -0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 0.0f, 1.0f},
    {-0.5f, -0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 1.0f, 1.0f},
    {-0.5f,  0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 1.0f, 0.0f},
    { 0.5f, -0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 0.0f, 1.0f},
    {-0.5f,  0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 1.0f, 0.0f},
    { 0.5f,  0.5f, -0.5f, 0.15f, 0.35f, 1.0f, 0.0f, 0.0f},

    // Left (-X), green.
    {-0.5f, -0.5f, -0.5f, 0.15f, 1.0f, 0.25f, 0.0f, 1.0f},
    {-0.5f, -0.5f,  0.5f, 0.15f, 1.0f, 0.25f, 1.0f, 1.0f},
    {-0.5f,  0.5f,  0.5f, 0.15f, 1.0f, 0.25f, 1.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, 0.15f, 1.0f, 0.25f, 0.0f, 1.0f},
    {-0.5f,  0.5f,  0.5f, 0.15f, 1.0f, 0.25f, 1.0f, 0.0f},
    {-0.5f,  0.5f, -0.5f, 0.15f, 1.0f, 0.25f, 0.0f, 0.0f},

    // Right (+X), yellow.
    { 0.5f, -0.5f,  0.5f, 1.0f, 0.85f, 0.1f, 0.0f, 1.0f},
    { 0.5f, -0.5f, -0.5f, 1.0f, 0.85f, 0.1f, 1.0f, 1.0f},
    { 0.5f,  0.5f, -0.5f, 1.0f, 0.85f, 0.1f, 1.0f, 0.0f},
    { 0.5f, -0.5f,  0.5f, 1.0f, 0.85f, 0.1f, 0.0f, 1.0f},
    { 0.5f,  0.5f, -0.5f, 1.0f, 0.85f, 0.1f, 1.0f, 0.0f},
    { 0.5f,  0.5f,  0.5f, 1.0f, 0.85f, 0.1f, 0.0f, 0.0f},

    // Top (+Y), magenta.
    {-0.5f,  0.5f,  0.5f, 1.0f, 0.2f, 1.0f, 0.0f, 1.0f},
    { 0.5f,  0.5f,  0.5f, 1.0f, 0.2f, 1.0f, 1.0f, 1.0f},
    { 0.5f,  0.5f, -0.5f, 1.0f, 0.2f, 1.0f, 1.0f, 0.0f},
    {-0.5f,  0.5f,  0.5f, 1.0f, 0.2f, 1.0f, 0.0f, 1.0f},
    { 0.5f,  0.5f, -0.5f, 1.0f, 0.2f, 1.0f, 1.0f, 0.0f},
    {-0.5f,  0.5f, -0.5f, 1.0f, 0.2f, 1.0f, 0.0f, 0.0f},

    // Bottom (-Y), cyan.
    {-0.5f, -0.5f, -0.5f, 0.1f, 0.9f, 1.0f, 0.0f, 1.0f},
    { 0.5f, -0.5f, -0.5f, 0.1f, 0.9f, 1.0f, 1.0f, 1.0f},
    { 0.5f, -0.5f,  0.5f, 0.1f, 0.9f, 1.0f, 1.0f, 0.0f},
    {-0.5f, -0.5f, -0.5f, 0.1f, 0.9f, 1.0f, 0.0f, 1.0f},
    { 0.5f, -0.5f,  0.5f, 0.1f, 0.9f, 1.0f, 1.0f, 0.0f},
    {-0.5f, -0.5f,  0.5f, 0.1f, 0.9f, 1.0f, 0.0f, 0.0f},
};

constexpr uint32_t kCubeVertexCount = static_cast<uint32_t>(
    sizeof(kCubeSourceVertices) / sizeof(kCubeSourceVertices[0]));

struct Vec4 {
    float x;
    float y;
    float z;
    float w;
};

Vec4 TransformPoint(const Mat4& matrix, const CubeSourceVertex& vertex)
{
    return {
        matrix.value[0] * vertex.x + matrix.value[4] * vertex.y + matrix.value[8] * vertex.z + matrix.value[12],
        matrix.value[1] * vertex.x + matrix.value[5] * vertex.y + matrix.value[9] * vertex.z + matrix.value[13],
        matrix.value[2] * vertex.x + matrix.value[6] * vertex.y + matrix.value[10] * vertex.z + matrix.value[14],
        matrix.value[3] * vertex.x + matrix.value[7] * vertex.y + matrix.value[11] * vertex.z + matrix.value[15],
    };
}

constexpr uint64_t kVulkanOperationTimeoutNs = 1'000'000'000ULL;

// glTF asset loaded from the HAP rawfile bundle.  Buffers and textures it
// references by relative URI resolve against this file's own directory.
constexpr const char* kModelPath = "models/DamagedHelmet/glTF/DamagedHelmet.gltf";

// The built-in rotating cube was the scaffold that proved this renderer works.
// Its pipeline and vertex buffer are still created so flipping this back to
// true is a one word rollback, but the loaded model is what the scene shows:
// the model is normalised to the same 2 unit extent, so both cannot share the
// origin without intersecting.
constexpr bool kDrawLegacyCube = false;

class VulkanRenderer final : public rhi::IRenderer {
public:
    ~VulkanRenderer() override
    {
        Destroy();
    }

    const char* Name() const override
    {
        return "vulkan";
    }

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
        // The OHOS XComponent may destroy its NativeWindow while this renderer
        // stays alive. Keep the device and scene assets, but never reuse the
        // old VkSurfaceKHR after the next foreground transition.
        if (device_ != VK_NULL_HANDLE && !deviceLost_) {
            const VkResult idleResult = vkDeviceWaitIdle(device_);
            if (idleResult != VK_SUCCESS) {
                LogVkError("device_idle_on_background", idleResult);
                if (idleResult == VK_ERROR_DEVICE_LOST) {
                    deviceLost_ = true;
                }
            }
        }
        surfaceNeedsRecreate_ = true;
    }

    bool OnApplicationForeground() override
    {
        surfaceNeedsRecreate_ = true;
        return true;
    }

    rhi::UiScreen CurrentScreen() const override
    {
        return uiScreen_;
    }

    bool Initialize(SDL_Window* window, uint64_t width, uint64_t height) override
    {
        if (window == nullptr) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=surface_invalid window=%{public}p width=%{public}llu height=%{public}llu",
                window, static_cast<unsigned long long>(width), static_cast<unsigned long long>(height));
            return false;
        }

        if (width == 0) {
            width = 720;
        }
        if (height == 0) {
            height = 1280;
        }

        if (surfaceNeedsRecreate_ && instance_ != VK_NULL_HANDLE &&
            device_ != VK_NULL_HANDLE && sceneReady_ && !deviceLost_) {
            window_ = window;
            width_ = width;
            height_ = height;
            if (!RecreateSurfaceAndSwapchain()) {
                return false;
            }
        }

        const bool rendererReady = instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE &&
            device_ != VK_NULL_HANDLE && queue_ != VK_NULL_HANDLE && swapchain_ != VK_NULL_HANDLE &&
            renderPass_ != VK_NULL_HANDLE && pipeline_ != VK_NULL_HANDLE && skyboxPipeline_ != VK_NULL_HANDLE &&
            postRenderPass_ != VK_NULL_HANDLE && postPipeline_ != VK_NULL_HANDLE &&
            bloomRenderPass_ != VK_NULL_HANDLE && bloomThresholdPipeline_ != VK_NULL_HANDLE &&
            bloomDownPipeline_ != VK_NULL_HANDLE && bloomUpPipeline_ != VK_NULL_HANDLE &&
            overlayPipeline_ != VK_NULL_HANDLE && iconPipeline_ != VK_NULL_HANDLE &&
            textPipeline_ != VK_NULL_HANDLE &&
            commandPool_ != VK_NULL_HANDLE && inFlightFences_[0] != VK_NULL_HANDLE &&
            inFlightFences_[kInFlightFrames - 1] != VK_NULL_HANDLE &&
            !commandBuffers_.empty() && uiVertexBuffers_.size() == commandBuffers_.size() && sceneReady_;
        if (rendererReady && window_ == window && !deviceLost_) {
            if (width_ == width && height_ == height && !swapchainDirty_) {
                return true;
            }

            width_ = width;
            height_ = height;
            return RecreateSwapchain();
        }

        Destroy();
        window_ = window;
        width_ = width;
        height_ = height;

        if (!CreateInstance() || !CreateSurface() || !PickPhysicalDevice() || !CreateDevice() ||
            !CreateSwapchain() || !CreateRenderPass() || !CreateCommandPool() ||
            !CreateSceneResources() || !CreateGraphicsPipeline() ||
            !CreateFramebuffersAndCommands() || !CreateSyncObjects()) {
            Destroy();
            return false;
        }

        swapchainDirty_ = false;
        deviceLost_ = false;
        OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=scene_ready scene=skybox_textured_cube extent=%{public}ux%{public}u images=%{public}u",
            swapchainExtent_.width, swapchainExtent_.height, static_cast<unsigned int>(swapchainImages_.size()));
        return true;
    }

    bool DrawOnce() override
    {
        if (device_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE ||
            inFlightFences_[0] == VK_NULL_HANDLE || commandBuffers_.empty() ||
            uniformBuffers_.size() != commandBuffers_.size() ||
            uiVertexBuffers_.size() != commandBuffers_.size() || !sceneReady_) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=draw_not_ready");
            return false;
        }

        // Deferred offscreen-chain rebuild for the RENDER SCALE setting: the
        // tap arrived mid-frame (after acquire), so tear down / rebuild here
        // instead, while no swapchain image is acquired.
        if (renderTargetsDirty_) {
            renderTargetsDirty_ = false;
            if (!RecreateSwapchain()) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=render_scale_recreate_failed");
                return false;
            }
            const VkExtent2D rebuilt = SceneExtent();
            OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=render_scale_rebuilt scene=%{public}ux%{public}u",
                rebuilt.width, rebuilt.height);
        }

        const uint32_t slot = currentSlot_;
        VkResult result = vkWaitForFences(device_, 1, &inFlightFences_[slot], VK_TRUE, kVulkanOperationTimeoutNs);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            LogVkError("wait_fence", result);
            return false;
        }

        uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(device_, swapchain_, kVulkanOperationTimeoutNs,
            imageAvailable_[slot], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            surfaceNeedsRecreate_ = true;
            LogVkError("acquire_image_surface_lost", result);
            return false;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainDirty_ = true;
            LogVkError("acquire_image_needs_recreate", result);
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            LogVkError("acquire_image", result);
            return false;
        }
        if (imageIndex >= commandBuffers_.size() || imageIndex >= uniformBuffers_.size()) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=acquire_image_invalid_index index=%{public}u count=%{public}u",
                imageIndex, static_cast<unsigned int>(commandBuffers_.size()));
            swapchainDirty_ = true;
            (void)RecreateSyncObjects();
            return false;
        }

        UpdateFrameTiming();
        ConsumeCameraAndMenuInput();

        // Upload into the acquired image's own UBO: the previous render of
        // this image completed before it was re-presented and returned by
        // acquire, so there is no in-flight data hazard.
        if (!UpdateSceneUniforms(uniformBuffers_[imageIndex])) {
            return false;
        }
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            UpdatePlayerAnimation();
            UpdateEnemyAnimation();
        }

        std::vector<OverlayVertex> overlayVertices;
        std::vector<IconVertex> iconVertices;
        std::vector<TextVertex> textVertices;
        BuildUiGeometry(overlayVertices, iconVertices, textVertices);

        if (!UploadUiGeometry(uiVertexBuffers_[imageIndex], overlayVertices, iconVertices,
            textVertices)) {
            return false;
        }
        if (!RecordFrameCommands(static_cast<size_t>(imageIndex),
            static_cast<uint32_t>(overlayVertices.size()), static_cast<uint32_t>(iconVertices.size()),
            static_cast<uint32_t>(textVertices.size()))) {
            return false;
        }

        result = vkResetFences(device_, 1, &inFlightFences_[slot]);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                (void)RecreateSyncObjects();
            }
            LogVkError("reset_fence", result);
            swapchainDirty_ = true;
            return false;
        }

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageAvailable_[slot];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[imageIndex];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished_[slot];

        result = vkQueueSubmit(queue_, 1, &submitInfo, inFlightFences_[slot]);
        if (result != VK_SUCCESS) {
            LogVkError("queue_submit", result);
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            } else {
                (void)RecreateSyncObjects();
            }
            return false;
        }

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished_[slot];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(queue_, &presentInfo);
        if (presentResult == VK_ERROR_SURFACE_LOST_KHR) {
            surfaceNeedsRecreate_ = true;
            LogVkError("queue_present_surface_lost", presentResult);
            return false;
        }
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchainDirty_ = true;
            LogVkError("queue_present_needs_recreate", presentResult);
            return false;
        }
        if (presentResult == VK_ERROR_DEVICE_LOST) {
            deviceLost_ = true;
        }
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
            LogVkError("queue_present", presentResult);
            return false;
        }

        // Slot released for reuse: the next frame's CPU work now overlaps
        // with this frame's GPU execution and present.
        currentSlot_ = (currentSlot_ + 1u) % kInFlightFrames;

        if (!firstFrameLogged_) {
            OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=scene_presented scene=skybox_textured_cube");
            firstFrameLogged_ = true;
        }
        return true;
    }

    void Destroy() override
    {
        if (device_ != VK_NULL_HANDLE && !deviceLost_) {
            const VkResult result = vkDeviceWaitIdle(device_);
            if (result != VK_SUCCESS) {
                LogVkError("device_idle_before_destroy", result);
            }
        }

        DestroySyncObjects();
        DestroySwapchainResources();
        DestroySceneResources();

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }

        physicalDevice_ = VK_NULL_HANDLE;
        queueFamilyIndex_ = 0;
        queue_ = VK_NULL_HANDLE;
        window_ = nullptr;
        width_ = 0;
        height_ = 0;
        depthFormat_ = VK_FORMAT_UNDEFINED;
        swapchainDirty_ = false;
        surfaceNeedsRecreate_ = false;
        surfaceRecreateFailureLogged_ = false;
        deviceLost_ = false;
        firstFrameLogged_ = false;
    }

    // Mikan-style retry: restore the complete gameplay snapshot, including
    // both Jolt characters, animation cursors and the third-person camera.
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
        camDistance_ = 2.8f;
        camPanX_ = 0.0f;
        camPanY_ = 0.0f;
        camPanZ_ = 0.0f;
        camPosX_ = 0.0f;
        camPosY_ = 0.0f;
        camPosZ_ = camDistance_;
        moveLastTicks_ = SDL_GetTicks();

        playerAsset_.animationState = ModelAnimationState::Idle;
        playerAsset_.animationStateTime = 0.0f;
        playerAsset_.animationClipIndex = playerAsset_.idleClip >= 0
            ? playerAsset_.idleClip : (playerAsset_.walkClip >= 0
                ? playerAsset_.walkClip : (playerAsset_.animationClips.empty() ? -1 : 0));
        playerAsset_.animationLoop = true;
        playerAsset_.animationTime = 0.0f;
        playerAsset_.animationLastTicks = SDL_GetTicks();
        enemyAsset_.animationState = ModelAnimationState::Idle;
        enemyAsset_.animationStateTime = 0.0f;
        enemyAsset_.animationClipIndex = enemyAsset_.idleClip >= 0
            ? enemyAsset_.idleClip : (enemyAsset_.walkClip >= 0
                ? enemyAsset_.walkClip : (enemyAsset_.animationClips.empty() ? -1 : 0));
        enemyAsset_.animationLoop = true;
        enemyAsset_.animationTime = 0.0f;
        enemyAsset_.animationLastTicks = SDL_GetTicks();

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
    void DestroySyncObjects()
    {
        if (device_ == VK_NULL_HANDLE) {
            for (uint32_t slot = 0; slot < kInFlightFrames; ++slot) {
                inFlightFences_[slot] = VK_NULL_HANDLE;
                imageAvailable_[slot] = VK_NULL_HANDLE;
                renderFinished_[slot] = VK_NULL_HANDLE;
            }
            return;
        }
        for (uint32_t slot = 0; slot < kInFlightFrames; ++slot) {
            if (imageAvailable_[slot] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, imageAvailable_[slot], nullptr);
            }
            imageAvailable_[slot] = VK_NULL_HANDLE;
            if (renderFinished_[slot] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, renderFinished_[slot], nullptr);
            }
            renderFinished_[slot] = VK_NULL_HANDLE;
            if (inFlightFences_[slot] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, inFlightFences_[slot], nullptr);
            }
            inFlightFences_[slot] = VK_NULL_HANDLE;
        }
        currentSlot_ = 0;
    }

    bool RecreateSyncObjects()
    {
        DestroySyncObjects();
        return CreateSyncObjects();
    }

    void DestroyBuffer(BufferResource& buffer)
    {
        if (device_ != VK_NULL_HANDLE && buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device_, buffer.memory);
        }
        buffer.mapped = nullptr;
        if (device_ != VK_NULL_HANDLE && buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, buffer.buffer, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, buffer.memory, nullptr);
        }
        buffer = {};
    }

    void DestroyTexture(TextureResource& texture)
    {
        if (device_ != VK_NULL_HANDLE && texture.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, texture.view, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && texture.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, texture.image, nullptr);
        }
        if (device_ != VK_NULL_HANDLE && texture.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, texture.memory, nullptr);
        }
        texture = {};
    }

    void DestroyDepthResources()
    {
        if (device_ != VK_NULL_HANDLE && depthImageView_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, depthImageView_, nullptr);
        }
        depthImageView_ = VK_NULL_HANDLE;
        if (device_ != VK_NULL_HANDLE && depthImage_ != VK_NULL_HANDLE) {
            vkDestroyImage(device_, depthImage_, nullptr);
        }
        depthImage_ = VK_NULL_HANDLE;
        if (device_ != VK_NULL_HANDLE && depthMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, depthMemory_, nullptr);
        }
        depthMemory_ = VK_NULL_HANDLE;
    }

    void DestroySceneResources()
    {
        if (device_ == VK_NULL_HANDLE) {
            sceneReady_ = false;
            return;
        }

        if (descriptorPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        }
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSets_.clear();
        overlayDescriptorSet_ = VK_NULL_HANDLE;
        actionIconDescriptorSets_.fill(VK_NULL_HANDLE);
        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        }
        descriptorSetLayout_ = VK_NULL_HANDLE;
        if (overlayDescriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, overlayDescriptorSetLayout_, nullptr);
        }
        overlayDescriptorSetLayout_ = VK_NULL_HANDLE;
        if (postDescriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, postDescriptorSetLayout_, nullptr);
        }
        postDescriptorSetLayout_ = VK_NULL_HANDLE;
        if (sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, sampler_, nullptr);
        }
        sampler_ = VK_NULL_HANDLE;
        DestroyTexture(cubeTexture_);
        DestroyTexture(whiteTexture_);
        DestroyTexture(neutralMrTexture_);
        DestroyTexture(neutralNormalTexture_);
        DestroyTexture(neutralAoTexture_);
        DestroyTexture(blackTexture_);
        DestroyTexture(skyboxTexture_);
        DestroyTexture(irradianceTexture_);
        DestroyTexture(prefilteredTexture_);
        DestroyTexture(brdfLutTexture_);
        DestroyTexture(textAtlasTexture_);
        for (TextureResource& texture : actionIconTextures_) {
            DestroyTexture(texture);
        }
        DestroyTexture(shadowMap_);
        DestroyBuffer(cubeVertexBuffer_);
        for (BufferResource& buffer : uniformBuffers_) {
            DestroyBuffer(buffer);
        }
        uniformBuffers_.clear();
        DestroyBuffer(modelInstanceBuffer_);
        // glTF residency: mesh buffers and decoded textures are device-scoped,
        // so they survive swapchain recreation and are torn down here.
        const auto destroyAsset = [this](ModelGpuAsset& asset) {
            for (ModelMeshResource& mesh : asset.meshes) {
                DestroyBuffer(mesh.vertexBuffer);
                DestroyBuffer(mesh.indexBuffer);
            }
            asset.meshes.clear();
            DestroyBuffer(asset.jointBuffer);
            for (TextureResource& texture : asset.textures) {
                DestroyTexture(texture);
            }
            asset.textures.clear();
            asset.materials.clear();
            asset.animationNodes.clear();
            asset.animationSkin.jointNodes.clear();
            asset.animationSkin.inverseBindMatrices.clear();
            asset.animationClips.clear();
            asset.jointMatrices.clear();
            asset.materialSets.clear();
            asset.loaded = false;
        };
        destroyAsset(playerAsset_);
        destroyAsset(enemyAsset_);
        destroyAsset(helmetAsset_);
        destroyAsset(groundAsset_);
        DestroyBuffer(identityJointBuffer_);
        sceneReady_ = false;
    }

    void DestroySwapchainResources()
    {
        if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE && !commandBuffers_.empty()) {
            vkFreeCommandBuffers(device_, commandPool_, static_cast<uint32_t>(commandBuffers_.size()),
                commandBuffers_.data());
        }
        commandBuffers_.clear();

        // Per-image UBOs and descriptor sets are swapchain-scoped: free them
        // so recreation starts clean (the pool itself is scene-scoped).
        for (BufferResource& buffer : uniformBuffers_) {
            DestroyBuffer(buffer);
        }
        uniformBuffers_.clear();
        for (BufferResource& buffer : uiVertexBuffers_) {
            DestroyBuffer(buffer);
        }
        uiVertexBuffers_.clear();
        if (device_ != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE &&
            (!descriptorSets_.empty() || !playerAsset_.materialSets.empty() ||
                !enemyAsset_.materialSets.empty() || !helmetAsset_.materialSets.empty() ||
                !groundAsset_.materialSets.empty() ||
                !postDescriptorSets_.empty() || bloomThresholdDescriptorSet_ != VK_NULL_HANDLE)) {
            vkResetDescriptorPool(device_, descriptorPool_, 0);
        }
        descriptorSets_.clear();
        postDescriptorSets_.clear();
        bloomThresholdDescriptorSet_ = VK_NULL_HANDLE;
        bloomDownDescriptorSets_.fill(VK_NULL_HANDLE);
        bloomUpDescriptorSets_.fill(VK_NULL_HANDLE);
        // Material sets died with the pool reset above; clear the handles so
        // RecordModelDraw cannot bind a stale set if recording runs again.
        playerAsset_.materialSets.clear();
        enemyAsset_.materialSets.clear();
        helmetAsset_.materialSets.clear();
        groundAsset_.materialSets.clear();
        overlayDescriptorSet_ = VK_NULL_HANDLE;
        actionIconDescriptorSets_.fill(VK_NULL_HANDLE);

        if (device_ != VK_NULL_HANDLE && commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }
        commandPool_ = VK_NULL_HANDLE;

        if (device_ != VK_NULL_HANDLE) {
            for (VkFramebuffer framebuffer : framebuffers_) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
            framebuffers_.clear();
            for (VkFramebuffer framebuffer : postFramebuffers_) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
            postFramebuffers_.clear();
            for (VkFramebuffer framebuffer : bloomDsFramebuffers_) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
            bloomDsFramebuffers_.clear();
            for (VkFramebuffer framebuffer : bloomUpFramebuffers_) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
            bloomUpFramebuffers_.clear();
            if (shadowFramebuffer_ != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, shadowFramebuffer_, nullptr);
            }
            shadowFramebuffer_ = VK_NULL_HANDLE;
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
            }
            pipeline_ = VK_NULL_HANDLE;
            if (skyboxPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, skyboxPipeline_, nullptr);
            }
            skyboxPipeline_ = VK_NULL_HANDLE;
            if (modelPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, modelPipeline_, nullptr);
            }
            modelPipeline_ = VK_NULL_HANDLE;
            if (overlayPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, overlayPipeline_, nullptr);
            }
            overlayPipeline_ = VK_NULL_HANDLE;
            if (iconPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, iconPipeline_, nullptr);
            }
            iconPipeline_ = VK_NULL_HANDLE;
            if (textPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, textPipeline_, nullptr);
            }
            textPipeline_ = VK_NULL_HANDLE;
            if (postPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, postPipeline_, nullptr);
            }
            postPipeline_ = VK_NULL_HANDLE;
            if (bloomThresholdPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomThresholdPipeline_, nullptr);
            }
            bloomThresholdPipeline_ = VK_NULL_HANDLE;
            if (bloomDownPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomDownPipeline_, nullptr);
            }
            bloomDownPipeline_ = VK_NULL_HANDLE;
            if (bloomUpPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomUpPipeline_, nullptr);
            }
            bloomUpPipeline_ = VK_NULL_HANDLE;
            if (shadowPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, shadowPipeline_, nullptr);
            }
            shadowPipeline_ = VK_NULL_HANDLE;
            if (pipelineLayout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            }
            pipelineLayout_ = VK_NULL_HANDLE;
            if (renderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, renderPass_, nullptr);
            }
            renderPass_ = VK_NULL_HANDLE;
            if (postRenderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, postRenderPass_, nullptr);
            }
            postRenderPass_ = VK_NULL_HANDLE;
            if (bloomRenderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, bloomRenderPass_, nullptr);
            }
            bloomRenderPass_ = VK_NULL_HANDLE;
            if (shadowRenderPass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, shadowRenderPass_, nullptr);
            }
            shadowRenderPass_ = VK_NULL_HANDLE;
            DestroyTexture(sceneColorTarget_);
            DestroyTexture(shadowMap_);
            for (TextureResource& texture : bloomDs_) {
                DestroyTexture(texture);
            }
            for (TextureResource& texture : bloomUp_) {
                DestroyTexture(texture);
            }
            DestroyDepthResources();
            for (VkImageView imageView : imageViews_) {
                vkDestroyImageView(device_, imageView, nullptr);
            }
            imageViews_.clear();
            if (swapchain_ != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            }
        }

        framebuffers_.clear();
        postFramebuffers_.clear();
        bloomDsFramebuffers_.clear();
        bloomUpFramebuffers_.clear();
        imageViews_.clear();
        swapchain_ = VK_NULL_HANDLE;
        swapchainImages_.clear();
        swapchainFormat_ = VK_FORMAT_UNDEFINED;
        sceneColorFormat_ = VK_FORMAT_UNDEFINED;
        swapchainExtent_ = {};
    }

    bool RecreateSwapchain()
    {
        if (device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=swapchain_recreate_not_ready");
            return false;
        }
        const VkResult idleResult = vkDeviceWaitIdle(device_);
        if (idleResult != VK_SUCCESS) {
            LogVkError("device_idle_before_swapchain_recreate", idleResult);
            deviceLost_ = true;
            Destroy();
            return false;
        }
        DestroySwapchainResources();
        if (!CreateSwapchain() || !CreateRenderPass() || !CreateGraphicsPipeline() ||
            !CreateFramebuffersAndCommands()) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=swapchain_recreate_failed");
            Destroy();
            return false;
        }
        swapchainDirty_ = false;
        return true;
    }

    bool RecreateSurfaceAndSwapchain()
    {
        if (instance_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE ||
            physicalDevice_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=surface_recreate_not_ready");
            return false;
        }

        const VkResult idleResult = vkDeviceWaitIdle(device_);
        if (idleResult != VK_SUCCESS) {
            LogVkError("device_idle_before_surface_recreate", idleResult);
            if (idleResult == VK_ERROR_DEVICE_LOST) {
                deviceLost_ = true;
            }
            return false;
        }

        DestroySwapchainResources();
        if (surface_ != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (!CreateSurface()) {
            if (!surfaceRecreateFailureLogged_) {
                OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=surface_recreate_waiting_for_native_window");
                surfaceRecreateFailureLogged_ = true;
            }
            return false;
        }
        surfaceRecreateFailureLogged_ = false;

        VkBool32 canPresent = VK_FALSE;
        const VkResult presentSupportResult = vkGetPhysicalDeviceSurfaceSupportKHR(
            physicalDevice_, queueFamilyIndex_, surface_, &canPresent);
        if (presentSupportResult != VK_SUCCESS) {
            if (!surfaceRecreateFailureLogged_) {
                LogVkError("surface_recreate_present_support", presentSupportResult);
                surfaceRecreateFailureLogged_ = true;
            }
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
            return false;
        }
        if (canPresent != VK_TRUE) {
            if (!surfaceRecreateFailureLogged_) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=surface_recreate_present_unsupported");
                surfaceRecreateFailureLogged_ = true;
            }
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
            return false;
        }

        if (!CreateSwapchain() || !CreateRenderPass() || !CreateGraphicsPipeline() ||
            !CreateFramebuffersAndCommands()) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=surface_recreate_resources_failed");
            Destroy();
            return false;
        }

        swapchainDirty_ = false;
        surfaceNeedsRecreate_ = false;
        surfaceRecreateFailureLogged_ = false;
        OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=surface_recreated extent=%{public}ux%{public}u",
            swapchainExtent_.width, swapchainExtent_.height);
        return true;
    }

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags* actualProperties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
        uint32_t fallback = std::numeric_limits<uint32_t>::max();
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeFilter & (1U << index)) == 0) {
                continue;
            }
            const VkMemoryPropertyFlags properties = memoryProperties.memoryTypes[index].propertyFlags;
            if ((properties & required) != required) {
                continue;
            }
            if (fallback == std::numeric_limits<uint32_t>::max()) {
                fallback = index;
            }
            if ((properties & preferred) == preferred) {
                if (actualProperties != nullptr) {
                    *actualProperties = properties;
                }
                return index;
            }
        }
        if (fallback != std::numeric_limits<uint32_t>::max() && actualProperties != nullptr) {
            *actualProperties = memoryProperties.memoryTypes[fallback].propertyFlags;
        }
        return fallback;
    }

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred, BufferResource& buffer)
    {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer.buffer);
        if (result != VK_SUCCESS) {
            LogVkError("create_buffer", result);
            return false;
        }
        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(device_, buffer.buffer, &memoryRequirements);
        buffer.memoryFlags = 0;
        const uint32_t memoryType = FindMemoryType(memoryRequirements.memoryTypeBits, required, preferred,
            &buffer.memoryFlags);
        if (memoryType == std::numeric_limits<uint32_t>::max()) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=buffer_memory_type_missing");
            DestroyBuffer(buffer);
            return false;
        }
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device_, &allocateInfo, nullptr, &buffer.memory);
        if (result != VK_SUCCESS) {
            LogVkError("allocate_buffer_memory", result);
            DestroyBuffer(buffer);
            return false;
        }
        result = vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0);
        if (result != VK_SUCCESS) {
            LogVkError("bind_buffer_memory", result);
            DestroyBuffer(buffer);
            return false;
        }
        buffer.size = size;
        return true;
    }

    bool CreateImage(uint32_t width, uint32_t height, uint32_t layerCount, VkFormat format,
        VkImageUsageFlags usage, VkImageCreateFlags flags, VkImage& image, VkDeviceMemory& memory,
        uint32_t mipLevels = 1)
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = flags;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = layerCount;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(device_, &imageInfo, nullptr, &image);
        if (result != VK_SUCCESS) {
            LogVkError("create_image", result);
            return false;
        }
        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(device_, image, &memoryRequirements);
        VkMemoryPropertyFlags actualProperties = 0;
        const uint32_t memoryType = FindMemoryType(memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &actualProperties);
        if (memoryType == std::numeric_limits<uint32_t>::max()) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=image_memory_type_missing");
            vkDestroyImage(device_, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocateInfo.allocationSize = memoryRequirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(device_, &allocateInfo, nullptr, &memory);
        if (result != VK_SUCCESS) {
            LogVkError("allocate_image_memory", result);
            vkDestroyImage(device_, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }
        result = vkBindImageMemory(device_, image, memory, 0);
        if (result != VK_SUCCESS) {
            LogVkError("bind_image_memory", result);
            vkFreeMemory(device_, memory, nullptr);
            memory = VK_NULL_HANDLE;
            vkDestroyImage(device_, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool CreateImageView(VkImage image, VkFormat format, VkImageViewType viewType,
        uint32_t layerCount, VkImageAspectFlags aspectMask, VkImageView* view,
        uint32_t mipLevels = 1, uint32_t baseMipLevel = 0, uint32_t baseArrayLayer = 0)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = viewType;
        viewInfo.format = format;
        viewInfo.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        viewInfo.subresourceRange.aspectMask = aspectMask;
        viewInfo.subresourceRange.baseMipLevel = baseMipLevel;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
        viewInfo.subresourceRange.layerCount = layerCount;
        const VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, view);
        if (result != VK_SUCCESS) {
            LogVkError("create_image_view", result);
            return false;
        }
        return true;
    }

    VkCommandBuffer BeginOneTimeCommands()
    {
        VkCommandBufferAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocationInfo.commandPool = commandPool_;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &allocationInfo, &commandBuffer) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
            return VK_NULL_HANDLE;
        }
        return commandBuffer;
    }

    bool EndOneTimeCommands(VkCommandBuffer commandBuffer)
    {
        VkResult result = vkEndCommandBuffer(commandBuffer);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            result = vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE);
        }
        if (result == VK_SUCCESS) {
            result = vkQueueWaitIdle(queue_);
        }
        vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
        if (result != VK_SUCCESS) {
            LogVkError("one_time_command", result);
            return false;
        }
        return true;
    }

    bool TransitionImageLayout(VkImage image, uint32_t layerCount, VkImageAspectFlags aspectMask,
        VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels = 1)
    {
        VkCommandBuffer commandBuffer = BeginOneTimeCommands();
        if (commandBuffer == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=begin_image_transition_failed");
            return false;
        }
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspectMask;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layerCount;

        VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=unsupported_image_transition");
            vkEndCommandBuffer(commandBuffer);
            vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
            return false;
        }
        vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        return EndOneTimeCommands(commandBuffer);
    }

    bool CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height,
        uint32_t layerCount)
    {
        VkCommandBuffer commandBuffer = BeginOneTimeCommands();
        if (commandBuffer == VK_NULL_HANDLE) {
            return false;
        }
        const VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) * height * 4;
        std::vector<VkBufferImageCopy> regions(layerCount);
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            regions[layer].bufferOffset = layerSize * layer;
            regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[layer].imageSubresource.mipLevel = 0;
            regions[layer].imageSubresource.baseArrayLayer = layer;
            regions[layer].imageSubresource.layerCount = 1;
            regions[layer].imageExtent = {width, height, 1};
        }
        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(regions.size()), regions.data());
        return EndOneTimeCommands(commandBuffer);
    }

    bool CopyBufferToImageMipChain(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height,
        uint32_t layerCount, uint32_t mipLevels, const std::vector<VkDeviceSize>& levelOffsets)
    {
        VkCommandBuffer commandBuffer = BeginOneTimeCommands();
        if (commandBuffer == VK_NULL_HANDLE) {
            return false;
        }
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(static_cast<size_t>(layerCount) * mipLevels);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        for (uint32_t level = 0; level < mipLevels; ++level) {
            const VkDeviceSize layerSize = static_cast<VkDeviceSize>(mipWidth) * mipHeight * 4;
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                VkBufferImageCopy region{};
                region.bufferOffset = levelOffsets[level] + layerSize * layer;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = level;
                region.imageSubresource.baseArrayLayer = layer;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {mipWidth, mipHeight, 1};
                regions.push_back(region);
            }
            mipWidth = std::max(mipWidth / 2U, 1U);
            mipHeight = std::max(mipHeight / 2U, 1U);
        }
        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(regions.size()), regions.data());
        return EndOneTimeCommands(commandBuffer);
    }

    bool GenerateTextureMipmaps(VkImage image, uint32_t width, uint32_t height,
        uint32_t layerCount, uint32_t mipLevels)
    {
        if (mipLevels <= 1) {
            return TransitionImageLayout(image, layerCount, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        VkCommandBuffer commandBuffer = BeginOneTimeCommands();
        if (commandBuffer == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=begin_texture_mipmap_failed");
            return false;
        }
        int32_t mipWidth = static_cast<int32_t>(width);
        int32_t mipHeight = static_cast<int32_t>(height);
        for (uint32_t level = 1; level < mipLevels; ++level) {
            VkImageMemoryBarrier sourceBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            sourceBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            sourceBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sourceBarrier.image = image;
            sourceBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sourceBarrier.subresourceRange.baseMipLevel = level - 1;
            sourceBarrier.subresourceRange.levelCount = 1;
            sourceBarrier.subresourceRange.baseArrayLayer = 0;
            sourceBarrier.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &sourceBarrier);

            const int32_t nextWidth = std::max(1, mipWidth / 2);
            const int32_t nextHeight = std::max(1, mipHeight / 2);
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = layerCount;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = layerCount;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
            vkCmdBlitImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            VkImageMemoryBarrier readyBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            readyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            readyBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            readyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            readyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            readyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readyBarrier.image = image;
            readyBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            readyBarrier.subresourceRange.baseMipLevel = level - 1;
            readyBarrier.subresourceRange.levelCount = 1;
            readyBarrier.subresourceRange.baseArrayLayer = 0;
            readyBarrier.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &readyBarrier);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
        VkImageMemoryBarrier finalBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        finalBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        finalBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        finalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        finalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        finalBarrier.image = image;
        finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        finalBarrier.subresourceRange.baseMipLevel = mipLevels - 1;
        finalBarrier.subresourceRange.levelCount = 1;
        finalBarrier.subresourceRange.baseArrayLayer = 0;
        finalBarrier.subresourceRange.layerCount = layerCount;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &finalBarrier);
        return EndOneTimeCommands(commandBuffer);
    }

    bool CreateTextureResource(const std::vector<const unsigned char*>& layers, uint32_t width,
        uint32_t height, bool cube, TextureResource& texture, bool generateMipmaps = false)
    {
        if (layers.empty() || width == 0 || height == 0) {
            return false;
        }
        uint32_t mipLevels = 1;
        if (generateMipmaps) {
            const uint32_t maxDimension = std::max(width, height);
            mipLevels = 1;
            for (uint32_t dimension = maxDimension; dimension > 1; dimension >>= 1) {
                ++mipLevels;
            }
        }
        std::vector<VkDeviceSize> levelOffsets(mipLevels, 0);
        std::vector<uint32_t> mipWidths(mipLevels, 1);
        std::vector<uint32_t> mipHeights(mipLevels, 1);
        VkDeviceSize totalSize = 0;
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        for (uint32_t level = 0; level < mipLevels; ++level) {
            levelOffsets[level] = totalSize;
            mipWidths[level] = mipWidth;
            mipHeights[level] = mipHeight;
            totalSize += static_cast<VkDeviceSize>(mipWidth) * mipHeight * 4 * layers.size();
            mipWidth = std::max(mipWidth / 2U, 1U);
            mipHeight = std::max(mipHeight / 2U, 1U);
        }
        if (totalSize == 0) {
            return false;
        }
        const VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!CreateImage(width, height, static_cast<uint32_t>(layers.size()), VK_FORMAT_R8G8B8A8_UNORM,
            imageUsage, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, texture.image, texture.memory,
            mipLevels)) {
            return false;
        }
        texture.layerCount = static_cast<uint32_t>(layers.size());
        texture.mipLevels = mipLevels;
        BufferResource staging;
        if (!CreateBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
            DestroyTexture(texture);
            return false;
        }
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, staging.memory, 0, totalSize, 0, &mapped);
        if (result != VK_SUCCESS) {
            LogVkError("map_texture_staging_memory", result);
            DestroyBuffer(staging);
            DestroyTexture(texture);
            return false;
        }
        auto* upload = static_cast<unsigned char*>(mapped);
        const VkDeviceSize baseLayerSize = static_cast<VkDeviceSize>(width) * height * 4;
        for (size_t layer = 0; layer < layers.size(); ++layer) {
            std::memcpy(upload + levelOffsets[0] + baseLayerSize * layer, layers[layer],
                static_cast<size_t>(baseLayerSize));
        }
        for (uint32_t level = 1; level < mipLevels; ++level) {
            const uint32_t previousWidth = mipWidths[level - 1];
            const uint32_t previousHeight = mipHeights[level - 1];
            const uint32_t currentWidth = mipWidths[level];
            const uint32_t currentHeight = mipHeights[level];
            const VkDeviceSize previousLayerSize = static_cast<VkDeviceSize>(previousWidth) *
                previousHeight * 4;
            const VkDeviceSize currentLayerSize = static_cast<VkDeviceSize>(currentWidth) *
                currentHeight * 4;
            for (size_t layer = 0; layer < layers.size(); ++layer) {
                const auto* source = upload + levelOffsets[level - 1] + previousLayerSize * layer;
                auto* destination = upload + levelOffsets[level] + currentLayerSize * layer;
                for (uint32_t y = 0; y < currentHeight; ++y) {
                    for (uint32_t x = 0; x < currentWidth; ++x) {
                        uint32_t sum[4] = {0, 0, 0, 0};
                        for (uint32_t dy = 0; dy < 2; ++dy) {
                            for (uint32_t dx = 0; dx < 2; ++dx) {
                                const uint32_t sampleX = std::min(x * 2U + dx, previousWidth - 1U);
                                const uint32_t sampleY = std::min(y * 2U + dy, previousHeight - 1U);
                                const size_t sourceOffset =
                                    (static_cast<size_t>(sampleY) * previousWidth + sampleX) * 4;
                                for (int channel = 0; channel < 4; ++channel) {
                                    sum[channel] += source[sourceOffset + channel];
                                }
                            }
                        }
                        const size_t destinationOffset =
                            (static_cast<size_t>(y) * currentWidth + x) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            destination[destinationOffset + channel] =
                                static_cast<unsigned char>(sum[channel] / 4U);
                        }
                    }
                }
            }
        }
        if ((staging.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = staging.memory;
            range.offset = 0;
            range.size = totalSize;
            result = vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        vkUnmapMemory(device_, staging.memory);
        if (result != VK_SUCCESS) {
            LogVkError("flush_texture_staging_memory", result);
            DestroyBuffer(staging);
            DestroyTexture(texture);
            return false;
        }

        bool success = TransitionImageLayout(texture.image, texture.layerCount, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.mipLevels);
        if (success) {
            success = mipLevels == 1
                ? CopyBufferToImage(staging.buffer, texture.image, width, height, texture.layerCount)
                : CopyBufferToImageMipChain(staging.buffer, texture.image, width, height,
                    texture.layerCount, texture.mipLevels, levelOffsets);
        }
        if (success) {
            success = TransitionImageLayout(texture.image, texture.layerCount, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                texture.mipLevels);
        }
        DestroyBuffer(staging);
        if (!success) {
            DestroyTexture(texture);
            return false;
        }
        const VkImageViewType viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        if (!CreateImageView(texture.image, VK_FORMAT_R8G8B8A8_UNORM, viewType, texture.layerCount,
            VK_IMAGE_ASPECT_COLOR_BIT, &texture.view, texture.mipLevels)) {
            DestroyTexture(texture);
            return false;
        }
        return true;
    }

    // Uploads an explicitly authored RGBA8 mip chain.  The GLES path renders
    // its irradiance and GGX prefilter maps into separate cubemaps; those maps
    // cannot be represented by the raw skybox box-filter chain above, so the
    // Vulkan path keeps every authored level and uploads it in one transfer.
    bool CreateTextureResourceMipChain(const std::vector<std::vector<unsigned char>>& levels,
        uint32_t width, uint32_t height, bool cube, TextureResource& texture)
    {
        if (levels.empty() || width == 0 || height == 0) {
            return false;
        }
        const uint32_t layerCount = cube ? 6U : 1U;
        std::vector<VkDeviceSize> levelOffsets(levels.size(), 0);
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        VkDeviceSize totalSize = 0;
        for (size_t level = 0; level < levels.size(); ++level) {
            const VkDeviceSize expectedSize = static_cast<VkDeviceSize>(mipWidth) * mipHeight * 4 * layerCount;
            if (levels[level].size() != static_cast<size_t>(expectedSize)) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_mip_size_mismatch level=%{public}u expected=%{public}llu actual=%{public}llu",
                    static_cast<unsigned int>(level), static_cast<unsigned long long>(expectedSize),
                    static_cast<unsigned long long>(levels[level].size()));
                return false;
            }
            levelOffsets[level] = totalSize;
            totalSize += expectedSize;
            mipWidth = std::max(mipWidth / 2U, 1U);
            mipHeight = std::max(mipHeight / 2U, 1U);
        }
        if (!CreateImage(width, height, layerCount, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, texture.image, texture.memory,
                static_cast<uint32_t>(levels.size()))) {
            return false;
        }
        texture.layerCount = layerCount;
        texture.mipLevels = static_cast<uint32_t>(levels.size());
        BufferResource staging;
        if (!CreateBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) {
            DestroyTexture(texture);
            return false;
        }
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, staging.memory, 0, totalSize, 0, &mapped);
        if (result != VK_SUCCESS) {
            LogVkError("map_ibl_staging_memory", result);
            DestroyBuffer(staging);
            DestroyTexture(texture);
            return false;
        }
        auto* upload = static_cast<unsigned char*>(mapped);
        for (size_t level = 0; level < levels.size(); ++level) {
            std::memcpy(upload + levelOffsets[level], levels[level].data(), levels[level].size());
        }
        if ((staging.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = staging.memory;
            range.offset = 0;
            range.size = totalSize;
            result = vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        vkUnmapMemory(device_, staging.memory);
        if (result != VK_SUCCESS) {
            LogVkError("flush_ibl_staging_memory", result);
            DestroyBuffer(staging);
            DestroyTexture(texture);
            return false;
        }
        bool success = TransitionImageLayout(texture.image, texture.layerCount, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.mipLevels);
        if (success) {
            success = CopyBufferToImageMipChain(staging.buffer, texture.image, width, height,
                texture.layerCount, texture.mipLevels, levelOffsets);
        }
        if (success) {
            success = TransitionImageLayout(texture.image, texture.layerCount, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                texture.mipLevels);
        }
        DestroyBuffer(staging);
        if (!success) {
            DestroyTexture(texture);
            return false;
        }
        const VkImageViewType viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        if (!CreateImageView(texture.image, VK_FORMAT_R8G8B8A8_UNORM, viewType, texture.layerCount,
                VK_IMAGE_ASPECT_COLOR_BIT, &texture.view, texture.mipLevels)) {
            DestroyTexture(texture);
            return false;
        }
        return true;
    }

    bool CreateCheckerTexture()
    {
        constexpr uint32_t textureSize = 64;
        constexpr uint32_t tileSize = 8;
        std::array<unsigned char, textureSize * textureSize * 4> pixels{};
        for (uint32_t y = 0; y < textureSize; ++y) {
            for (uint32_t x = 0; x < textureSize; ++x) {
                const unsigned char value = (((x / tileSize) + (y / tileSize)) & 1) ? 255 : 36;
                const size_t offset = (static_cast<size_t>(y) * textureSize + x) * 4;
                pixels[offset + 0] = value;
                pixels[offset + 1] = value;
                pixels[offset + 2] = value;
                pixels[offset + 3] = 255;
            }
        }
        const std::vector<const unsigned char*> layers = {pixels.data()};
        if (!CreateTextureResource(layers, textureSize, textureSize, false, cubeTexture_)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=checker_texture_failed");
            return false;
        }
        return true;
    }

    bool CreateWhiteTexture()
    {
        static const unsigned char white[4] = {255, 255, 255, 255};
        const std::vector<const unsigned char*> layers = {white};
        return CreateTextureResource(layers, 1, 1, false, whiteTexture_);
    }

    bool CreateMaterialFallbackTextures()
    {
        // Keep optional glTF maps neutral, matching the GLES fallback
        // textures: metallic=0, roughness=1, flat tangent normal, AO=1 and
        // emissive=0.  These are material defaults, not artistic albedo
        // adjustments.
        static const unsigned char neutralMr[4] = {0, 255, 255, 255};
        static const unsigned char neutralNormal[4] = {128, 128, 255, 255};
        static const unsigned char neutralAo[4] = {255, 255, 255, 255};
        static const unsigned char black[4] = {0, 0, 0, 255};
        const std::vector<const unsigned char*> mrLayers = {neutralMr};
        const std::vector<const unsigned char*> normalLayers = {neutralNormal};
        const std::vector<const unsigned char*> aoLayers = {neutralAo};
        const std::vector<const unsigned char*> emissiveLayers = {black};
        return CreateTextureResource(mrLayers, 1, 1, false, neutralMrTexture_) &&
            CreateTextureResource(normalLayers, 1, 1, false, neutralNormalTexture_) &&
            CreateTextureResource(aoLayers, 1, 1, false, neutralAoTexture_) &&
            CreateTextureResource(emissiveLayers, 1, 1, false, blackTexture_);
    }

    bool CreateVulkanIblResources(const std::array<stbi_uc*, 6>& faces, uint32_t width,
        uint32_t height)
    {
        if (width == 0 || height == 0) {
            return false;
        }
        struct CpuVec3 {
            float x;
            float y;
            float z;
        };
        const float pi = 3.14159265358979323846f;
        const auto add = [](CpuVec3 a, CpuVec3 b) {
            return CpuVec3{a.x + b.x, a.y + b.y, a.z + b.z};
        };
        const auto mul = [](CpuVec3 a, float scalar) {
            return CpuVec3{a.x * scalar, a.y * scalar, a.z * scalar};
        };
        const auto dot = [](CpuVec3 a, CpuVec3 b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        const auto cross = [](CpuVec3 a, CpuVec3 b) {
            return CpuVec3{
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x,
            };
        };
        const auto normalize = [](CpuVec3 value) {
            const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
            if (length <= 1.0e-6f) {
                return CpuVec3{0.0f, 0.0f, 1.0f};
            }
            return CpuVec3{value.x / length, value.y / length, value.z / length};
        };
        // This is the inverse of the exact cube-face basis used by the GLES
        // convolution vertex shader.  Keeping it here (instead of relying on
        // a host image library's cubemap convention) makes the CPU-generated
        // maps sample the same six source images as GLES.
        const auto faceUv = [](CpuVec3 direction, int& face, float& u, float& v) {
            const float ax = std::fabs(direction.x);
            const float ay = std::fabs(direction.y);
            const float az = std::fabs(direction.z);
            if (ax >= ay && ax >= az) {
                if (direction.x >= 0.0f) {
                    face = 0;
                    u = -direction.z / ax;
                    v = -direction.y / ax;
                } else {
                    face = 1;
                    u = direction.z / ax;
                    v = -direction.y / ax;
                }
            } else if (ay >= az) {
                if (direction.y >= 0.0f) {
                    face = 2;
                    u = direction.x / ay;
                    v = direction.z / ay;
                } else {
                    face = 3;
                    u = direction.x / ay;
                    v = -direction.z / ay;
                }
            } else if (direction.z >= 0.0f) {
                face = 4;
                u = direction.x / az;
                v = -direction.y / az;
            } else {
                face = 5;
                u = -direction.x / az;
                v = -direction.y / az;
            }
        };
        const auto faceDirection = [](int face, float u, float v) {
            switch (face) {
                case 0:
                    return CpuVec3{1.0f, -v, -u};
                case 1:
                    return CpuVec3{-1.0f, -v, u};
                case 2:
                    return CpuVec3{u, 1.0f, v};
                case 3:
                    return CpuVec3{u, -1.0f, -v};
                case 4:
                    return CpuVec3{u, -v, 1.0f};
                default:
                    return CpuVec3{-u, -v, -1.0f};
            }
        };
        const auto sampleLinear = [&](CpuVec3 direction) {
            direction = normalize(direction);
            int face = 0;
            float u = 0.0f;
            float v = 0.0f;
            faceUv(direction, face, u, v);
            const float x = std::clamp((u * 0.5f + 0.5f) * static_cast<float>(width - 1), 0.0f,
                static_cast<float>(width - 1));
            const float y = std::clamp((v * 0.5f + 0.5f) * static_cast<float>(height - 1), 0.0f,
                static_cast<float>(height - 1));
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const int x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
            const int y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
            const float tx = x - static_cast<float>(x0);
            const float ty = y - static_cast<float>(y0);
            const auto texel = [&](int px, int py, int channel) {
                const size_t offset = (static_cast<size_t>(py) * width + static_cast<size_t>(px)) * 4 +
                    static_cast<size_t>(channel);
                return static_cast<float>(faces[face][offset]) / 255.0f;
            };
            std::array<float, 3> result{};
            for (int channel = 0; channel < 3; ++channel) {
                const float top = texel(x0, y0, channel) * (1.0f - tx) + texel(x1, y0, channel) * tx;
                const float bottom = texel(x0, y1, channel) * (1.0f - tx) + texel(x1, y1, channel) * tx;
                const float srgb = top * (1.0f - ty) + bottom * ty;
                result[channel] = std::pow(std::max(srgb, 0.0f), 2.2f);
            }
            return result;
        };
        const auto writeLinear = [](std::vector<unsigned char>& destination, size_t offset,
            const std::array<float, 3>& color) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value = std::clamp(color[channel], 0.0f, 1.0f);
                destination[offset + static_cast<size_t>(channel)] =
                    static_cast<unsigned char>(std::lround(value * 255.0f));
            }
            destination[offset + 3] = 255;
        };

        // Precompute the same fixed sample locations as GLES' irradiance pass.
        struct IrradianceSample {
            CpuVec3 tangent;
            float weight;
        };
        std::vector<IrradianceSample> irradianceSamples;
        for (float phi = 0.0f; phi < 2.0f * pi; phi += 0.05f) {
            for (float theta = 0.0f; theta < 0.5f * pi; theta += 0.05f) {
                irradianceSamples.push_back({
                    CpuVec3{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi),
                        std::cos(theta)},
                    std::cos(theta) * std::sin(theta),
                });
            }
        }

        constexpr uint32_t kIrradianceSize = 32;
        std::vector<unsigned char> irradiance(static_cast<size_t>(kIrradianceSize) * kIrradianceSize * 6 * 4);
        for (int face = 0; face < 6; ++face) {
            for (uint32_t y = 0; y < kIrradianceSize; ++y) {
                for (uint32_t x = 0; x < kIrradianceSize; ++x) {
                    const float u = (2.0f * (static_cast<float>(x) + 0.5f) /
                        static_cast<float>(kIrradianceSize)) - 1.0f;
                    const float v = (2.0f * (static_cast<float>(y) + 0.5f) /
                        static_cast<float>(kIrradianceSize)) - 1.0f;
                    const CpuVec3 normal = normalize(faceDirection(face, u, v));
                    CpuVec3 up = std::fabs(normal.y) < 0.999f
                        ? CpuVec3{0.0f, 1.0f, 0.0f} : CpuVec3{1.0f, 0.0f, 0.0f};
                    const CpuVec3 right = normalize(cross(up, normal));
                    up = cross(normal, right);
                    std::array<float, 3> color{};
                    for (const IrradianceSample& sample : irradianceSamples) {
                        const CpuVec3 sampleDirection = add(add(mul(right, sample.tangent.x),
                            mul(up, sample.tangent.y)), mul(normal, sample.tangent.z));
                        const std::array<float, 3> source = sampleLinear(sampleDirection);
                        for (int channel = 0; channel < 3; ++channel) {
                            color[channel] += source[channel] * sample.weight;
                        }
                    }
                    for (float& channel : color) {
                        channel = pi * channel / static_cast<float>(irradianceSamples.size());
                    }
                    const size_t offset = (static_cast<size_t>(face) * kIrradianceSize * kIrradianceSize +
                        static_cast<size_t>(y) * kIrradianceSize + x) * 4;
                    writeLinear(irradiance, offset, color);
                }
            }
        }
        const std::vector<std::vector<unsigned char>> irradianceLevels = {std::move(irradiance)};
        if (!CreateTextureResourceMipChain(irradianceLevels, kIrradianceSize, kIrradianceSize, true,
                irradianceTexture_)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=irradiance_upload_failed");
            DestroyTexture(irradianceTexture_);
            return false;
        }
        SDL_Log("SDL3_VULKAN stage=irradiance_ready size=%u samples=%u", kIrradianceSize,
            static_cast<unsigned int>(irradianceSamples.size()));

        auto radicalInverse = [](std::uint32_t bits) {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        };
        constexpr uint32_t kPrefilterSize = 128;
        constexpr uint32_t kPrefilterLevels = 6;
        constexpr uint32_t kPrefilterSamples = 64;
        std::array<std::array<float, 2>, kPrefilterSamples> hammersley{};
        for (uint32_t sample = 0; sample < kPrefilterSamples; ++sample) {
            hammersley[sample] = {static_cast<float>(sample) / static_cast<float>(kPrefilterSamples),
                radicalInverse(sample)};
        }
        std::vector<std::vector<unsigned char>> prefilteredLevels;
        prefilteredLevels.reserve(kPrefilterLevels);
        for (uint32_t level = 0; level < kPrefilterLevels; ++level) {
            const uint32_t size = kPrefilterSize >> level;
            const float roughness = static_cast<float>(level) /
                static_cast<float>(kPrefilterLevels - 1);
            std::vector<unsigned char> output(static_cast<size_t>(size) * size * 6 * 4);
            for (int face = 0; face < 6; ++face) {
                for (uint32_t y = 0; y < size; ++y) {
                    for (uint32_t x = 0; x < size; ++x) {
                        const float u = (2.0f * (static_cast<float>(x) + 0.5f) /
                            static_cast<float>(size)) - 1.0f;
                        const float v = (2.0f * (static_cast<float>(y) + 0.5f) /
                            static_cast<float>(size)) - 1.0f;
                        const CpuVec3 normal = normalize(faceDirection(face, u, v));
                        const CpuVec3 view = normal;
                        std::array<float, 3> color{};
                        float weight = 0.0f;
                        const float alpha = roughness * roughness;
                        for (const std::array<float, 2>& xi : hammersley) {
                            const float phi = 2.0f * pi * xi[0];
                            const float denominator = 1.0f + (alpha * alpha - 1.0f) * xi[1];
                            const float cosTheta = std::sqrt(std::max((1.0f - xi[1]) /
                                std::max(denominator, 1.0e-6f), 0.0f));
                            const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
                            const CpuVec3 halfVector = {
                                sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
                            const CpuVec3 up = std::fabs(normal.z) < 0.999f
                                ? CpuVec3{0.0f, 0.0f, 1.0f} : CpuVec3{1.0f, 0.0f, 0.0f};
                            const CpuVec3 tangent = normalize(cross(up, normal));
                            const CpuVec3 bitangent = cross(normal, tangent);
                            const CpuVec3 halfWorld = normalize(add(add(mul(tangent, halfVector.x),
                                mul(bitangent, halfVector.y)), mul(normal, halfVector.z)));
                            const CpuVec3 light = normalize(add(mul(halfWorld, 2.0f * dot(view, halfWorld)),
                                mul(view, -1.0f)));
                            const float noLight = std::max(dot(normal, light), 0.0f);
                            if (noLight <= 0.0f) {
                                continue;
                            }
                            const std::array<float, 3> source = sampleLinear(light);
                            for (int channel = 0; channel < 3; ++channel) {
                                color[channel] += source[channel] * noLight;
                            }
                            weight += noLight;
                        }
                        for (float& channel : color) {
                            channel /= std::max(weight, 1.0e-4f);
                        }
                        const size_t offset = (static_cast<size_t>(face) * size * size +
                            static_cast<size_t>(y) * size + x) * 4;
                        writeLinear(output, offset, color);
                    }
                }
            }
            prefilteredLevels.push_back(std::move(output));
        }
        if (!CreateTextureResourceMipChain(prefilteredLevels, kPrefilterSize, kPrefilterSize, true,
                prefilteredTexture_)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=prefilter_upload_failed");
            DestroyTexture(irradianceTexture_);
            DestroyTexture(prefilteredTexture_);
            return false;
        }
        SDL_Log("SDL3_VULKAN stage=prefilter_ready size=%u levels=%u samples=%u", kPrefilterSize,
            kPrefilterLevels, kPrefilterSamples);

        void* encoded = nullptr;
        size_t encodedSize = 0;
        if (!OHOS_ReadRawFile("BrdfLut.png", &encoded, &encodedSize) || encoded == nullptr ||
            encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            if (encoded != nullptr) {
                OHOS_FreeRawFile(encoded);
            }
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=brdf_lut_rawfile_failed");
            DestroyTexture(irradianceTexture_);
            DestroyTexture(prefilteredTexture_);
            return false;
        }
        int lutWidth = 0;
        int lutHeight = 0;
        int lutChannels = 0;
        stbi_uc* lutPixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
            static_cast<int>(encodedSize), &lutWidth, &lutHeight, &lutChannels, 4);
        OHOS_FreeRawFile(encoded);
        if (lutPixels == nullptr || lutWidth <= 0 || lutHeight <= 0) {
            if (lutPixels != nullptr) {
                stbi_image_free(lutPixels);
            }
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=brdf_lut_decode_failed");
            DestroyTexture(irradianceTexture_);
            DestroyTexture(prefilteredTexture_);
            return false;
        }
        const std::vector<const unsigned char*> lutLayers = {lutPixels};
        const bool lutSuccess = CreateTextureResource(lutLayers, static_cast<uint32_t>(lutWidth),
            static_cast<uint32_t>(lutHeight), false, brdfLutTexture_);
        stbi_image_free(lutPixels);
        if (!lutSuccess) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=brdf_lut_upload_failed");
            DestroyTexture(irradianceTexture_);
            DestroyTexture(prefilteredTexture_);
            DestroyTexture(brdfLutTexture_);
            return false;
        }
        SDL_Log("SDL3_VULKAN stage=brdf_lut_ready size=%dx%d", lutWidth, lutHeight);
        return true;
    }

    bool CreateSkyboxTexture()
    {
        const std::array<const char*, 6> paths = {
            "skybox/right.jpg",
            "skybox/left.jpg",
            "skybox/top.jpg",
            "skybox/bottom.jpg",
            "skybox/front.jpg",
            "skybox/back.jpg",
        };
        std::array<stbi_uc*, 6> pixels{};
        std::vector<const unsigned char*> layers;
        int expectedWidth = 0;
        int expectedHeight = 0;
        bool success = true;

        for (size_t index = 0; index < paths.size(); ++index) {
            void* encoded = nullptr;
            size_t encodedSize = 0;
            if (!OHOS_ReadRawFile(paths[index], &encoded, &encodedSize) || encoded == nullptr ||
                encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=skybox_rawfile_failed path=%{public}s", paths[index]);
                if (encoded != nullptr) {
                    OHOS_FreeRawFile(encoded);
                }
                success = false;
                break;
            }
            int channels = 0;
            int width = 0;
            int height = 0;
            pixels[index] = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
                static_cast<int>(encodedSize), &width, &height, &channels, 4);
            OHOS_FreeRawFile(encoded);
            if (pixels[index] == nullptr) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=skybox_decode_failed path=%{public}s",
                    paths[index]);
                success = false;
                break;
            }
            if (index == 0) {
                expectedWidth = width;
                expectedHeight = height;
            } else if (width != expectedWidth || height != expectedHeight) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=skybox_size_mismatch path=%{public}s", paths[index]);
                success = false;
                break;
            }
            layers.push_back(pixels[index]);
        }

        if (success) {
            success = CreateTextureResource(layers, static_cast<uint32_t>(expectedWidth),
                static_cast<uint32_t>(expectedHeight), true, skyboxTexture_,
                kEnableVulkanSkyboxMipmaps);
        }
        for (stbi_uc* image : pixels) {
            if (image != nullptr) {
                stbi_image_free(image);
            }
        }
        if (!success) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=skybox_texture_failed");
            return false;
        }
        OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=skybox_ready faces=6 size=%{public}dx%{public}d mips=%{public}u",
            expectedWidth, expectedHeight, skyboxTexture_.mipLevels);
        return true;
    }

    bool CreateTextAtlas()
    {
        void* encoded = nullptr;
        size_t encodedSize = 0;
        if (!OHOS_ReadRawFile("SdfAtlas.png", &encoded, &encodedSize) || encoded == nullptr ||
            encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
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
        if (pixels == nullptr || width <= 0 || height <= 0) {
            if (pixels != nullptr) {
                stbi_image_free(pixels);
            }
            return false;
        }
        const std::vector<const unsigned char*> layers = {pixels};
        const bool success = CreateTextureResource(layers, static_cast<uint32_t>(width),
            static_cast<uint32_t>(height), false, textAtlasTexture_);
        stbi_image_free(pixels);
        if (success) {
            SDL_Log("SDL3_VULKAN stage=sdf_text_ready atlas=%dx%d", width, height);
        }
        return success;
    }

    bool CreateActionIconTextures()
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
                SDL_Log("SDL3_VULKAN stage=touch_icon_missing index=%d path=%s", index, kPaths[index]);
                continue;
            }
            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
                static_cast<int>(encodedSize), &width, &height, &channels, 4);
            OHOS_FreeRawFile(encoded);
            if (pixels == nullptr || width <= 0 || height <= 0) {
                if (pixels != nullptr) {
                    stbi_image_free(pixels);
                }
                SDL_Log("SDL3_VULKAN stage=touch_icon_decode_failed index=%d path=%s",
                    index, kPaths[index]);
                continue;
            }
            const std::vector<const unsigned char*> layers = {pixels};
            const bool success = CreateTextureResource(layers, static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), false, actionIconTextures_[index]);
            stbi_image_free(pixels);
            if (success) {
                ++loaded;
            }
        }
        SDL_Log("SDL3_VULKAN stage=touch_icons_ready count=%d", loaded);
        return loaded == rhi::CameraInput::kActionButtonCount;
    }

    bool CreateVulkanGpuIblResources()
    {
        // The GLES reference path builds these maps with fragment shaders.  Do
        // the same work in a short Vulkan render pass so boot does not spend
        // tens of seconds convolving the skybox on the CPU.
        constexpr uint32_t kIrradianceSize = 32;
        constexpr uint32_t kPrefilterSize = 128;
        constexpr uint32_t kPrefilterLevels = 6;
        constexpr VkFormat kIblFormat = VK_FORMAT_R8G8B8A8_UNORM;

        TextureResource generatedIrradiance{};
        TextureResource generatedPrefiltered{};
        TextureResource generatedBrdfLut{};
        std::vector<VkImageView> transientViews;
        std::vector<VkFramebuffer> transientFramebuffers;
        VkRenderPass iblRenderPass = VK_NULL_HANDLE;
        VkDescriptorSetLayout iblDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout iblPipelineLayout = VK_NULL_HANDLE;
        VkShaderModule iblVertexShader = VK_NULL_HANDLE;
        VkShaderModule irradianceFragmentShader = VK_NULL_HANDLE;
        VkShaderModule prefilterFragmentShader = VK_NULL_HANDLE;
        VkPipeline irradiancePipeline = VK_NULL_HANDLE;
        VkPipeline prefilterPipeline = VK_NULL_HANDLE;
        VkDescriptorSet iblDescriptorSet = VK_NULL_HANDLE;

        auto cleanupTransient = [&]() {
            for (VkFramebuffer framebuffer : transientFramebuffers) {
                if (framebuffer != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(device_, framebuffer, nullptr);
                }
            }
            transientFramebuffers.clear();
            for (VkImageView view : transientViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device_, view, nullptr);
                }
            }
            transientViews.clear();
            if (irradiancePipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, irradiancePipeline, nullptr);
                irradiancePipeline = VK_NULL_HANDLE;
            }
            if (prefilterPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, prefilterPipeline, nullptr);
                prefilterPipeline = VK_NULL_HANDLE;
            }
            if (iblVertexShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, iblVertexShader, nullptr);
                iblVertexShader = VK_NULL_HANDLE;
            }
            if (irradianceFragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, irradianceFragmentShader, nullptr);
                irradianceFragmentShader = VK_NULL_HANDLE;
            }
            if (prefilterFragmentShader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_, prefilterFragmentShader, nullptr);
                prefilterFragmentShader = VK_NULL_HANDLE;
            }
            if (iblPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, iblPipelineLayout, nullptr);
                iblPipelineLayout = VK_NULL_HANDLE;
            }
            if (iblDescriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, iblDescriptorSetLayout, nullptr);
                iblDescriptorSetLayout = VK_NULL_HANDLE;
            }
            if (iblRenderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, iblRenderPass, nullptr);
                iblRenderPass = VK_NULL_HANDLE;
            }
        };
        auto fail = [&]() {
            cleanupTransient();
            DestroyTexture(generatedIrradiance);
            DestroyTexture(generatedPrefiltered);
            DestroyTexture(generatedBrdfLut);
            return false;
        };

        const auto createOutputImage = [this, kIblFormat](uint32_t size, uint32_t mipLevels,
            TextureResource& texture) {
            if (!CreateImage(size, size, 6, kIblFormat,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, texture.image, texture.memory, mipLevels)) {
                return false;
            }
            texture.layerCount = 6;
            texture.mipLevels = mipLevels;
            return true;
        };
        if (!createOutputImage(kIrradianceSize, 1, generatedIrradiance) ||
            !createOutputImage(kPrefilterSize, kPrefilterLevels, generatedPrefiltered)) {
            return fail();
        }

        VkAttachmentDescription attachment{};
        attachment.format = kIblFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkSubpassDependency toColor{};
        toColor.srcSubpass = VK_SUBPASS_EXTERNAL;
        toColor.dstSubpass = 0;
        toColor.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        toColor.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkSubpassDependency fromColor{};
        fromColor.srcSubpass = 0;
        fromColor.dstSubpass = VK_SUBPASS_EXTERNAL;
        fromColor.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        fromColor.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        fromColor.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        fromColor.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        const std::array<VkSubpassDependency, 2> dependencies = {toColor, fromColor};
        VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();
        if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &iblRenderPass) != VK_SUCCESS) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_renderpass_failed");
            return fail();
        }

        VkDescriptorSetLayoutBinding skyboxBinding{};
        skyboxBinding.binding = 0;
        skyboxBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        skyboxBinding.descriptorCount = 1;
        skyboxBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptorLayoutInfo.bindingCount = 1;
        descriptorLayoutInfo.pBindings = &skyboxBinding;
        if (vkCreateDescriptorSetLayout(device_, &descriptorLayoutInfo, nullptr,
                &iblDescriptorSetLayout) != VK_SUCCESS) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_descriptor_layout_failed");
            return fail();
        }
        VkDescriptorSetAllocateInfo descriptorAllocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptorAllocateInfo.descriptorPool = descriptorPool_;
        descriptorAllocateInfo.descriptorSetCount = 1;
        descriptorAllocateInfo.pSetLayouts = &iblDescriptorSetLayout;
        if (vkAllocateDescriptorSets(device_, &descriptorAllocateInfo, &iblDescriptorSet) != VK_SUCCESS) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_descriptor_allocate_failed");
            return fail();
        }
        VkDescriptorImageInfo skyboxInfo{};
        skyboxInfo.sampler = sampler_;
        skyboxInfo.imageView = skyboxTexture_.view;
        skyboxInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet descriptorWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        descriptorWrite.dstSet = iblDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.pImageInfo = &skyboxInfo;
        vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(int32_t) + sizeof(float);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &iblDescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                &iblPipelineLayout) != VK_SUCCESS) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_pipeline_layout_failed");
            return fail();
        }
        if (!CreateShaderModule(kSceneIblVertSpv, kSceneIblVertSpvWordCount, &iblVertexShader) ||
            !CreateShaderModule(kSceneIblIrradianceFragSpv, kSceneIblIrradianceFragSpvWordCount,
                &irradianceFragmentShader) ||
            !CreateShaderModule(kSceneIblPrefilterFragSpv, kSceneIblPrefilterFragSpvWordCount,
                &prefilterFragmentShader)) {
            return fail();
        }
        irradiancePipeline = CreatePipeline(iblVertexShader, irradianceFragmentShader,
            VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VertexLayout::kNone, VK_FALSE,
            iblRenderPass, iblPipelineLayout);
        prefilterPipeline = CreatePipeline(iblVertexShader, prefilterFragmentShader,
            VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VertexLayout::kNone, VK_FALSE,
            iblRenderPass, iblPipelineLayout);
        if (irradiancePipeline == VK_NULL_HANDLE || prefilterPipeline == VK_NULL_HANDLE) {
            return fail();
        }

        const auto createFramebuffer = [&](VkImage image, uint32_t mipLevel, uint32_t face,
            uint32_t size, VkFramebuffer* framebuffer) {
            VkImageView view = VK_NULL_HANDLE;
            if (!CreateImageView(image, kIblFormat, VK_IMAGE_VIEW_TYPE_2D, 1,
                    VK_IMAGE_ASPECT_COLOR_BIT, &view, 1, mipLevel, face)) {
                return false;
            }
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = iblRenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &view;
            framebufferInfo.width = size;
            framebufferInfo.height = size;
            framebufferInfo.layers = 1;
            const VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr, framebuffer);
            if (result != VK_SUCCESS) {
                LogVkError("create_ibl_framebuffer", result);
                vkDestroyImageView(device_, view, nullptr);
                return false;
            }
            transientViews.push_back(view);
            transientFramebuffers.push_back(*framebuffer);
            return true;
        };

        std::array<VkFramebuffer, 6> irradianceFramebuffers{};
        std::array<std::array<VkFramebuffer, 6>, kPrefilterLevels> prefilterFramebuffers{};
        for (uint32_t face = 0; face < 6; ++face) {
            if (!createFramebuffer(generatedIrradiance.image, 0, face, kIrradianceSize,
                    &irradianceFramebuffers[face])) {
                return fail();
            }
            for (uint32_t level = 0; level < kPrefilterLevels; ++level) {
                const uint32_t size = std::max(kPrefilterSize >> level, 1U);
                if (!createFramebuffer(generatedPrefiltered.image, level, face, size,
                        &prefilterFramebuffers[level][face])) {
                    return fail();
                }
            }
        }

        struct IblPushConstants {
            int32_t cubeFace;
            float roughness;
        };
        static_assert(sizeof(IblPushConstants) == 8, "IBL push constants must be 8 bytes");
        const auto recordPass = [&](VkCommandBuffer commandBuffer, VkFramebuffer framebuffer,
            VkPipeline pipeline, uint32_t size, uint32_t face, float roughness) {
            VkClearValue clearValue{};
            clearValue.color.float32[0] = 0.0f;
            clearValue.color.float32[1] = 0.0f;
            clearValue.color.float32[2] = 0.0f;
            clearValue.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo beginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            beginInfo.renderPass = iblRenderPass;
            beginInfo.framebuffer = framebuffer;
            beginInfo.renderArea.extent = {size, size};
            beginInfo.clearValueCount = 1;
            beginInfo.pClearValues = &clearValue;
            vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{};
            viewport.width = static_cast<float>(size);
            viewport.height = static_cast<float>(size);
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = {size, size};
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                iblPipelineLayout, 0, 1, &iblDescriptorSet, 0, nullptr);
            const IblPushConstants pushConstants{static_cast<int32_t>(face), roughness};
            vkCmdPushConstants(commandBuffer, iblPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                sizeof(pushConstants), &pushConstants);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(commandBuffer);
        };

        VkCommandBuffer commandBuffer = BeginOneTimeCommands();
        if (commandBuffer == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_command_begin_failed");
            return fail();
        }
        for (uint32_t face = 0; face < 6; ++face) {
            recordPass(commandBuffer, irradianceFramebuffers[face], irradiancePipeline,
                kIrradianceSize, face, 0.0f);
            for (uint32_t level = 0; level < kPrefilterLevels; ++level) {
                const uint32_t size = std::max(kPrefilterSize >> level, 1U);
                const float roughness = static_cast<float>(level) /
                    static_cast<float>(kPrefilterLevels - 1);
                recordPass(commandBuffer, prefilterFramebuffers[level][face], prefilterPipeline,
                    size, face, roughness);
            }
        }
        if (!EndOneTimeCommands(commandBuffer)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_render_failed");
            return fail();
        }

        if (!CreateImageView(generatedIrradiance.image, kIblFormat, VK_IMAGE_VIEW_TYPE_CUBE, 6,
                VK_IMAGE_ASPECT_COLOR_BIT, &generatedIrradiance.view, 1) ||
            !CreateImageView(generatedPrefiltered.image, kIblFormat, VK_IMAGE_VIEW_TYPE_CUBE, 6,
                VK_IMAGE_ASPECT_COLOR_BIT, &generatedPrefiltered.view, kPrefilterLevels)) {
            return fail();
        }

        void* encoded = nullptr;
        size_t encodedSize = 0;
        if (!OHOS_ReadRawFile("BrdfLut.png", &encoded, &encodedSize) || encoded == nullptr ||
            encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            if (encoded != nullptr) {
                OHOS_FreeRawFile(encoded);
            }
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_brdf_lut_read_failed");
            return fail();
        }
        int lutWidth = 0;
        int lutHeight = 0;
        int lutChannels = 0;
        stbi_uc* lutPixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
            static_cast<int>(encodedSize), &lutWidth, &lutHeight, &lutChannels, 4);
        OHOS_FreeRawFile(encoded);
        if (lutPixels == nullptr || lutWidth <= 0 || lutHeight <= 0) {
            if (lutPixels != nullptr) {
                stbi_image_free(lutPixels);
            }
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_brdf_lut_decode_failed");
            return fail();
        }
        const std::vector<const unsigned char*> lutLayers = {lutPixels};
        const bool lutSuccess = CreateTextureResource(lutLayers, static_cast<uint32_t>(lutWidth),
            static_cast<uint32_t>(lutHeight), false, generatedBrdfLut);
        stbi_image_free(lutPixels);
        if (!lutSuccess) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_brdf_lut_upload_failed");
            return fail();
        }

        cleanupTransient();
        DestroyTexture(irradianceTexture_);
        DestroyTexture(prefilteredTexture_);
        DestroyTexture(brdfLutTexture_);
        irradianceTexture_ = generatedIrradiance;
        prefilteredTexture_ = generatedPrefiltered;
        brdfLutTexture_ = generatedBrdfLut;
        generatedIrradiance = {};
        generatedPrefiltered = {};
        generatedBrdfLut = {};
        OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_ready irradiance=%{public}ux%{public}u "
            "prefilter=%{public}ux%{public}u levels=%{public}u",
            kIrradianceSize, kIrradianceSize, kPrefilterSize, kPrefilterSize, kPrefilterLevels);
        return true;
    }

    bool CreateSampler()
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        // REPEAT, matching glTF's implicit sampler: DamagedHelmet ships its UVs
        // in v [1,2] and relies on wrap to fold them back into [0,1].  Clamping
        // pins every fragment to the edge row and smears it into streaks.  The
        // cube and skybox never leave [0,1]/direction space, so this is safe.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = skyboxTexture_.mipLevels > 0
            ? static_cast<float>(skyboxTexture_.mipLevels - 1) : 0.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        const VkResult result = vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_);
        if (result != VK_SUCCESS) {
            LogVkError("create_texture_sampler", result);
            return false;
        }
        return true;
    }

    // The OHOS emulator's Vulkan implementation forwards every call to a
    // host-side proxy process that owns the real device.  Measurements on the
    // emulator show that the proxy only publishes guest-mapped memory to the
    // GPU when the mapping is released: sticking with a persistent mapping and
    // rewriting it every frame leaves the device reading zeros, which is why
    // shaders used to see an all-zero vertex buffer (and why uniform-buffer
    // reads were previously reported as unreliable).
    //
    // Every dynamic buffer is therefore updated with a full
    // map -> memcpy -> flush -> unmap cycle.  Flushing is issued
    // unconditionally because the reported memory type is HOST_COHERENT here,
    // which would otherwise skip the only sync the proxy understands.
    bool UploadToMappedBufferRange(BufferResource& buffer, const void* data, VkDeviceSize size,
        VkDeviceSize offset)
    {
        if (buffer.memory == VK_NULL_HANDLE || data == nullptr || size == 0 ||
            offset > buffer.size || size > buffer.size - offset) {
            return false;
        }
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device_, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            LogVkError("map_dynamic_buffer", result);
            return false;
        }
        std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, data, static_cast<size_t>(size));
        // No flush: the allocation is HOST_COHERENT (spec says host writes are
        // visible without a flush), and this emulator ICD additionally
        // publishes the mapping on unmap.  Dropping the flush removes one
        // proxy round-trip per frame, which matters at 60 fps.
        vkUnmapMemory(device_, buffer.memory);
        return true;
    }

    bool UploadToMappedBuffer(BufferResource& buffer, const void* data, VkDeviceSize size)
    {
        return UploadToMappedBufferRange(buffer, data, size, 0);
    }

    bool CreateCubeVertexBuffer()
    {
        const VkDeviceSize bufferSize = sizeof(CubeVertex) * kCubeVertexCount;
        return CreateBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, cubeVertexBuffer_);
    }

    bool CreateIdentityJointBuffer()
    {
        std::vector<Mat4> identity(kMaxJoints, Mat4Identity());
        if (!CreateBuffer(sizeof(Mat4) * kMaxJoints, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                identityJointBuffer_)) {
            return false;
        }
        if (!UploadToMappedBuffer(identityJointBuffer_, identity.data(),
                sizeof(Mat4) * kMaxJoints)) {
            DestroyBuffer(identityJointBuffer_);
            return false;
        }
        return true;
    }

    bool CreateModelInstanceBuffer()
    {
        const std::size_t staticCubeCount = sceneDefinition_ != nullptr
            ? sceneDefinition_->StaticCubeCount() : 0;
        modelInstanceData_.assign(4 + staticCubeCount, CharacterInstanceData{});
        return CreateBuffer(sizeof(CharacterInstanceData) * modelInstanceData_.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            modelInstanceBuffer_);
    }

    bool CreateDescriptorSetLayout()
    {
        // Set 0 is shared by the sky/cube and material pipelines.  The
        // additional material bindings are intentionally present on the
        // common layout so a model draw can consume the same UBO and skybox
        // while opting into the full GLES material set.
        VkDescriptorSetLayoutBinding bindings[13]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (uint32_t binding = 1; binding < 7; ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        bindings[7].binding = 7;
        bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[12].binding = 12;
        bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[12].descriptorCount = 1;
        bindings[12].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        for (uint32_t binding = 8; binding < 12; ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 13;
        layoutInfo.pBindings = bindings;
        VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_descriptor_set_layout", result);
            return false;
        }

        VkDescriptorSetLayoutBinding overlayBindings[2]{};
        overlayBindings[0].binding = 0;
        overlayBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        overlayBindings[0].descriptorCount = 1;
        overlayBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        overlayBindings[1].binding = 1;
        overlayBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        overlayBindings[1].descriptorCount = 1;
        overlayBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo overlayLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        overlayLayoutInfo.bindingCount = 2;
        overlayLayoutInfo.pBindings = overlayBindings;
        result = vkCreateDescriptorSetLayout(device_, &overlayLayoutInfo, nullptr, &overlayDescriptorSetLayout_);
        if (result != VK_SUCCESS) {
            LogVkError("create_overlay_descriptor_set_layout", result);
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            descriptorSetLayout_ = VK_NULL_HANDLE;
            return false;
        }
        VkDescriptorSetLayoutBinding postBindings[2]{};
        postBindings[0].binding = 0;
        postBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        postBindings[0].descriptorCount = 1;
        postBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        postBindings[1].binding = 1;
        postBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        postBindings[1].descriptorCount = 1;
        postBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo postLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        postLayoutInfo.bindingCount = 2;
        postLayoutInfo.pBindings = postBindings;
        result = vkCreateDescriptorSetLayout(device_, &postLayoutInfo, nullptr,
            &postDescriptorSetLayout_);
        if (result != VK_SUCCESS) {
            LogVkError("create_post_descriptor_set_layout", result);
            vkDestroyDescriptorSetLayout(device_, overlayDescriptorSetLayout_, nullptr);
            overlayDescriptorSetLayout_ = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            descriptorSetLayout_ = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool CreateDescriptorPool()
    {
        // Sets are allocated as: one per swapchain image (each binds its own
        // UBO) plus one per (swapchain image x model material), so the pool
        // has to cover both.  Headroom is kept for swapchain recreation.
        constexpr uint32_t kMaxDescriptorSets = 160;
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = kMaxDescriptorSets * 2;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // Scene sets use three samplers today; the extra headroom covers the
        // SDF text set and keeps the pool valid if a material grows another
        // optional texture binding later.
        poolSizes[1].descriptorCount = kMaxDescriptorSets * 12;
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = kMaxDescriptorSets;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        const VkResult result = vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_descriptor_pool", result);
            return false;
        }
        return true;
    }

    // Allocate one descriptor set per swapchain image, each pointing at its
    // own uniform buffer plus the shared samplers.  Called from
    // CreateFramebuffersAndCommands (after the scene pool exists and the
    // image count is known).
    bool CreateImageDescriptorSets()
    {
        if (descriptorSets_.empty()) {
            return true;
        }
        std::vector<VkDescriptorSetAllocateInfo> allocateInfos(descriptorSets_.size());
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(descriptorSets_.size() * 3);
        std::vector<VkDescriptorBufferInfo> bufferInfos(descriptorSets_.size());
        VkDescriptorImageInfo cubeImageInfo{};
        cubeImageInfo.sampler = sampler_;
        cubeImageInfo.imageView = cubeTexture_.view;
        cubeImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo skyboxImageInfo{};
        skyboxImageInfo.sampler = sampler_;
        skyboxImageInfo.imageView = skyboxTexture_.view;
        skyboxImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (size_t index = 0; index < descriptorSets_.size(); ++index) {
            allocateInfos[index].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfos[index].descriptorPool = descriptorPool_;
            allocateInfos[index].descriptorSetCount = 1;
            allocateInfos[index].pSetLayouts = &descriptorSetLayout_;
            const VkResult result = vkAllocateDescriptorSets(device_, &allocateInfos[index],
                &descriptorSets_[index]);
            if (result != VK_SUCCESS) {
                LogVkError("allocate_scene_descriptor_set", result);
                return false;
            }
            bufferInfos[index].buffer = uniformBuffers_[index].buffer;
            bufferInfos[index].range = sizeof(SceneUniforms);
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = descriptorSets_[index];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &bufferInfos[index];
            writes.push_back(write);
            write.dstBinding = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pBufferInfo = nullptr;
            write.pImageInfo = &cubeImageInfo;
            writes.push_back(write);
            write.dstBinding = 2;
            write.pImageInfo = &skyboxImageInfo;
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return true;
    }

    // One descriptor set per (swapchain image x material) for each model
    // asset.  The layout is shared with the built-in scene, so the model gets
    // the same UBO and skybox as the GLES lighting contract plus the complete
    // glTF material texture set.  Keeping the UBO binding per image means
    // re-recording a frame never touches a buffer the GPU might still be
    // reading.
    bool CreateModelDescriptorSets()
    {
        const std::array<ModelGpuAsset*, 4> assets = {
            &playerAsset_, &enemyAsset_, &helmetAsset_, &groundAsset_};
        size_t totalSets = 0;
        for (const ModelGpuAsset* asset : assets) {
            if (asset->loaded) {
                const size_t materialCount = asset->materials.empty() ? 1 : asset->materials.size();
                totalSets += uniformBuffers_.size() * materialCount;
            }
        }
        if (totalSets == 0 || uniformBuffers_.empty()) {
            return true;
        }

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(totalSets * 12);
        std::vector<VkDescriptorBufferInfo> bufferInfos(totalSets);
        std::vector<VkDescriptorBufferInfo> jointBufferInfos(totalSets);
        std::vector<VkDescriptorBufferInfo> alternateJointBufferInfos(totalSets);
        std::vector<VkDescriptorImageInfo> baseColorInfos(totalSets);
        std::vector<VkDescriptorImageInfo> skyboxInfos(totalSets);
        std::vector<VkDescriptorImageInfo> metallicRoughnessInfos(totalSets);
        std::vector<VkDescriptorImageInfo> normalInfos(totalSets);
        std::vector<VkDescriptorImageInfo> aoInfos(totalSets);
        std::vector<VkDescriptorImageInfo> emissiveInfos(totalSets);
        std::vector<VkDescriptorImageInfo> irradianceInfos(totalSets);
        std::vector<VkDescriptorImageInfo> prefilteredInfos(totalSets);
        std::vector<VkDescriptorImageInfo> brdfLutInfos(totalSets);
        std::vector<VkDescriptorImageInfo> shadowInfos(totalSets);
        VkDescriptorImageInfo skyboxImageInfo{};
        skyboxImageInfo.sampler = sampler_;
        skyboxImageInfo.imageView = skyboxTexture_.view;
        skyboxImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const auto resolveTextureView = [](const ModelGpuAsset& asset, int32_t imageIndex,
            const TextureResource& fallback) {
            if (imageIndex >= 0 && static_cast<size_t>(imageIndex) < asset.textures.size() &&
                asset.textures[imageIndex].view != VK_NULL_HANDLE) {
                return asset.textures[imageIndex].view;
            }
            return fallback.view;
        };
        const auto setImageInfo = [this](VkDescriptorImageInfo& info, VkImageView view) {
            info.sampler = sampler_;
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };

        size_t slot = 0;
        const ModelMaterialResource defaultMaterial{};
        for (ModelGpuAsset* asset : assets) {
            if (!asset->loaded) {
                asset->materialSets.clear();
                continue;
            }
            const size_t materialCount = asset->materials.empty() ? 1 : asset->materials.size();
            asset->materialSets.assign(uniformBuffers_.size() * materialCount, VK_NULL_HANDLE);
            for (size_t image = 0; image < uniformBuffers_.size(); ++image) {
                for (size_t material = 0; material < materialCount; ++material) {
                VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                allocateInfo.descriptorPool = descriptorPool_;
                allocateInfo.descriptorSetCount = 1;
                allocateInfo.pSetLayouts = &descriptorSetLayout_;
                const VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo,
                    &asset->materialSets[image * materialCount + material]);
                if (result != VK_SUCCESS) {
                    LogVkError("allocate_model_material_descriptor_set", result);
                    for (ModelGpuAsset* clearAsset : assets) {
                        clearAsset->materialSets.clear();
                    }
                    return false;
                }
                bufferInfos[slot].buffer = uniformBuffers_[image].buffer;
                bufferInfos[slot].range = sizeof(SceneUniforms);
                const BufferResource& jointBuffer = asset->jointBuffer.buffer != VK_NULL_HANDLE
                    ? asset->jointBuffer : identityJointBuffer_;
                jointBufferInfos[slot].buffer = jointBuffer.buffer;
                jointBufferInfos[slot].range = sizeof(Mat4) * kMaxJoints;
                const ModelGpuAsset* alternateAsset = nullptr;
                if (asset == &playerAsset_) {
                    alternateAsset = &enemyAsset_;
                } else if (asset == &enemyAsset_) {
                    alternateAsset = &playerAsset_;
                }
                const BufferResource& alternateJointBuffer = alternateAsset != nullptr &&
                    alternateAsset->jointBuffer.buffer != VK_NULL_HANDLE
                    ? alternateAsset->jointBuffer : identityJointBuffer_;
                alternateJointBufferInfos[slot].buffer = alternateJointBuffer.buffer;
                alternateJointBufferInfos[slot].range = sizeof(Mat4) * kMaxJoints;
                const ModelMaterialResource& materialInfo = asset->materials.empty() ? defaultMaterial
                    : asset->materials[material];
                setImageInfo(baseColorInfos[slot], resolveTextureView(*asset,
                    materialInfo.baseColorTexture, whiteTexture_));
                skyboxInfos[slot] = skyboxImageInfo;
                setImageInfo(metallicRoughnessInfos[slot], resolveTextureView(*asset,
                    materialInfo.metallicRoughnessTexture, neutralMrTexture_));
                setImageInfo(normalInfos[slot], resolveTextureView(*asset,
                    materialInfo.normalTexture, neutralNormalTexture_));
                setImageInfo(aoInfos[slot], resolveTextureView(*asset,
                    materialInfo.aoTexture, neutralAoTexture_));
                setImageInfo(emissiveInfos[slot], resolveTextureView(*asset,
                    materialInfo.emissiveTexture, blackTexture_));
                setImageInfo(irradianceInfos[slot], irradianceTexture_.view != VK_NULL_HANDLE
                    ? irradianceTexture_.view : skyboxTexture_.view);
                setImageInfo(prefilteredInfos[slot], prefilteredTexture_.view != VK_NULL_HANDLE
                    ? prefilteredTexture_.view : skyboxTexture_.view);
                setImageInfo(brdfLutInfos[slot], brdfLutTexture_.view != VK_NULL_HANDLE
                    ? brdfLutTexture_.view : whiteTexture_.view);
                shadowInfos[slot].sampler = sampler_;
                shadowInfos[slot].imageView = shadowMap_.view != VK_NULL_HANDLE
                    ? shadowMap_.view : whiteTexture_.view;
                shadowInfos[slot].imageLayout = shadowMap_.view != VK_NULL_HANDLE
                    ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = asset->materialSets[image * materialCount + material];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &bufferInfos[slot];
                writes.push_back(write);
                write.dstBinding = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pBufferInfo = nullptr;
                write.pImageInfo = &baseColorInfos[slot];
                writes.push_back(write);
                write.dstBinding = 2;
                write.pImageInfo = &skyboxInfos[slot];
                writes.push_back(write);
                write.dstBinding = 3;
                write.pImageInfo = &metallicRoughnessInfos[slot];
                writes.push_back(write);
                write.dstBinding = 4;
                write.pImageInfo = &normalInfos[slot];
                writes.push_back(write);
                write.dstBinding = 5;
                write.pImageInfo = &aoInfos[slot];
                writes.push_back(write);
                write.dstBinding = 6;
                write.pImageInfo = &emissiveInfos[slot];
                writes.push_back(write);
                write.dstBinding = 7;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pImageInfo = nullptr;
                write.pBufferInfo = &jointBufferInfos[slot];
                writes.push_back(write);
                write.dstBinding = 12;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &alternateJointBufferInfos[slot];
                writes.push_back(write);
                write.dstBinding = 8;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pBufferInfo = nullptr;
                write.pImageInfo = &irradianceInfos[slot];
                writes.push_back(write);
                write.dstBinding = 9;
                write.pImageInfo = &prefilteredInfos[slot];
                writes.push_back(write);
                write.dstBinding = 10;
                write.pImageInfo = &brdfLutInfos[slot];
                writes.push_back(write);
                write.dstBinding = 11;
                write.pImageInfo = &shadowInfos[slot];
                writes.push_back(write);
                ++slot;
                }
            }
        }
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        SDL_Log("SDL3_MODEL stage=material_sets_ready sets=%u images=%u",
            static_cast<unsigned int>(slot), static_cast<unsigned int>(uniformBuffers_.size()));
        return true;
    }

    bool CreateOverlayDescriptorSet()
    {
        // Binding 0 is the SDF atlas and binding 1 is one action icon.  Keep
        // one descriptor set per icon so a draw can select the four PNGs
        // without rebuilding a texture atlas at runtime.
        std::array<VkDescriptorSetLayout, rhi::CameraInput::kActionButtonCount + 1> layouts{};
        layouts.fill(overlayDescriptorSetLayout_);
        std::array<VkDescriptorSet, rhi::CameraInput::kActionButtonCount + 1> sets{};
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = descriptorPool_;
        allocateInfo.descriptorSetCount = static_cast<uint32_t>(sets.size());
        allocateInfo.pSetLayouts = layouts.data();
        const VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo, sets.data());
        if (result != VK_SUCCESS) {
            LogVkError("allocate_overlay_descriptor_sets", result);
            overlayDescriptorSet_ = VK_NULL_HANDLE;
            return false;
        }
        overlayDescriptorSet_ = textAtlasTexture_.view != VK_NULL_HANDLE ? sets[0] : VK_NULL_HANDLE;
        for (int index = 0; index < rhi::CameraInput::kActionButtonCount; ++index) {
            actionIconDescriptorSets_[index] = sets[static_cast<size_t>(index) + 1];
        }
        const VkImageView textView = textAtlasTexture_.view != VK_NULL_HANDLE
            ? textAtlasTexture_.view : whiteTexture_.view;
        for (size_t setIndex = 0; setIndex < sets.size(); ++setIndex) {
            const int iconIndex = static_cast<int>(setIndex) - 1;
            const VkImageView iconView = iconIndex >= 0 &&
                iconIndex < rhi::CameraInput::kActionButtonCount &&
                actionIconTextures_[iconIndex].view != VK_NULL_HANDLE
                ? actionIconTextures_[iconIndex].view : whiteTexture_.view;
            VkDescriptorImageInfo imageInfos[2]{};
            imageInfos[0].sampler = sampler_;
            imageInfos[0].imageView = textView;
            imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[1].sampler = sampler_;
            imageInfos[1].imageView = iconView;
            imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t binding = 0; binding < 2; ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = sets[setIndex];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].pImageInfo = &imageInfos[binding];
            }
            vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        }
        return true;
    }

    bool CreatePostDescriptorSets()
    {
        if (sceneColorTarget_.view == VK_NULL_HANDLE || bloomUp_[0].view == VK_NULL_HANDLE ||
            uniformBuffers_.empty()) {
            return false;
        }
        postDescriptorSets_.assign(uniformBuffers_.size(), VK_NULL_HANDLE);
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(postDescriptorSets_.size() * 2);
        std::vector<VkDescriptorImageInfo> imageInfos(postDescriptorSets_.size());
        std::vector<VkDescriptorImageInfo> bloomInfos(postDescriptorSets_.size());
        for (size_t index = 0; index < postDescriptorSets_.size(); ++index) {
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = descriptorPool_;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &postDescriptorSetLayout_;
            const VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo,
                &postDescriptorSets_[index]);
            if (result != VK_SUCCESS) {
                LogVkError("allocate_post_descriptor_set", result);
                postDescriptorSets_.clear();
                return false;
            }
            imageInfos[index].sampler = sampler_;
            imageInfos[index].imageView = sceneColorTarget_.view;
            imageInfos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bloomInfos[index].sampler = sampler_;
            bloomInfos[index].imageView = bloomUp_[0].view;
            bloomInfos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = postDescriptorSets_[index];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfos[index];
            writes.push_back(write);
            write.dstBinding = 1;
            write.pImageInfo = &bloomInfos[index];
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return true;
    }

    bool CreateBloomDescriptorSets()
    {
        if (sceneColorTarget_.view == VK_NULL_HANDLE || bloomDs_[0].view == VK_NULL_HANDLE ||
            bloomDs_[kBloomDsLevels - 1].view == VK_NULL_HANDLE || bloomUp_[0].view == VK_NULL_HANDLE) {
            return false;
        }
        bloomThresholdDescriptorSet_ = VK_NULL_HANDLE;
        bloomDownDescriptorSets_.fill(VK_NULL_HANDLE);
        bloomUpDescriptorSets_.fill(VK_NULL_HANDLE);

        const auto allocateSet = [this](VkDescriptorSet& set, const TextureResource& input,
            const TextureResource* previous) {
            VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocateInfo.descriptorPool = descriptorPool_;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &postDescriptorSetLayout_;
            if (vkAllocateDescriptorSets(device_, &allocateInfo, &set) != VK_SUCCESS) {
                LogVkError("allocate_bloom_descriptor_set", VK_ERROR_OUT_OF_POOL_MEMORY);
                set = VK_NULL_HANDLE;
                return false;
            }
            VkDescriptorImageInfo inputInfo{};
            inputInfo.sampler = sampler_;
            inputInfo.imageView = input.view;
            inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo previousInfo = inputInfo;
            if (previous != nullptr) {
                previousInfo.imageView = previous->view;
            }
            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = set;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &inputInfo;
            writes[1] = writes[0];
            writes[1].dstBinding = 1;
            writes[1].pImageInfo = &previousInfo;
            vkUpdateDescriptorSets(device_, previous != nullptr ? 2U : 1U, writes, 0, nullptr);
            return true;
        };

        if (!allocateSet(bloomThresholdDescriptorSet_, sceneColorTarget_, nullptr)) {
            return false;
        }
        for (int level = 1; level < kBloomDsLevels; ++level) {
            if (!allocateSet(bloomDownDescriptorSets_[level - 1], bloomDs_[level - 1], nullptr)) {
                return false;
            }
        }
        for (int level = 0; level < kBloomUpLevels; ++level) {
            const TextureResource* previous = level == kBloomUpLevels - 1
                ? &bloomDs_[kBloomDsLevels - 1] : &bloomUp_[level + 1];
            if (!allocateSet(bloomUpDescriptorSets_[level], bloomDs_[level], previous)) {
                return false;
            }
        }
        SDL_Log("SDL3_VULKAN stage=bloom_descriptors_ready down=%d up=%d",
            kBloomDsLevels, kBloomUpLevels);
        return true;
    }

    // Narrows the loader's 32 bit indices to the width chosen at load time.
    static std::vector<unsigned char> PackIndices(const ohos_model::Mesh& mesh)
    {
        const size_t bytes = mesh.indices.size() * mesh.indexBytes;
        std::vector<unsigned char> packed(bytes);
        if (mesh.indexBytes == 2) {
            std::vector<std::uint16_t> narrowed(mesh.indices.size());
            for (size_t i = 0; i < mesh.indices.size(); ++i) {
                narrowed[i] = static_cast<std::uint16_t>(mesh.indices[i]);
            }
            std::memcpy(packed.data(), narrowed.data(), bytes);
        } else {
            std::memcpy(packed.data(), mesh.indices.data(), bytes);
        }
        return packed;
    }

    // Kept for source compatibility with older callers while the live Vulkan
    // path now uploads raw JOINTS_0/WEIGHTS_0 and evaluates the skeleton in a
    // per-frame UBO.  CPU baking would make the character permanently static.
    static void BakeSkinnedBindPose(ohos_model::Model& model)
    {
        if (!model.skinned || model.nodes.empty() || model.skin.JointCount() == 0 ||
            model.skin.inverseBindMatrices.size() < model.skin.JointCount() * 16) {
            return;
        }
        // Use the first key of the same Idle_Loop clip that GLES selects for
        // the initial frame.  The loader's node defaults are a T-pose, which
        // was the most visible remaining scene mismatch after the coordinate
        // fix.
        std::vector<float> translations(model.nodes.size() * 3, 0.0f);
        std::vector<float> rotations(model.nodes.size() * 4, 0.0f);
        std::vector<float> scales(model.nodes.size() * 3, 1.0f);
        for (size_t index = 0; index < model.nodes.size(); ++index) {
            const ohos_model::Node& node = model.nodes[index];
            for (int component = 0; component < 3; ++component) {
                translations[index * 3 + component] = node.translation[component];
                scales[index * 3 + component] = node.scale[component];
            }
            for (int component = 0; component < 4; ++component) {
                rotations[index * 4 + component] = node.rotation[component];
            }
        }
        const ohos_model::AnimationClip* idleClip = nullptr;
        for (const ohos_model::AnimationClip& clip : model.animations) {
            if (clip.name == "Idle_Loop") {
                idleClip = &clip;
                break;
            }
        }
        if (idleClip != nullptr) {
            for (const ohos_model::AnimationChannel& channel : idleClip->channels) {
                if (channel.nodeIndex >= model.nodes.size() || channel.values.empty()) {
                    continue;
                }
                if (channel.path == 0 && channel.values.size() >= 3) {
                    std::memcpy(&translations[channel.nodeIndex * 3], channel.values.data(),
                        sizeof(float) * 3);
                } else if (channel.path == 1 && channel.values.size() >= 4) {
                    std::memcpy(&rotations[channel.nodeIndex * 4], channel.values.data(),
                        sizeof(float) * 4);
                } else if (channel.path == 2 && channel.values.size() >= 3) {
                    std::memcpy(&scales[channel.nodeIndex * 3], channel.values.data(),
                        sizeof(float) * 3);
                }
            }
        }

        std::vector<Mat4> globals(model.nodes.size());
        for (size_t index = 0; index < model.nodes.size(); ++index) {
            const ohos_model::Node& node = model.nodes[index];
            const Mat4 local = Mat4ComposeTrs(&translations[index * 3], &rotations[index * 4],
                &scales[index * 3]);
            if (node.parent >= 0 && static_cast<size_t>(node.parent) < index) {
                globals[index] = Mat4Multiply(globals[static_cast<size_t>(node.parent)], local);
            } else {
                globals[index] = local;
            }
        }

        const Mat4 normalise = Mat4FromArray(model.normaliseMatrix);
        std::vector<Mat4> jointMatrices(model.skin.JointCount(), Mat4Identity());
        for (size_t joint = 0; joint < jointMatrices.size(); ++joint) {
            const uint32_t nodeIndex = model.skin.jointNodes[joint];
            if (nodeIndex >= globals.size()) {
                continue;
            }
            const Mat4 inverseBind = Mat4FromArray(
                &model.skin.inverseBindMatrices[joint * 16]);
            jointMatrices[joint] = Mat4Multiply(normalise,
                Mat4Multiply(globals[nodeIndex], inverseBind));
        }

        for (ohos_model::Mesh& mesh : model.meshes) {
            for (ohos_model::Vertex& vertex : mesh.vertices) {
                float position[3] = {0.0f, 0.0f, 0.0f};
                float normal[3] = {0.0f, 0.0f, 0.0f};
                float weightSum = 0.0f;
                for (int slot = 0; slot < 4; ++slot) {
                    const float weight = static_cast<float>(vertex.weights[slot]) / 255.0f;
                    const uint32_t joint = vertex.joints[slot];
                    if (weight <= 0.0f || joint >= jointMatrices.size()) {
                        continue;
                    }
                    float transformedPosition[3]{};
                    float transformedNormal[3]{};
                    Mat4TransformPoint(jointMatrices[joint], vertex.position, transformedPosition);
                    Mat4TransformVector(jointMatrices[joint], vertex.normal, transformedNormal);
                    for (int component = 0; component < 3; ++component) {
                        position[component] += transformedPosition[component] * weight;
                        normal[component] += transformedNormal[component] * weight;
                    }
                    weightSum += weight;
                }
                if (weightSum <= 0.001f) {
                    Mat4TransformPoint(normalise, vertex.position, position);
                    Mat4TransformVector(normalise, vertex.normal, normal);
                }
                const float normalLength = std::sqrt(normal[0] * normal[0]
                    + normal[1] * normal[1] + normal[2] * normal[2]);
                if (normalLength > 1.0e-6f) {
                    normal[0] /= normalLength;
                    normal[1] /= normalLength;
                    normal[2] /= normalLength;
                }
                std::memcpy(vertex.position, position, sizeof(position));
                std::memcpy(vertex.normal, normal, sizeof(normal));
                std::memset(vertex.joints, 0, sizeof(vertex.joints));
                std::memset(vertex.weights, 0, sizeof(vertex.weights));
            }
        }
        model.skinned = false;
    }

    // Loads one model asset and uploads its meshes/textures.  Keeping the
    // player, helmet and ground in separate GPU assets lets command recording
    // apply the exact GLES per-object transforms while reusing one pipeline.
    bool LoadModelAsset(const char* path, ModelGpuAsset& asset, bool bakeSkinned)
    {
        ohos_model::Model model;
        std::string error;
        if (!ohos_model::LoadModelFromRawFile(path, model, error)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_MODEL stage=load_failed path=%s error=%s", path, error.c_str());
            return false;
        }
        asset.textures.reserve(model.images.size());
        for (const ohos_model::Image& image : model.images) {
            asset.textures.emplace_back();
            TextureResource& texture = asset.textures.back();
            if (image.rgba.empty() || image.width == 0 || image.height == 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_MODEL stage=image_empty path=%s uri=%s",
                    path, image.uri.c_str());
                continue;
            }
            const std::vector<const unsigned char*> layers = {image.rgba.data()};
            if (!CreateTextureResource(layers, image.width, image.height, false, texture)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL3_MODEL stage=texture_upload_failed path=%s uri=%s", path, image.uri.c_str());
            }
        }

        asset.materials.reserve(model.materials.size());
        for (const ohos_model::Material& material : model.materials) {
            ModelMaterialResource resource;
            resource.baseColorTexture = material.baseColorImage;
            resource.metallicRoughnessTexture = material.metallicRoughnessImage;
            resource.normalTexture = material.normalImage;
            resource.aoTexture = material.aoImage;
            resource.emissiveTexture = material.emissiveImage;
            std::memcpy(resource.baseColorFactor, material.baseColorFactor,
                sizeof(resource.baseColorFactor));
            resource.metallicFactor = material.metallicFactor;
            resource.roughnessFactor = material.roughnessFactor;
            resource.normalScale = material.normalScale;
            std::memcpy(resource.emissiveFactor, material.emissiveFactor,
                sizeof(resource.emissiveFactor));
            asset.materials.push_back(resource);
        }

        asset.meshes.reserve(model.meshes.size());
        for (const ohos_model::Mesh& mesh : model.meshes) {
            if (mesh.vertices.empty() || mesh.indices.empty()) {
                continue;
            }
            ModelMeshResource resource;
            resource.indexCount = static_cast<uint32_t>(mesh.indices.size());
            resource.materialIndex = mesh.materialIndex;
            resource.indexType = mesh.indexBytes == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            // Quaternius' joint shell is a debug material inside the body.  It
            // is part of the visible mesh in GLES, but must use the first real
            // body material instead of showing the purple weight-paint tint.
            if (bakeSkinned && resource.materialIndex < asset.materials.size() &&
                resource.materialIndex < model.materials.size()) {
                std::string materialName = model.materials[resource.materialIndex].name;
                for (char& character : materialName) {
                    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                }
                if (materialName.find("joint") != std::string::npos) {
                    for (size_t material = 0; material < model.materials.size(); ++material) {
                        std::string candidate = model.materials[material].name;
                        for (char& character : candidate) {
                            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                        }
                        if (candidate.find("joint") == std::string::npos) {
                            resource.materialIndex = static_cast<uint32_t>(material);
                            break;
                        }
                    }
                }
            }
            const float* baseColorFactor = nullptr;
            if (resource.materialIndex < asset.materials.size()) {
                baseColorFactor = asset.materials[resource.materialIndex].baseColorFactor;
            }
            const float fallbackColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (baseColorFactor == nullptr) {
                baseColorFactor = fallbackColor;
            }
            const ModelMaterialResource fallbackMaterial{};
            const ModelMaterialResource& materialInfo = resource.materialIndex < asset.materials.size() ?
                asset.materials[resource.materialIndex] : fallbackMaterial;
            std::vector<ModelVertexGpu> gpuVertices(mesh.vertices.size());
            for (size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex) {
                const ohos_model::Vertex& source = mesh.vertices[vertexIndex];
                ModelVertexGpu& destination = gpuVertices[vertexIndex];
                destination.x = source.position[0];
                destination.y = source.position[1];
                destination.z = source.position[2];
                destination.nx = source.normal[0];
                destination.ny = source.normal[1];
                destination.nz = source.normal[2];
                destination.u = source.uv[0];
                destination.v = source.uv[1];
                destination.r = baseColorFactor[0];
                destination.g = baseColorFactor[1];
                destination.b = baseColorFactor[2];
                destination.a = baseColorFactor[3];
                destination.metallicFactor = materialInfo.metallicFactor;
                destination.roughnessFactor = materialInfo.roughnessFactor;
                // Match GLES' hasNormalMap gate: the neutral fallback normal
                // must not perturb textureless Quaternius materials.
                destination.normalScale = materialInfo.normalTexture >= 0
                    ? materialInfo.normalScale : 0.0f;
                destination.emissiveR = materialInfo.emissiveFactor[0];
                destination.emissiveG = materialInfo.emissiveFactor[1];
                destination.emissiveB = materialInfo.emissiveFactor[2];
                destination.aoStrength = materialInfo.aoStrength;
                destination.materialPadding = 0.0f;
                for (int slot = 0; slot < 4; ++slot) {
                    destination.joints[slot] = source.joints[slot];
                    destination.weights[slot] = static_cast<float>(source.weights[slot]) / 255.0f;
                }
            }
            const VkDeviceSize vertexBytes =
                static_cast<VkDeviceSize>(gpuVertices.size()) * sizeof(ModelVertexGpu);
            if (!CreateBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    resource.vertexBuffer) ||
                !UploadToMappedBuffer(resource.vertexBuffer, gpuVertices.data(), vertexBytes)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL3_MODEL stage=vertex_upload_failed path=%s verts=%u", path,
                    static_cast<unsigned int>(mesh.vertices.size()));
                DestroyBuffer(resource.vertexBuffer);
                return false;
            }
            const std::vector<unsigned char> packed = PackIndices(mesh);
            if (packed.empty() || !CreateBuffer(static_cast<VkDeviceSize>(packed.size()),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, resource.indexBuffer) ||
                !UploadToMappedBuffer(resource.indexBuffer, packed.data(),
                    static_cast<VkDeviceSize>(packed.size()))) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL3_MODEL stage=index_upload_failed path=%s indices=%u", path,
                    static_cast<unsigned int>(mesh.indices.size()));
                DestroyBuffer(resource.vertexBuffer);
                DestroyBuffer(resource.indexBuffer);
                return false;
            }
            asset.meshes.push_back(std::move(resource));
        }
        asset.loaded = !asset.meshes.empty();
        if (bakeSkinned && model.skinned && !model.nodes.empty() && model.skin.JointCount() > 0 &&
            model.skin.inverseBindMatrices.size() >= model.skin.JointCount() * 16) {
            asset.skinned = true;
            asset.animationNodes = std::move(model.nodes);
            asset.animationSkin = std::move(model.skin);
            asset.animationClips = std::move(model.animations);
            asset.animationNormalise = Mat4FromArray(model.normaliseMatrix);
            asset.jointMatrices.assign(kMaxJoints, Mat4Identity());
            if (asset.animationSkin.JointCount() > kMaxJoints) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL3_MODEL stage=animation_joint_limit path=%s joints=%u limit=%u", path,
                    static_cast<unsigned int>(asset.animationSkin.JointCount()),
                    static_cast<unsigned int>(kMaxJoints));
            }
            if (!CreateBuffer(sizeof(Mat4) * kMaxJoints, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    asset.jointBuffer) ||
                !UploadToMappedBuffer(asset.jointBuffer, asset.jointMatrices.data(),
                    sizeof(Mat4) * kMaxJoints)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL3_MODEL stage=joint_buffer_failed path=%s", path);
                DestroyBuffer(asset.jointBuffer);
                asset.skinned = false;
            } else {
                const auto findClip = [&asset](const char* name) {
                    for (size_t index = 0; index < asset.animationClips.size(); ++index) {
                        if (asset.animationClips[index].name == name) {
                            return static_cast<int>(index);
                        }
                    }
                    return -1;
                };
                asset.idleClip = findClip("Idle_Loop");
                asset.walkClip = findClip("Walk_Loop");
                asset.sprintClip = findClip("Sprint_Loop");
                asset.crouchClip = findClip("Crouch_Idle_Loop");
                asset.attackClip = findClip("Punch_Jab");
                asset.jumpStartClip = findClip("Jump_Start");
                asset.jumpLoopClip = findClip("Jump_Loop");
                asset.jumpLandClip = findClip("Jump_Land");
                asset.deathClip = findClip("Death01");
                // Mikan third-person prototype: Hit_Chest one-shot reaction.
                asset.hitClip = findClip("Hit_Chest");
                asset.animationClipIndex = asset.idleClip >= 0 ? asset.idleClip :
                    (asset.walkClip >= 0 ? asset.walkClip :
                        (asset.animationClips.empty() ? -1 : 0));
                asset.animationState = ModelAnimationState::Idle;
                asset.animationStateTime = 0.0f;
                asset.animationLoop = true;
                asset.animationLastTicks = SDL_GetTicks();
                SDL_Log("SDL3_MODEL stage=animation_gpu_ready path=%s joints=%u clips=%u idle=%d walk=%d sprint=%d crouch=%d attack=%d jump_start=%d jump_loop=%d jump_land=%d",
                    path, static_cast<unsigned int>(asset.animationSkin.JointCount()),
                    static_cast<unsigned int>(asset.animationClips.size()), asset.idleClip,
                    asset.walkClip, asset.sprintClip, asset.crouchClip, asset.attackClip,
                    asset.jumpStartClip, asset.jumpLoopClip, asset.jumpLandClip);
            }
        }
        SDL_Log("SDL3_MODEL stage=gpu_ready path=%s meshes=%u textures=%u materials=%u loaded=%d",
            path, static_cast<unsigned int>(asset.meshes.size()),
            static_cast<unsigned int>(asset.textures.size()),
            static_cast<unsigned int>(asset.materials.size()), asset.loaded ? 1 : 0);
        return asset.loaded;
    }

    void LoadModels()
    {
        (void)LoadModelAsset(kModelPath, playerAsset_, true);
        (void)LoadModelAsset(kModelPath, enemyAsset_, true);
        (void)LoadModelAsset(kHelmetPath, helmetAsset_, false);
        (void)LoadModelAsset(kCubePath, groundAsset_, false);
    }

    bool CreateSceneResources()
    {
        if (!CreateCheckerTexture() || !CreateWhiteTexture() || !CreateMaterialFallbackTextures() ||
            !CreateSkyboxTexture() || !CreateSampler() ||
            !CreateCubeVertexBuffer() ||
            !CreateDescriptorSetLayout() || !CreateDescriptorPool()) {
            DestroySceneResources();
            return false;
        }
        if (!CreateVulkanGpuIblResources()) {
            // Keep boot resilient on devices that expose Vulkan but cannot
            // render a sampled cubemap attachment.  Descriptor creation later
            // falls back to the raw skybox; the caller can select GLES when
            // this diagnostic is present.
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=ibl_gpu_unavailable");
        }
        if (!CreateTextAtlas()) {
            SDL_Log("SDL3_VULKAN stage=sdf_text_unavailable reason=atlas_load_failed");
        }
        if (!CreateActionIconTextures()) {
            SDL_Log("SDL3_VULKAN stage=touch_icons_partial");
        }
        // Upload the cube vertex data once.  Positions stay in model space
        // (w = 1); the vertex shader applies the per-frame UBO matrix.
        {
            std::array<CubeVertex, kCubeVertexCount> vertices{};
            for (uint32_t index = 0; index < kCubeVertexCount; ++index) {
                const CubeSourceVertex& source = kCubeSourceVertices[index];
                CubeVertex& destination = vertices[index];
                destination.x = source.x;
                destination.y = source.y;
                destination.z = source.z;
                destination.w = 1.0f;
                destination.r = source.r;
                destination.g = source.g;
                destination.b = source.b;
                destination.u = source.u;
                destination.v = source.v;
            }
            if (!UploadToMappedBuffer(cubeVertexBuffer_, vertices.data(), sizeof(vertices))) {
                DestroySceneResources();
                return false;
            }
        }
        if (!CreateIdentityJointBuffer()) {
            DestroySceneResources();
            return false;
        }
        // Best effort: a failure here logs and keeps the built-in scene.
        LoadModels();
        if (!CreateModelInstanceBuffer()) {
            DestroySceneResources();
            return false;
        }
        sceneReady_ = true;
        return true;
    }

    bool CreateShaderModule(const uint32_t* code, size_t wordCount, VkShaderModule* module)
    {
        VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = wordCount * sizeof(uint32_t);
        createInfo.pCode = code;
        const VkResult result = vkCreateShaderModule(device_, &createInfo, nullptr, module);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_shader_module", result);
            return false;
        }
        return true;
    }

    VkPipeline CreatePipeline(VkShaderModule vertexShader, VkShaderModule fragmentShader,
        VkCullModeFlags cullMode, VkBool32 depthTestEnable, VkBool32 depthWriteEnable,
        VertexLayout vertexLayout, VkBool32 blendEnable = VK_FALSE,
        VkRenderPass targetRenderPass = VK_NULL_HANDLE,
        VkPipelineLayout layoutOverride = VK_NULL_HANDLE,
        VkBool32 hasColorAttachment = VK_TRUE,
        VkBool32 depthBiasEnable = VK_FALSE)
    {
        VkPipelineShaderStageCreateInfo vertexStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexShader;
        vertexStage.pName = "main";
        VkPipelineShaderStageCreateInfo fragmentStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentShader;
        fragmentStage.pName = "main";
        VkPipelineShaderStageCreateInfo stages[] = {vertexStage, fragmentStage};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkVertexInputBindingDescription vertexBindings[2]{};
        VkVertexInputBindingDescription& vertexBinding = vertexBindings[0];
        VkVertexInputAttributeDescription vertexAttributes[14]{};
        uint32_t vertexAttributeCount = 0;
        uint32_t vertexBindingCount = 1;
        if (vertexLayout != VertexLayout::kNone) {
            vertexBinding.binding = 0;
            vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            switch (vertexLayout) {
            case VertexLayout::kOverlay:
                vertexBinding.stride = sizeof(OverlayVertex);
                vertexAttributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, x)};
                vertexAttributes[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayVertex, r)};
                vertexAttributeCount = 2;
                break;
            case VertexLayout::kIcon:
                vertexBinding.stride = sizeof(IconVertex);
                vertexAttributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(IconVertex, x)};
                vertexAttributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(IconVertex, u)};
                vertexAttributes[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(IconVertex, r)};
                vertexAttributeCount = 3;
                break;
            case VertexLayout::kText:
                vertexBinding.stride = sizeof(TextVertex);
                vertexAttributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVertex, x)};
                vertexAttributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TextVertex, u)};
                vertexAttributes[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TextVertex, r)};
                vertexAttributes[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(TextVertex, screenPxRange)};
                vertexAttributeCount = 4;
                break;
            case VertexLayout::kCube:
            case VertexLayout::kModel: {
                const bool model = vertexLayout == VertexLayout::kModel;
                vertexBinding.stride = model ? sizeof(ModelVertexGpu) : sizeof(CubeVertex);
                // The cube packs its position as vec4 with w = 1; glTF positions
                // are vec3.
                vertexAttributes[0] = {0, 0,
                    model ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT,
                    static_cast<uint32_t>(model ? offsetof(ModelVertexGpu, x) : offsetof(CubeVertex, x))};
                vertexAttributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                    static_cast<uint32_t>(model ? offsetof(ModelVertexGpu, nx) : offsetof(CubeVertex, r))};
                vertexAttributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,
                    static_cast<uint32_t>(model ? offsetof(ModelVertexGpu, u) : offsetof(CubeVertex, u))};
                if (model) {
                    vertexAttributes[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(ModelVertexGpu, r)};
                    vertexAttributes[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(ModelVertexGpu, metallicFactor)};
                    vertexAttributes[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(ModelVertexGpu, emissiveR)};
                    vertexAttributes[6] = {6, 0, VK_FORMAT_R32G32B32A32_UINT,
                        offsetof(ModelVertexGpu, joints)};
                    vertexAttributes[7] = {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(ModelVertexGpu, weights)};
                    vertexBindings[1].binding = 1;
                    vertexBindings[1].stride = sizeof(CharacterInstanceData);
                    vertexBindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                    for (uint32_t column = 0; column < 4; ++column) {
                        vertexAttributes[8 + column] = {
                            8 + column, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                            static_cast<uint32_t>(offsetof(CharacterInstanceData, model) +
                                sizeof(float) * 4 * column)};
                    }
                    vertexAttributes[12] = {12, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(CharacterInstanceData, color)};
                    vertexAttributes[13] = {13, 1, VK_FORMAT_R32_UINT,
                        offsetof(CharacterInstanceData, skinIndex)};
                }
                vertexAttributeCount = model ? 14 : 3;
                vertexBindingCount = model ? 2 : 1;
                break;
            }
            default:
                break;
            }
            vertexInput.vertexBindingDescriptionCount = vertexBindingCount;
            vertexInput.pVertexBindingDescriptions = vertexBindings;
            vertexInput.vertexAttributeDescriptionCount = vertexAttributeCount;
            vertexInput.pVertexAttributeDescriptions = vertexAttributes;
        }
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = cullMode;
        // The positive-height Vulkan viewport maps the projected Y axis in
        // the opposite screen direction to GLES.  Treat clockwise triangles
        // as front-facing so VK_CULL_MODE_BACK_BIT removes the same back
        // faces as GL's CCW + GL_BACK configuration.
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = depthBiasEnable;
        rasterizer.depthBiasConstantFactor = depthBiasEnable ? 1.25f : 0.0f;
        rasterizer.depthBiasSlopeFactor = depthBiasEnable ? 2.0f : 0.0f;
        rasterizer.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = depthTestEnable;
        depthStencil.depthWriteEnable = depthWriteEnable;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.blendEnable = blendEnable;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo colorBlending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlending.attachmentCount = hasColorAttachment ? 1 : 0;
        colorBlending.pAttachments = hasColorAttachment ? &colorBlendAttachment : nullptr;
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = layoutOverride != VK_NULL_HANDLE ? layoutOverride : pipelineLayout_;
        pipelineInfo.renderPass = targetRenderPass != VK_NULL_HANDLE ? targetRenderPass : renderPass_;
        pipelineInfo.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
            nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_graphics_pipeline", result);
            return VK_NULL_HANDLE;
        }
        return pipeline;
    }

    bool CreateGraphicsPipeline()
    {
        VkShaderModule cubeVertex = VK_NULL_HANDLE;
        VkShaderModule cubeFragment = VK_NULL_HANDLE;
        VkShaderModule skyboxVertex = VK_NULL_HANDLE;
        VkShaderModule skyboxFragment = VK_NULL_HANDLE;
        VkShaderModule modelVertex = VK_NULL_HANDLE;
        VkShaderModule modelFragment = VK_NULL_HANDLE;
        VkShaderModule shadowVertex = VK_NULL_HANDLE;
        VkShaderModule shadowFragment = VK_NULL_HANDLE;
        VkShaderModule overlayVertex = VK_NULL_HANDLE;
        VkShaderModule overlayFragment = VK_NULL_HANDLE;
        VkShaderModule iconVertex = VK_NULL_HANDLE;
        VkShaderModule iconFragment = VK_NULL_HANDLE;
        VkShaderModule textVertex = VK_NULL_HANDLE;
        VkShaderModule textFragment = VK_NULL_HANDLE;
        VkShaderModule postVertex = VK_NULL_HANDLE;
        VkShaderModule postFragment = VK_NULL_HANDLE;
        VkShaderModule bloomThresholdFragment = VK_NULL_HANDLE;
        VkShaderModule bloomDownFragment = VK_NULL_HANDLE;
        VkShaderModule bloomUpFragment = VK_NULL_HANDLE;
        if (!CreateShaderModule(kSceneCubeVertSpv, kSceneCubeVertSpvWordCount, &cubeVertex) ||
            !CreateShaderModule(kSceneCubeFragSpv, kSceneCubeFragSpvWordCount, &cubeFragment) ||
            !CreateShaderModule(kSceneSkyboxVertSpv, kSceneSkyboxVertSpvWordCount, &skyboxVertex) ||
            !CreateShaderModule(kSceneSkyboxFragSpv, kSceneSkyboxFragSpvWordCount, &skyboxFragment) ||
            !CreateShaderModule(kSceneModelVertSpv, kSceneModelVertSpvWordCount, &modelVertex) ||
            !CreateShaderModule(kSceneModelFragSpv, kSceneModelFragSpvWordCount, &modelFragment) ||
            !CreateShaderModule(kSceneShadowVertSpv, kSceneShadowVertSpvWordCount, &shadowVertex) ||
            !CreateShaderModule(kSceneShadowFragSpv, kSceneShadowFragSpvWordCount, &shadowFragment) ||
            !CreateShaderModule(kSceneOverlayVertSpv, kSceneOverlayVertSpvWordCount, &overlayVertex) ||
            !CreateShaderModule(kSceneOverlayFragSpv, kSceneOverlayFragSpvWordCount, &overlayFragment) ||
            !CreateShaderModule(kSceneIconVertSpv, kSceneIconVertSpvWordCount, &iconVertex) ||
            !CreateShaderModule(kSceneIconFragSpv, kSceneIconFragSpvWordCount, &iconFragment) ||
            !CreateShaderModule(kSceneTextVertSpv, kSceneTextVertSpvWordCount, &textVertex) ||
            !CreateShaderModule(kSceneTextFragSpv, kSceneTextFragSpvWordCount, &textFragment) ||
            !CreateShaderModule(kScenePostVertSpv, kScenePostVertSpvWordCount, &postVertex) ||
            !CreateShaderModule(kScenePostFragSpv, kScenePostFragSpvWordCount, &postFragment) ||
            !CreateShaderModule(kSceneBloomThresholdFragSpv, kSceneBloomThresholdFragSpvWordCount,
                &bloomThresholdFragment) ||
            !CreateShaderModule(kSceneBloomDownFragSpv, kSceneBloomDownFragSpvWordCount,
                &bloomDownFragment) ||
            !CreateShaderModule(kSceneBloomUpFragSpv, kSceneBloomUpFragSpvWordCount, &bloomUpFragment)) {
            if (cubeVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, cubeVertex, nullptr);
            if (cubeFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, cubeFragment, nullptr);
            if (skyboxVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, skyboxVertex, nullptr);
            if (skyboxFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, skyboxFragment, nullptr);
            if (modelVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, modelVertex, nullptr);
            if (modelFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, modelFragment, nullptr);
            if (shadowVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shadowVertex, nullptr);
            if (shadowFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, shadowFragment, nullptr);
            if (overlayVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, overlayVertex, nullptr);
            if (overlayFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, overlayFragment, nullptr);
            if (iconVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, iconVertex, nullptr);
            if (iconFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, iconFragment, nullptr);
            if (textVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, textVertex, nullptr);
            if (textFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, textFragment, nullptr);
            if (postVertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, postVertex, nullptr);
            if (postFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, postFragment, nullptr);
            if (bloomThresholdFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, bloomThresholdFragment, nullptr);
            if (bloomDownFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, bloomDownFragment, nullptr);
            if (bloomUpFragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, bloomUpFragment, nullptr);
            return false;
        }
        VkDescriptorSetLayout pipelineSetLayouts[] = {
            descriptorSetLayout_, overlayDescriptorSetLayout_, postDescriptorSetLayout_};
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        // The model vertex shader consumes two mat4 transforms; UI shaders
        // only read the first 16 bytes of the same range.
        pushConstantRange.size = sizeof(ModelPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 3;
        layoutInfo.pSetLayouts = pipelineSetLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;
        VkResult result = vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_pipeline_layout", result);
        } else {
            // The sample draws the skybox first and the cube second.  The
            // cube consumes the real vertex buffer and depth-tests against
            // the skybox so that its own faces occlude each other.  Back-face
            // culling stays off because the CPU clip-space transform emits
            // faces whose winding depends on the current rotation, and the
            // depth buffer already resolves the visible surface.
            pipeline_ = CreatePipeline(cubeVertex, cubeFragment, VK_CULL_MODE_NONE, VK_TRUE, VK_TRUE,
                VertexLayout::kCube);
            // The skybox is a full-screen triangle with no vertex input and
            // is drawn without depth writes so it acts as the background.
            skyboxPipeline_ = CreatePipeline(skyboxVertex, skyboxFragment, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE,
                VertexLayout::kNone);
            // Match GLES model rendering: closed meshes render only their
            // front faces, while the shared frontFace state accounts for the
            // Vulkan positive-height viewport.
            modelPipeline_ = CreatePipeline(modelVertex, modelFragment, VK_CULL_MODE_BACK_BIT, VK_TRUE, VK_TRUE,
                VertexLayout::kModel);
            shadowPipeline_ = CreatePipeline(shadowVertex, shadowFragment, VK_CULL_MODE_BACK_BIT, VK_TRUE, VK_TRUE,
                VertexLayout::kModel, VK_FALSE, shadowRenderPass_, pipelineLayout_, VK_FALSE, VK_TRUE);
            overlayPipeline_ = CreatePipeline(overlayVertex, overlayFragment, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE,
                VertexLayout::kOverlay, VK_TRUE);
            iconPipeline_ = CreatePipeline(iconVertex, iconFragment, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE,
                VertexLayout::kIcon, VK_TRUE);
            textPipeline_ = CreatePipeline(textVertex, textFragment, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE,
                VertexLayout::kText, VK_TRUE);
            postPipeline_ = CreatePipeline(postVertex, postFragment, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE,
                VertexLayout::kNone, VK_FALSE, postRenderPass_);
            bloomThresholdPipeline_ = CreatePipeline(postVertex, bloomThresholdFragment,
                VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VertexLayout::kNone, VK_FALSE, bloomRenderPass_);
            bloomDownPipeline_ = CreatePipeline(postVertex, bloomDownFragment,
                VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VertexLayout::kNone, VK_FALSE, bloomRenderPass_);
            bloomUpPipeline_ = CreatePipeline(postVertex, bloomUpFragment,
                VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VertexLayout::kNone, VK_FALSE, bloomRenderPass_);
            if (pipeline_ == VK_NULL_HANDLE || skyboxPipeline_ == VK_NULL_HANDLE ||
                modelPipeline_ == VK_NULL_HANDLE || shadowPipeline_ == VK_NULL_HANDLE ||
                overlayPipeline_ == VK_NULL_HANDLE ||
                iconPipeline_ == VK_NULL_HANDLE ||
                textPipeline_ == VK_NULL_HANDLE || postPipeline_ == VK_NULL_HANDLE ||
                bloomThresholdPipeline_ == VK_NULL_HANDLE || bloomDownPipeline_ == VK_NULL_HANDLE ||
                bloomUpPipeline_ == VK_NULL_HANDLE) {
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        vkDestroyShaderModule(device_, cubeVertex, nullptr);
        vkDestroyShaderModule(device_, cubeFragment, nullptr);
        vkDestroyShaderModule(device_, skyboxVertex, nullptr);
        vkDestroyShaderModule(device_, skyboxFragment, nullptr);
        vkDestroyShaderModule(device_, modelVertex, nullptr);
        vkDestroyShaderModule(device_, modelFragment, nullptr);
        vkDestroyShaderModule(device_, shadowVertex, nullptr);
        vkDestroyShaderModule(device_, shadowFragment, nullptr);
        vkDestroyShaderModule(device_, overlayVertex, nullptr);
        vkDestroyShaderModule(device_, overlayFragment, nullptr);
        vkDestroyShaderModule(device_, iconVertex, nullptr);
        vkDestroyShaderModule(device_, iconFragment, nullptr);
        vkDestroyShaderModule(device_, textVertex, nullptr);
        vkDestroyShaderModule(device_, textFragment, nullptr);
        vkDestroyShaderModule(device_, postVertex, nullptr);
        vkDestroyShaderModule(device_, postFragment, nullptr);
        vkDestroyShaderModule(device_, bloomThresholdFragment, nullptr);
        vkDestroyShaderModule(device_, bloomDownFragment, nullptr);
        vkDestroyShaderModule(device_, bloomUpFragment, nullptr);
        if (result != VK_SUCCESS) {
            if (pipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, pipeline_, nullptr);
                pipeline_ = VK_NULL_HANDLE;
            }
            if (skyboxPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, skyboxPipeline_, nullptr);
                skyboxPipeline_ = VK_NULL_HANDLE;
            }
            if (modelPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, modelPipeline_, nullptr);
                modelPipeline_ = VK_NULL_HANDLE;
            }
            if (shadowPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, shadowPipeline_, nullptr);
                shadowPipeline_ = VK_NULL_HANDLE;
            }
            if (overlayPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, overlayPipeline_, nullptr);
                overlayPipeline_ = VK_NULL_HANDLE;
            }
            if (iconPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, iconPipeline_, nullptr);
                iconPipeline_ = VK_NULL_HANDLE;
            }
            if (textPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, textPipeline_, nullptr);
                textPipeline_ = VK_NULL_HANDLE;
            }
            if (postPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, postPipeline_, nullptr);
                postPipeline_ = VK_NULL_HANDLE;
            }
            if (bloomThresholdPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomThresholdPipeline_, nullptr);
                bloomThresholdPipeline_ = VK_NULL_HANDLE;
            }
            if (bloomDownPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomDownPipeline_, nullptr);
                bloomDownPipeline_ = VK_NULL_HANDLE;
            }
            if (bloomUpPipeline_ != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, bloomUpPipeline_, nullptr);
                bloomUpPipeline_ = VK_NULL_HANDLE;
            }
            if (pipelineLayout_ != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
                pipelineLayout_ = VK_NULL_HANDLE;
            }
            return false;
        }
        return true;
    }

    VkFormat FindSceneColorFormat() const
    {
        const VkFormat candidates[] = {
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        };
        for (VkFormat format : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
            const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((properties.optimalTilingFeatures & required) == required) {
                return format;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkFormat FindDepthFormat() const
    {
        const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
        for (VkFormat format : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                return format;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkImageAspectFlags DepthAspectMask() const
    {
        return depthFormat_ == VK_FORMAT_D24_UNORM_S8_UINT ?
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    bool CreateDepthResources()
    {
        // The render pass references depthFormat_, so make sure it is
        // resolved before CreateRenderPass() runs.  CreateSwapchain() is
        // invoked first and already calls this function, but the format
        // resolution is kept idempotent so the ordering is not load-bearing.
        if (depthFormat_ == VK_FORMAT_UNDEFINED) {
            depthFormat_ = FindDepthFormat();
        }
        if (depthFormat_ == VK_FORMAT_UNDEFINED) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=depth_format_missing");
            return false;
        }
        if (!CreateImage(SceneExtent().width, SceneExtent().height, 1, depthFormat_,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, depthImage_, depthMemory_)) {
            return false;
        }
        if (!CreateImageView(depthImage_, depthFormat_, VK_IMAGE_VIEW_TYPE_2D, 1, DepthAspectMask(),
            &depthImageView_)) {
            DestroyDepthResources();
            return false;
        }
        return true;
    }

    bool CreateShadowTarget()
    {
        if (shadowFramebuffer_ != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_, shadowFramebuffer_, nullptr);
            shadowFramebuffer_ = VK_NULL_HANDLE;
        }
        DestroyTexture(shadowMap_);
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, depthFormat_, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=shadow_format_not_sampled format=%{public}d",
                static_cast<int>(depthFormat_));
            return false;
        }
        if (!CreateImage(kShadowMapSize, kShadowMapSize, 1, depthFormat_,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            0, shadowMap_.image, shadowMap_.memory)) {
            return false;
        }
        shadowMap_.layerCount = 1;
        shadowMap_.mipLevels = 1;
        // Sample only the depth aspect, including when the selected format has
        // a packed stencil component.
        if (!CreateImageView(shadowMap_.image, depthFormat_, VK_IMAGE_VIEW_TYPE_2D, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT, &shadowMap_.view)) {
            DestroyTexture(shadowMap_);
            return false;
        }
        VkImageView attachment = shadowMap_.view;
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = shadowRenderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = kShadowMapSize;
        framebufferInfo.height = kShadowMapSize;
        framebufferInfo.layers = 1;
        const VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
            &shadowFramebuffer_);
        if (result != VK_SUCCESS) {
            LogVkError("create_shadow_framebuffer", result);
            DestroyTexture(shadowMap_);
            shadowFramebuffer_ = VK_NULL_HANDLE;
            return false;
        }
        SDL_Log("SDL3_VULKAN stage=shadow_target_ready size=%ux%u", kShadowMapSize, kShadowMapSize);
        return true;
    }

    bool CreateCommandPool()
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex_;
        const VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
        if (result != VK_SUCCESS) {
            LogVkError("create_command_pool", result);
            return false;
        }
        return true;
    }

    // Records one model asset from the shared instance stream. Characters use
    // one call with two instances; static props use one instance at a later
    // buffer offset. This mirrors MikanEngine's material-batched model path.
    void RecordModelDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex,
        const ModelGpuAsset& asset, const Mat4& viewProj,
        VkDeviceSize instanceOffset, uint32_t instanceCount,
        float enemyDissolve = 0.0f)
    {
        if (!asset.loaded || modelInstanceBuffer_.buffer == VK_NULL_HANDLE || instanceCount == 0) {
            return;
        }
        const ModelPushConstants pushConstants = {viewProj, Mat4Identity(), enemyDissolve, {}};
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(pushConstants), &pushConstants);
        const size_t materialCount = asset.materials.empty() ? 1 : asset.materials.size();
        for (const ModelMeshResource& mesh : asset.meshes) {
            if (mesh.indexCount == 0 || mesh.vertexBuffer.buffer == VK_NULL_HANDLE ||
                mesh.indexBuffer.buffer == VK_NULL_HANDLE) {
                continue;
            }
            VkDescriptorSet set = VK_NULL_HANDLE;
            const size_t slot = static_cast<size_t>(imageIndex) * materialCount + mesh.materialIndex;
            if (slot < asset.materialSets.size()) {
                set = asset.materialSets[slot];
            }
            if (set == VK_NULL_HANDLE && imageIndex < descriptorSets_.size()) {
                set = descriptorSets_[imageIndex];
            }
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                0, 1, &set, 0, nullptr);
            VkBuffer vertexBuffers[2] = {mesh.vertexBuffer.buffer, modelInstanceBuffer_.buffer};
            VkDeviceSize vertexOffsets[2] = {0, instanceOffset};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, vertexOffsets);
            vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.buffer, 0, mesh.indexType);
            vkCmdDrawIndexed(commandBuffer, mesh.indexCount, instanceCount, 0, 0, 0);
        }
    }

    void RecordShadowDraw(VkCommandBuffer commandBuffer, uint32_t imageIndex,
        const ModelGpuAsset& asset, const Mat4& shadowViewProj,
        VkDeviceSize instanceOffset, uint32_t instanceCount,
        float enemyDissolve = 0.0f)
    {
        if (!asset.loaded || modelInstanceBuffer_.buffer == VK_NULL_HANDLE || instanceCount == 0) {
            return;
        }
        const ModelPushConstants pushConstants = {shadowViewProj, Mat4Identity(), enemyDissolve, {}};
        vkCmdPushConstants(commandBuffer, pipelineLayout_,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(pushConstants), &pushConstants);
        const size_t materialCount = asset.materials.empty() ? 1 : asset.materials.size();
        for (const ModelMeshResource& mesh : asset.meshes) {
            if (mesh.indexCount == 0 || mesh.vertexBuffer.buffer == VK_NULL_HANDLE ||
                mesh.indexBuffer.buffer == VK_NULL_HANDLE) {
                continue;
            }
            VkDescriptorSet set = VK_NULL_HANDLE;
            const size_t slot = static_cast<size_t>(imageIndex) * materialCount + mesh.materialIndex;
            if (slot < asset.materialSets.size()) {
                set = asset.materialSets[slot];
            }
            if (set == VK_NULL_HANDLE && imageIndex < descriptorSets_.size()) {
                set = descriptorSets_[imageIndex];
            }
            if (set == VK_NULL_HANDLE) {
                continue;
            }
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                0, 1, &set, 0, nullptr);
            VkBuffer vertexBuffers[2] = {mesh.vertexBuffer.buffer, modelInstanceBuffer_.buffer};
            VkDeviceSize vertexOffsets[2] = {0, instanceOffset};
            vkCmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, vertexOffsets);
            vkCmdBindIndexBuffer(commandBuffer, mesh.indexBuffer.buffer, 0, mesh.indexType);
            vkCmdDrawIndexed(commandBuffer, mesh.indexCount, instanceCount, 0, 0, 0);
        }
    }

    struct MenuButtonPos {
        float cx;
        float cy;
        float hw;
        float hh;
        float r;
    };

    float UiWidth() const
    {
        return swapchainExtent_.width > 0 ? static_cast<float>(swapchainExtent_.width)
            : static_cast<float>(width_);
    }

    float UiHeight() const
    {
        return swapchainExtent_.height > 0 ? static_cast<float>(swapchainExtent_.height)
            : static_cast<float>(height_);
    }

    // Offscreen chain resolution: the swapchain size scaled by the RENDER
    // SCALE setting.  Mirrors the GLES backend, where the scene/bloom/post
    // targets live at 1080p x renderScale_ and only the final blit runs at
    // full surface resolution.  UI geometry and the post->swapchain pass
    // stay at swapchainExtent_ so HUD quality is independent of the setting.
    VkExtent2D SceneExtent() const
    {
        VkExtent2D extent{};
        extent.width = std::max(static_cast<uint32_t>(
            static_cast<float>(swapchainExtent_.width) * renderScale_), 1U);
        extent.height = std::max(static_cast<uint32_t>(
            static_cast<float>(swapchainExtent_.height) * renderScale_), 1U);
        return extent;
    }

    MenuButtonPos MenuButtonLayout(int index) const
    {
        // SDL reports the initial logical 1024x1024 window on OHOS Vulkan,
        // while the swapchain is the physical 2412x1084 surface.  GLES lays
        // out against the drawable surface, so Vulkan must use it as well.
        const float w = UiWidth();
        const float h = UiHeight();
        const float mn = w < h ? w : h;
        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            if (index == 0) {
                return {w * 0.5f, h * 0.62f, mn * 0.22f, mn * 0.072f, mn * 0.072f};
            }
            return {w * 0.95f, h * 0.085f, mn * 0.040f, mn * 0.040f, mn * 0.040f};
        }
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            // Vertical stack on the right edge, matching the GLES layout:
            // SET above, BAG below, above the action-button cluster.
            if (index == 0) {
                return {w * 0.93f, h * 0.26f, mn * 0.055f, mn * 0.055f, mn * 0.055f};
            }
            return {w * 0.93f, h * 0.42f, mn * 0.055f, mn * 0.055f, mn * 0.055f};
        }
        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            // 0 = close, 1..3 = equipment slots (weapon/helmet/armor),
            // 10+i = bag grid cell i (5 columns x 3 rows).  Same geometry as
            // the GLES backend so both stay visually identical.
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
                return {w * 0.5f, h * 0.60f, mn * 0.24f, mn * 0.072f, mn * 0.072f};
            }
            return {w * 0.5f, h * 0.83f, mn * 0.24f, mn * 0.062f, mn * 0.062f};
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
                    mn * 0.028f, mn * 0.028f, mn * 0.010f};
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
        case 0: return {w * 0.28f, h * kRowY[0], mn * 0.10f, mn * 0.055f, mn * 0.055f};
        case 1: return {w * 0.28f, h * kRowY[1], mn * 0.10f, mn * 0.055f, mn * 0.055f};
        case 2: return {w * 0.28f, h * kRowY[2], mn * 0.10f, mn * 0.055f, mn * 0.055f};
        case 3: return {w * 0.28f, h * kRowY[3], mn * 0.10f, mn * 0.055f, mn * 0.055f};
        default: return {w * 0.93f, h * 0.085f, mn * 0.048f, mn * 0.048f, mn * 0.048f};
        }
    }

    static bool HitRect(float x, float y, const MenuButtonPos& button)
    {
        const float ex = std::fabs(x - button.cx);
        const float ey = std::fabs(y - button.cy);
        if (ex > button.hw || ey > button.hh) {
            return false;
        }
        const float dx = ex - (button.hw - button.r) > 0.0f ? ex - (button.hw - button.r) : 0.0f;
        const float dy = ey - (button.hh - button.r) > 0.0f ? ey - (button.hh - button.r) : 0.0f;
        return dx * dx + dy * dy <= button.r * button.r;
    }

    void HandleMenuTap(float x, float y)
    {
        SDL_Log("SDL3_VULKAN stage=menu_tap x=%.0f y=%.0f screen=%d win=%ux%u",
            x, y, static_cast<int>(uiScreen_), static_cast<unsigned int>(width_),
            static_cast<unsigned int>(height_));
        const float inputWidth = width_ > 0 ? static_cast<float>(width_) : UiWidth();
        const float inputHeight = height_ > 0 ? static_cast<float>(height_) : UiHeight();
        const float uiX = x * UiWidth() / inputWidth;
        const float uiY = y * UiHeight() / inputHeight;
        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            if (HitRect(uiX, uiY, MenuButtonLayout(0))) {
                ResetGameplayScene();
                SDL_Log("SDL3_VULKAN stage=player_retry");
            } else if (HitRect(uiX, uiY, MenuButtonLayout(5))) {
                uiScreen_ = rhi::UiScreen::MainMenu;
                SDL_Log("SDL3_VULKAN stage=menu_open_from_death");
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            if (HitRect(uiX, uiY, MenuButtonLayout(0))) {
                uiScreen_ = rhi::UiScreen::GameplaySettings;
                SDL_Log("SDL3_VULKAN stage=gameplay_settings_open");
            } else if (HitRect(uiX, uiY, MenuButtonLayout(1))) {
                uiScreen_ = rhi::UiScreen::GameplayInventory;
                SDL_Log("SDL3_VULKAN stage=inventory_open");
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            if (HitRect(uiX, uiY, MenuButtonLayout(0))) {
                uiScreen_ = rhi::UiScreen::Gameplay;
                SDL_Log("SDL3_VULKAN stage=inventory_close");
                return;
            }
            for (int slot = 1; slot <= 3; ++slot) {
                if (HitRect(uiX, uiY, MenuButtonLayout(slot))) {
                    inventory_.TapEquip(slot - 1);
                    SDL_Log("SDL3_VULKAN stage=inventory_unequip slot=%d", slot - 1);
                    return;
                }
            }
            for (int cell = 0; cell < inventory::kBagCapacity; ++cell) {
                if (!HitRect(uiX, uiY, MenuButtonLayout(10 + cell))) {
                    continue;
                }
                if (inventory_.DefOf(cell) != nullptr) {
                    inventory_.TapBag(cell);
                    SDL_Log("SDL3_VULKAN stage=inventory_bag_tap cell=%d", cell);
                }
                return;
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::GameplaySettings) {
            for (int index = 0; index <= 5; ++index) {
                if (!HitRect(uiX, uiY, MenuButtonLayout(index))) {
                    continue;
                }
                switch (index) {
                case 0:
                    uiScreen_ = rhi::UiScreen::Gameplay;
                    SDL_Log("SDL3_VULKAN stage=gameplay_settings_close");
                    break;
                case 1:
                    settingFps_ = !settingFps_;
                    SDL_Log("SDL3_VULKAN stage=setting_fps on=%d", settingFps_ ? 1 : 0);
                    break;
                case 2:
                    bloomEnabled_ = !bloomEnabled_;
                    SDL_Log("SDL3_VULKAN stage=setting_bloom on=%d", bloomEnabled_ ? 1 : 0);
                    break;
                case 3:
                    resIndex_ = (resIndex_ + 1) % 4;
                    renderScale_ = kScaleChoices[resIndex_];
                    renderTargetsDirty_ = true;
                    SDL_Log("SDL3_VULKAN stage=setting_res scale=%.2f", renderScale_);
                    break;
                case 4:
                    uiOpacityIndex_ = (uiOpacityIndex_ + 1) % 5;
                    uiOpacity_ = kOpacityChoices[uiOpacityIndex_];
                    SDL_Log("SDL3_VULKAN stage=setting_ui_opacity a=%.2f", uiOpacity_);
                    break;
                case 5:
                    uiScreen_ = rhi::UiScreen::MainMenu;
                    SDL_Log("SDL3_VULKAN stage=menu_open_from_gameplay_settings");
                    break;
                default:
                    break;
                }
                break;
            }
            return;
        }
        if (uiScreen_ == rhi::UiScreen::MainMenu) {
            if (HitRect(uiX, uiY, MenuButtonLayout(0))) {
                ResetGameplayScene();
                SDL_Log("SDL3_VULKAN stage=menu_start");
            } else if (HitRect(uiX, uiY, MenuButtonLayout(1))) {
                uiScreen_ = rhi::UiScreen::Settings;
                SDL_Log("SDL3_VULKAN stage=menu_settings");
            }
            return;
        }
        for (int index = 0; index < 6; ++index) {
            if (!HitRect(uiX, uiY, MenuButtonLayout(index))) {
                continue;
            }
            switch (index) {
            case 0:
                settingFps_ = !settingFps_;
                SDL_Log("SDL3_VULKAN stage=setting_fps on=%d", settingFps_ ? 1 : 0);
                break;
            case 1:
                sensIndex_ = (sensIndex_ + 1) % 3;
                lookSensitivityScale_ = kSensitivityChoices[sensIndex_];
                SDL_Log("SDL3_VULKAN stage=setting_sens scale=%.1f", lookSensitivityScale_);
                break;
            case 2:
                bloomEnabled_ = !bloomEnabled_;
                SDL_Log("SDL3_VULKAN stage=setting_bloom on=%d", bloomEnabled_ ? 1 : 0);
                break;
            case 3:
                resIndex_ = (resIndex_ + 1) % 4;
                renderScale_ = kScaleChoices[resIndex_];
                // Taps are processed after vkAcquireNextImageKHR, so the
                // offscreen chain cannot be torn down here; defer the rebuild
                // to the top of the next DrawOnce, before the next acquire.
                renderTargetsDirty_ = true;
                SDL_Log("SDL3_VULKAN stage=setting_res scale=%.2f", renderScale_);
                break;
            case 4:
                uiOpacityIndex_ = (uiOpacityIndex_ + 1) % 5;
                uiOpacity_ = kOpacityChoices[uiOpacityIndex_];
                SDL_Log("SDL3_VULKAN stage=setting_ui_opacity a=%.2f", uiOpacity_);
                break;
            default:
                uiScreen_ = rhi::UiScreen::MainMenu;
                SDL_Log("SDL3_VULKAN stage=menu_back");
                break;
            }
            break;
        }
    }

    void UpdateFrameTiming()
    {
        const Uint64 nowTicks = SDL_GetTicks();
        if (fpsLastTicks_ != 0) {
            const float dtMs = static_cast<float>(nowTicks - fpsLastTicks_);
            if (dtMs > 0.0f && dtMs < 1000.0f) {
                fpsAvgMs_ = fpsAvgMs_ <= 0.0f ? dtMs : fpsAvgMs_ * 0.9f + dtMs * 0.1f;
            }
        }
        fpsLastTicks_ = nowTicks;
    }

    void ConsumeCameraAndMenuInput()
    {
        if (cameraInput_.menuTapEdge) {
            HandleMenuTap(cameraInput_.menuTapX, cameraInput_.menuTapY);
            cameraInput_.menuTapEdge = false;
        }

        constexpr float kLookSensitivity = 0.0055f;
        constexpr float kZoomWorldPerPixel = 0.004f;
        if (uiScreen_ == rhi::UiScreen::Gameplay) {
            camYaw_ += cameraInput_.lookDeltaX * kLookSensitivity * lookSensitivityScale_;
            camPitch_ += cameraInput_.lookDeltaY * kLookSensitivity * lookSensitivityScale_;
            camPitch_ = camPitch_ > 1.35f ? 1.35f : (camPitch_ < -1.35f ? -1.35f : camPitch_);
            camDistance_ -= cameraInput_.zoomDelta * kZoomWorldPerPixel;
            camDistance_ = camDistance_ < 1.2f ? 1.2f : (camDistance_ > 6.0f ? 6.0f : camDistance_);
        }
        cameraInput_.lookDeltaX = 0.0f;
        cameraInput_.lookDeltaY = 0.0f;
        cameraInput_.zoomDelta = 0.0f;
        cameraInput_.menuTapX = 0.0f;
        cameraInput_.menuTapY = 0.0f;
        // Action edges belong to the gameplay animation state machine.  Do
        // not consume them here: the previous Vulkan path cleared the mask
        // before UpdatePlayerAnimation(), making ATK/JMP/CROUCH no-ops.
        if (uiScreen_ != rhi::UiScreen::Gameplay) {
            cameraInput_.actionButtonPressedMask = 0;
        }
    }

    void AppendCircle(std::vector<OverlayVertex>& vertices, float cx, float cy, float radius,
        const float color[4]) const
    {
        // The overlay pipeline consumes a triangle list, so each fan sector
        // must carry its own copy of the centre vertex.  Submitting one
        // centre followed by the circumference only creates one valid
        // triangle and then interprets the remaining perimeter points as
        // unrelated triangles; on the device that made the touch circles
        // look like faint arcs or disappear completely.
        const float alpha = color[3] * uiOpacity_;
        for (int i = 0; i < kOverlaySegments; ++i) {
            const float angle0 = static_cast<float>(i) /
                static_cast<float>(kOverlaySegments) * 6.28318530717958647692f;
            const float angle1 = static_cast<float>(i + 1) /
                static_cast<float>(kOverlaySegments) * 6.28318530717958647692f;
            vertices.push_back({cx, cy, color[0], color[1], color[2], alpha});
            vertices.push_back({cx + std::cos(angle0) * radius,
                cy + std::sin(angle0) * radius, color[0], color[1], color[2], alpha});
            vertices.push_back({cx + std::cos(angle1) * radius,
                cy + std::sin(angle1) * radius, color[0], color[1], color[2], alpha});
        }
    }

    void AppendRoundedRect(std::vector<OverlayVertex>& vertices, const MenuButtonPos& button,
        const float color[4]) const
    {
        const float alpha = color[3] * uiOpacity_;
        const float x0 = button.cx - button.hw;
        const float x1 = button.cx + button.hw;
        const float y0 = button.cy - button.hh;
        const float y1 = button.cy + button.hh;
        const float radius = std::min(button.r, std::min(button.hw, button.hh));
        const auto push = [&](float x, float y) {
            vertices.push_back({x, y, color[0], color[1], color[2], alpha});
        };
        const auto pushQuad = [&](float ax, float ay, float bx, float by) {
            push(ax, ay); push(bx, ay); push(bx, by);
            push(ax, ay); push(bx, by); push(ax, by);
        };
        const auto pushCorner = [&](float cx, float cy, float a0, float a1) {
            constexpr int kCornerSegments = 12;
            for (int i = 0; i < kCornerSegments; ++i) {
                const float t0 = a0 + (a1 - a0) * static_cast<float>(i) /
                    static_cast<float>(kCornerSegments);
                const float t1 = a0 + (a1 - a0) * static_cast<float>(i + 1) /
                    static_cast<float>(kCornerSegments);
                push(cx, cy);
                push(cx + std::cos(t0) * radius, cy + std::sin(t0) * radius);
                push(cx + std::cos(t1) * radius, cy + std::sin(t1) * radius);
            }
        };
        pushQuad(x0 + radius, y0, x1 - radius, y1);
        pushQuad(x0, y0 + radius, x0 + radius, y1 - radius);
        pushQuad(x1 - radius, y0 + radius, x1, y1 - radius);
        pushCorner(x0 + radius, y0 + radius, 3.14159265f, 4.71238898f);
        pushCorner(x1 - radius, y0 + radius, 4.71238898f, 6.28318531f);
        pushCorner(x1 - radius, y1 - radius, 0.0f, 1.57079633f);
        pushCorner(x0 + radius, y1 - radius, 1.57079633f, 3.14159265f);
    }

    void AppendIconQuad(std::vector<IconVertex>& vertices, float centerX, float centerY,
        float radius, const float color[4]) const
    {
        const float halfSize = radius * 0.39f;
        const float alpha = color[3] * uiOpacity_;
        const auto push = [&](float x, float y, float u, float v) {
            vertices.push_back({x, y, u, v, color[0], color[1], color[2], alpha});
        };
        push(centerX - halfSize, centerY - halfSize, 0.0f, 0.0f);
        push(centerX - halfSize, centerY + halfSize, 0.0f, 1.0f);
        push(centerX + halfSize, centerY + halfSize, 1.0f, 1.0f);
        push(centerX - halfSize, centerY - halfSize, 0.0f, 0.0f);
        push(centerX + halfSize, centerY + halfSize, 1.0f, 1.0f);
        push(centerX + halfSize, centerY - halfSize, 1.0f, 0.0f);
    }

    void AppendTextPx(std::vector<TextVertex>& vertices, const char* text, float x, float y,
        float emPx, const float color[4]) const
    {
        if (text == nullptr || *text == '\0') {
            return;
        }
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        const float atlasW = static_cast<float>(rhi::kSdfAtlasWidth);
        const float atlasH = static_cast<float>(rhi::kSdfAtlasHeight);
        const float pad = static_cast<float>(rhi::kSdfCellSize - rhi::kSdfFontEm) * 0.5f;
        float penX = x;
        for (const char* cursor = text; *cursor != '\0';) {
            const unsigned int codepoint = rhi::SdfNextCodepoint(&cursor);
            const rhi::SdfGlyph* glyph = rhi::SdfFindGlyph(codepoint);
            if (glyph == nullptr || glyph->w <= 0.0f) {
                penX += emPx * 0.30f;
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
            const float range = rhi::kSdfPxRange * scale;
            const float alpha = color[3] * uiOpacity_;
            const auto push = [&](float px, float py, float u, float v) {
                vertices.push_back({px, py, u, v, color[0], color[1], color[2], alpha, range});
            };
            push(x0, y0, u0, v0); push(x0, y1, u0, v1); push(x1, y1, u1, v1);
            push(x0, y0, u0, v0); push(x1, y1, u1, v1); push(x1, y0, u1, v0);
            penX += glyph->advance * scale;
        }
    }

    void AppendTextCentered(std::vector<TextVertex>& vertices, const char* text, float cx,
        float cy, float emPx, const float color[4], bool drawShadow = true) const
    {
        if (text == nullptr) {
            return;
        }
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        float advanceSum = 0.0f;
        for (const char* cursor = text; *cursor != '\0';) {
            const unsigned int codepoint = rhi::SdfNextCodepoint(&cursor);
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
            AppendTextPx(vertices, text, x + 2.0f, y + 2.0f, emPx, shadow);
        }
        AppendTextPx(vertices, text, x, y, emPx, color);
    }

    void AppendTextLeftAligned(std::vector<TextVertex>& vertices, const char* text,
        float leftX, float cy, float emPx, const float color[4], bool drawShadow = true) const
    {
        if (text == nullptr) {
            return;
        }
        const float scale = emPx / static_cast<float>(rhi::kSdfFontEm);
        const float y = cy - 22.5f * scale;
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.55f};
        if (drawShadow) {
            AppendTextPx(vertices, text, leftX + 2.0f, y + 2.0f, emPx, shadow);
        }
        AppendTextPx(vertices, text, leftX, y, emPx, color);
    }

    // Mikan hit feedback: brief full-screen red flash, alpha ramps 1 -> 0
    // over kHitFlashDuration and follows the global UI opacity.
    void AppendHitFlash(std::vector<OverlayVertex>& overlay, float w, float h)
    {
        if (playerHitFlashTimer_ <= 0.0f) {
            return;
        }
        const float flash = std::min(playerHitFlashTimer_ / kHitFlashDuration, 1.0f);
        const float color[4] = {0.92f, 0.05f, 0.03f, 0.28f * flash * uiOpacity_};
        const MenuButtonPos fullscreen = {w * 0.5f, h * 0.5f,
            w * 0.5f + 1.0f, h * 0.5f + 1.0f, 0.0f};
        AppendRoundedRect(overlay, fullscreen, color);
    }

    void AppendPlayerHealthBar(std::vector<OverlayVertex>& overlay,
        std::vector<TextVertex>& text) const
    {
        const float w = UiWidth();
        const float h = UiHeight();
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
        AppendRoundedRect(overlay, frame, frameColor);
        if (ratio > 0.0f) {
            const float fillHalfWidth = std::max(0.0f, barHalfWidth - mn * 0.004f) * ratio;
            const MenuButtonPos fill = {
                barCenterX - barHalfWidth + mn * 0.004f + fillHalfWidth,
                barCenterY, fillHalfWidth, std::max(0.0f, barHalfHeight - mn * 0.004f),
                std::max(0.0f, radius - mn * 0.003f)};
            AppendRoundedRect(overlay, fill, fillColor);
        }
        char label[32]{};
        std::snprintf(label, sizeof(label), "血量 %.0f/%.0f", playerHealth_, playerMaxHealth_);
        AppendTextCentered(text, label, barCenterX, barCenterY, mn * 0.030f, white);
    }

    void AppendEnemyHealthBar(std::vector<OverlayVertex>& overlay,
        std::vector<TextVertex>& text) const
    {
        if (!enemyAlive_) {
            return;
        }
        const float w = UiWidth();
        const float h = UiHeight();
        const float mn = w < h ? w : h;
        const float localHead[3] = {0.0f, 1.28f, 0.0f};
        float worldHead[3]{};
        Mat4TransformPoint(enemyMatrix_, localHead, worldHead);
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
        AppendRoundedRect(overlay, frame, frameColor);
        if (ratio > 0.0f) {
            const float fillHalfWidth = std::max(0.0f, barHalfWidth - mn * 0.0025f) * ratio;
            const MenuButtonPos fill = {
                centerX - barHalfWidth + mn * 0.0025f + fillHalfWidth, barCenterY,
                fillHalfWidth, std::max(0.0f, barHalfHeight - mn * 0.0025f),
                std::max(0.0f, radius - mn * 0.0015f)};
            AppendRoundedRect(overlay, fill, healthRed);
        }
        char label[32]{};
        std::snprintf(label, sizeof(label), "敌人 %.0f/%.0f", enemyHealth_, enemyMaxHealth_);
        AppendTextCentered(text, label, centerX, barCenterY - mn * 0.022f, mn * 0.020f, white);
    }

    void BuildUiGeometry(std::vector<OverlayVertex>& overlay, std::vector<IconVertex>& icons,
        std::vector<TextVertex>& text)
    {
        if (UiWidth() <= 0.0f || UiHeight() <= 0.0f) {
            return;
        }
        const float w = UiWidth();
        const float h = UiHeight();
        const float mn = w < h ? w : h;
        const float white[4] = {1.0f, 1.0f, 1.0f, 0.92f};
        const float panel[4] = {0.30f, 0.32f, 0.36f, 0.55f};
        const float accent[4] = {0.20f, 0.48f, 0.66f, 1.0f};
        if (uiScreen_ == rhi::UiScreen::MainMenu) {
            AppendTextCentered(text, "MIKAN", w * 0.5f, h * 0.20f, mn * 0.105f, white);
            AppendTextCentered(text, "SDL3 鸿蒙运行时", w * 0.5f, h * 0.30f, mn * 0.030f, white);
            const MenuButtonPos start = MenuButtonLayout(0);
            AppendRoundedRect(overlay, start, accent);
            AppendTextCentered(text, "开始游戏", start.cx, start.cy, start.hh * 0.92f, white);
            const MenuButtonPos settings = MenuButtonLayout(1);
            AppendRoundedRect(overlay, settings, panel);
            AppendTextCentered(text, "设置", settings.cx, settings.cy, settings.hh * 0.85f, white);
            return;
        }
        if (uiScreen_ == rhi::UiScreen::Settings) {
            const float settingsBackground[4] = {0.015f, 0.035f, 0.055f, 0.20f};
            const float settingsSheet[4] = {0.94f, 0.97f, 0.99f, 0.84f};
            const float rowColor[4] = {0.96f, 0.98f, 1.0f, 0.30f};
            const float controlColor[4] = {0.72f, 0.82f, 0.90f, 0.78f};
            const float activeColor[4] = {0.20f, 0.48f, 0.66f, 0.94f};
            const float ink[4] = {0.08f, 0.14f, 0.20f, 0.98f};
            const float secondaryInk[4] = {0.18f, 0.26f, 0.34f, 0.98f};
            const float dividerColor[4] = {0.22f, 0.34f, 0.44f, 0.28f};
            const float leftX = w * 0.5f - mn * 0.35f;
            AppendRoundedRect(overlay,
                {w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f},
                settingsBackground);
            AppendRoundedRect(overlay,
                {w * 0.5f, h * 0.5f, mn * 0.43f, mn * 0.39f, mn * 0.020f},
                settingsSheet);
            AppendTextLeftAligned(text, "设置", leftX, h * 0.22f, mn * 0.050f, ink, false);
            const MenuButtonPos back = MenuButtonLayout(5);
            AppendRoundedRect(overlay, back, rowColor);
            AppendTextCentered(text, "X", back.cx, back.cy, back.hh * 0.82f, ink, false);
            AppendRoundedRect(overlay,
                {w * 0.5f, h * 0.30f, mn * 0.37f, mn * 0.001f, 0.0f},
                dividerColor);

            static const char* const kLabels[5] = {
                "调试面板", "视角灵敏度", "泛光", "渲染分辨率", "UI透明度"};
            for (int settingIndex = 0; settingIndex < 5; ++settingIndex) {
                const MenuButtonPos control = MenuButtonLayout(settingIndex);
                const MenuButtonPos row = {w * 0.5f, control.cy,
                    mn * 0.40f, mn * 0.045f, mn * 0.008f};
                AppendRoundedRect(overlay, row, rowColor);
                const bool isToggle = settingIndex == 0 || settingIndex == 2;
                const bool enabled = settingIndex == 0 ? settingFps_ : bloomEnabled_;
                const float* valueColor = isToggle && enabled ? activeColor : controlColor;
                const float* valueInk = isToggle && enabled ? white : ink;
                AppendRoundedRect(overlay, control, valueColor);
                AppendTextLeftAligned(text, kLabels[settingIndex], leftX, control.cy,
                    mn * 0.032f, secondaryInk, false);

                char value[16]{};
                if (settingIndex == 0) {
                    std::snprintf(value, sizeof(value), "%s", settingFps_ ? "开" : "关");
                } else if (settingIndex == 1) {
                    std::snprintf(value, sizeof(value), "%.1f", lookSensitivityScale_);
                } else if (settingIndex == 2) {
                    std::snprintf(value, sizeof(value), "%s", bloomEnabled_ ? "开" : "关");
                } else if (settingIndex == 3) {
                    std::snprintf(value, sizeof(value), "%.0f%%", renderScale_ * 100.0f);
                } else {
                    std::snprintf(value, sizeof(value), "%.0f%%", uiOpacity_ * 100.0f);
                }
                AppendTextCentered(text, value, control.cx, control.cy,
                    control.hh * 0.84f, valueInk, false);
            }
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplayRetry) {
            const MenuButtonPos dim = {w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, 0.0f};
            const float dimColor[4] = {0.005f, 0.008f, 0.015f, 0.72f};
            const float danger[4] = {0.72f, 0.16f, 0.12f, 0.88f};
            AppendRoundedRect(overlay, dim, dimColor);
            AppendTextCentered(text, "你死了", w * 0.5f, h * 0.36f, mn * 0.085f, white);
            const MenuButtonPos retry = MenuButtonLayout(0);
            AppendRoundedRect(overlay, retry, danger);
            AppendTextCentered(text, "重试", retry.cx, retry.cy, retry.hh * 0.88f, white);
            const MenuButtonPos menu = MenuButtonLayout(5);
            AppendRoundedRect(overlay, menu, panel);
            AppendTextCentered(text, "菜单", menu.cx, menu.cy, menu.hh * 0.52f, white);
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplaySettings) {
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
            AppendRoundedRect(overlay, dim, dimColor);
            AppendRoundedRect(overlay, sheet, sheetColor);
            AppendTextLeftAligned(text, "显示设置", labelX, h * 0.275f, mn * 0.050f, ink, false);
            const MenuButtonPos close = MenuButtonLayout(0);
            AppendRoundedRect(overlay, close, controlColor);
            AppendTextCentered(text, "X", close.cx, close.cy, close.hh * 0.90f, ink, false);
            const float dividerY[5] = {0.31f, 0.41f, 0.51f, 0.61f, 0.71f};
            for (float rowY : dividerY) {
                AppendRoundedRect(overlay,
                    {w * 0.5f, h * rowY, mn * 0.27f, mn * 0.0012f, 0.0f},
                    dividerColor);
            }
            const MenuButtonPos fps = MenuButtonLayout(1);
            const float* fpsColor = settingFps_ ? activeColor : controlColor;
            const float* fpsInk = settingFps_ ? white : ink;
            AppendRoundedRect(overlay, fps, fpsColor);
            AppendTextCentered(text, settingFps_ ? "开" : "关", fps.cx, fps.cy,
                fps.hh * 0.85f, fpsInk, false);
            AppendTextLeftAligned(text, "调试面板", labelX, fps.cy, mn * 0.030f,
                labelColor, false);
            const MenuButtonPos bloom = MenuButtonLayout(2);
            const float* bloomColor = bloomEnabled_ ? activeColor : controlColor;
            const float* bloomInk = bloomEnabled_ ? white : ink;
            AppendRoundedRect(overlay, bloom, bloomColor);
            AppendTextCentered(text, bloomEnabled_ ? "开" : "关", bloom.cx, bloom.cy,
                bloom.hh * 0.85f, bloomInk, false);
            AppendTextLeftAligned(text, "泛光", labelX, bloom.cy, mn * 0.030f,
                labelColor, false);
            const MenuButtonPos res = MenuButtonLayout(3);
            AppendRoundedRect(overlay, res, controlColor);
            char displayScaleLabel[8]{};
            std::snprintf(displayScaleLabel, sizeof(displayScaleLabel), "x%.2f", renderScale_);
            AppendTextCentered(text, displayScaleLabel, res.cx, res.cy,
                res.hh * 0.85f, ink, false);
            AppendTextLeftAligned(text, "渲染分辨率", labelX, res.cy, mn * 0.030f,
                labelColor, false);
            const MenuButtonPos opacity = MenuButtonLayout(4);
            AppendRoundedRect(overlay, opacity, controlColor);
            char opacityLabel[8]{};
            std::snprintf(opacityLabel, sizeof(opacityLabel), "%.0f%%",
                uiOpacity_ * 100.0f);
            AppendTextCentered(text, opacityLabel, opacity.cx, opacity.cy,
                opacity.hh * 0.85f, ink, false);
            AppendTextLeftAligned(text, "UI透明度", labelX, opacity.cy, mn * 0.030f,
                labelColor, false);
            const MenuButtonPos title = MenuButtonLayout(5);
            AppendRoundedRect(overlay, title, accent);
            AppendTextCentered(text, "返回游戏标题", title.cx, title.cy, mn * 0.032f, white);
            return;
        }

        if (uiScreen_ == rhi::UiScreen::GameplayInventory) {
            // Same glass-card language as the settings sheet: light panel over
            // the frozen live scene, soft control pills, dark ink labels.
            // Geometry mirrors the GLES backend cell for cell.
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
            AppendRoundedRect(overlay, dim, dimColor);
            AppendRoundedRect(overlay, sheet, sheetColor);
            AppendTextLeftAligned(text, "背包", labelX, h * 0.175f, mn * 0.050f, ink, false);
            AppendTextLeftAligned(text, "装备", w * 0.5f - mn * 0.335f - mn * 0.068f,
                h * 0.245f, mn * 0.032f, secondaryInk, false);
            AppendTextLeftAligned(text, "物品", w * 0.5f - mn * 0.135f,
                h * 0.245f, mn * 0.032f, secondaryInk, false);
            const MenuButtonPos close = MenuButtonLayout(0);
            AppendRoundedRect(overlay, close, slotColor);
            AppendTextCentered(text, "X", close.cx, close.cy, close.hh * 0.90f, ink, false);

            static const char* const kSlotNames[3] = {"武器", "头盔", "护甲"};
            for (int slot = 1; slot <= 3; ++slot) {
                const MenuButtonPos slotPos = MenuButtonLayout(slot);
                const int bagIndex = inventory_.equipped[slot - 1];
                const inventory::ItemDef* def = inventory_.DefOf(bagIndex);
                AppendRoundedRect(overlay, slotPos, def != nullptr ? activeColor : emptyColor);
                if (def != nullptr) {
                    const MenuButtonPos icon = {slotPos.cx, slotPos.cy - mn * 0.022f,
                        mn * 0.030f, mn * 0.030f, mn * 0.008f};
                    AppendRoundedRect(overlay, icon, def->color);
                    AppendTextCentered(text, def->name, slotPos.cx,
                        slotPos.cy + mn * 0.040f, mn * 0.024f, ink, false);
                } else {
                    AppendTextCentered(text, "空", slotPos.cx, slotPos.cy,
                        mn * 0.030f, secondaryInk, false);
                }
                AppendTextCentered(text, kSlotNames[slot - 1], slotPos.cx,
                    slotPos.cy + mn * 0.095f, mn * 0.026f, secondaryInk, false);
            }

            for (int cell = 0; cell < inventory::kBagCapacity; ++cell) {
                const MenuButtonPos cellPos = MenuButtonLayout(10 + cell);
                const inventory::ItemDef* def = inventory_.DefOf(cell);
                AppendRoundedRect(overlay, cellPos, def != nullptr ? slotColor : emptyColor);
                if (def != nullptr) {
                    const MenuButtonPos icon = {cellPos.cx, cellPos.cy - mn * 0.014f,
                        mn * 0.020f, mn * 0.020f, mn * 0.006f};
                    AppendRoundedRect(overlay, icon, def->color);
                    AppendTextCentered(text, def->name, cellPos.cx,
                        cellPos.cy + mn * 0.022f, mn * 0.019f, ink, false);
                    if (inventory::kItemDefs[inventory_.bag[cell].defId].maxCount > 1) {
                        char countLabel[8]{};
                        std::snprintf(countLabel, sizeof(countLabel), "x%d",
                            inventory_.bag[cell].count);
                        AppendTextCentered(text, countLabel, cellPos.cx + mn * 0.024f,
                            cellPos.cy + mn * 0.036f, mn * 0.016f, secondaryInk, false);
                    }
                }
            }
            AppendTextCentered(text, "轻点物品装备 轻点装备栏卸下", w * 0.5f, h * 0.82f,
                mn * 0.026f, secondaryInk, false);
            return;
        }

        AppendPlayerHealthBar(overlay, text);
        AppendEnemyHealthBar(overlay, text);
        AppendHitFlash(overlay, w, h);
        if (cameraInput_.showOverlay) {
            const float inputWidth = width_ > 0 ? static_cast<float>(width_) : w;
            const float inputHeight = height_ > 0 ? static_cast<float>(height_) : h;
            const float inputScaleX = w / inputWidth;
            const float inputScaleY = h / inputHeight;
            const float inputScale = std::min(inputScaleX, inputScaleY);
            const float stickBase[4] = {0.30f, 0.32f, 0.36f, 0.40f};
            const float stickKnob[4] = {cameraInput_.stickActive ? 0.78f : 0.60f,
                cameraInput_.stickActive ? 0.82f : 0.64f, cameraInput_.stickActive ? 0.90f : 0.72f,
                cameraInput_.stickActive ? 0.70f : 0.42f};
            AppendCircle(overlay, cameraInput_.stickBaseX * inputScaleX,
                cameraInput_.stickBaseY * inputScaleY, cameraInput_.stickRadius * inputScale, stickBase);
            AppendCircle(overlay, cameraInput_.stickKnobX * inputScaleX,
                cameraInput_.stickKnobY * inputScaleY, cameraInput_.knobRadius * inputScale, stickKnob);
            for (int index = 0; index < rhi::CameraInput::kActionButtonCount; ++index) {
                // One circle per button, same panel gray and base alpha as
                // the menu buttons so every control shares one opacity scale.
                const float buttonColor[4] = {cameraInput_.actionButtonHeld[index] ? 0.98f : 0.30f,
                    cameraInput_.actionButtonHeld[index] ? 0.72f : 0.32f,
                    cameraInput_.actionButtonHeld[index] ? 0.25f : 0.36f,
                    cameraInput_.actionButtonHeld[index] ? 0.70f : 0.55f};
                const float buttonX = cameraInput_.actionButtonX[index] * inputScaleX;
                const float buttonY = cameraInput_.actionButtonY[index] * inputScaleY;
                const float buttonRadius = cameraInput_.actionButtonRadius * inputScale;
                AppendCircle(overlay, buttonX, buttonY,
                    buttonRadius + 5.0f * inputScale, buttonColor);
                const bool hasIcon = actionIconTextures_[index].view != VK_NULL_HANDLE;
                const float iconColor[4] = {cameraInput_.actionButtonHeld[index] ? 1.0f : 0.94f,
                    cameraInput_.actionButtonHeld[index] ? 0.92f : 0.95f,
                    cameraInput_.actionButtonHeld[index] ? 0.68f : 0.97f,
                    hasIcon ? (cameraInput_.actionButtonHeld[index] ? 0.98f : 0.92f) : 0.0f};
                // Keep six vertices per logical button even if one optional
                // asset failed to decode; the transparent tint preserves the
                // descriptor-to-button mapping for the remaining icons.
                AppendIconQuad(icons, buttonX, buttonY, buttonRadius, iconColor);
            }
        }
        if (settingFps_ && fpsAvgMs_ > 0.0f) {
            char fpsText[32]{};
            std::snprintf(fpsText, sizeof(fpsText), "FPS %.1f", 1000.0f / fpsAvgMs_);
            AppendTextPx(text, fpsText, 46.0f, 38.0f, 90.0f, panel);
            AppendTextPx(text, fpsText, 40.0f, 32.0f, 90.0f, white);
            const float stateColor[4] = {1.0f, 0.85f, 0.45f, 0.95f};
            AppendTextPx(text, AnimationStateName(playerAsset_.animationState), 42.0f, 162.0f,
                58.0f, stateColor);
        }
        const MenuButtonPos set = MenuButtonLayout(0);
        AppendRoundedRect(overlay, set, panel);
        AppendTextCentered(text, "设置", set.cx, set.cy, set.hh * 0.58f, white);
        const MenuButtonPos bag = MenuButtonLayout(1);
        AppendRoundedRect(overlay, bag, panel);
        AppendTextCentered(text, "背包", bag.cx, bag.cy, bag.hh * 0.52f, white);
    }

    bool UploadUiGeometry(BufferResource& target, const std::vector<OverlayVertex>& overlay,
        const std::vector<IconVertex>& icons, const std::vector<TextVertex>& text)
    {
        const VkDeviceSize overlayBytes = static_cast<VkDeviceSize>(overlay.size() * sizeof(OverlayVertex));
        const VkDeviceSize iconBytes = static_cast<VkDeviceSize>(icons.size() * sizeof(IconVertex));
        const VkDeviceSize textBytes = static_cast<VkDeviceSize>(text.size() * sizeof(TextVertex));
        if (overlayBytes > kUiIconOffset || iconBytes > kUiTextOffset - kUiIconOffset ||
            textBytes > kUiVertexBufferSize - kUiTextOffset) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_VULKAN stage=ui_geometry_too_large overlay=%llu icons=%llu text=%llu",
                static_cast<unsigned long long>(overlayBytes), static_cast<unsigned long long>(iconBytes),
                static_cast<unsigned long long>(textBytes));
            return false;
        }
        if (target.memory == VK_NULL_HANDLE) {
            return false;
        }
        void* mapped = nullptr;
        const VkResult result = vkMapMemory(device_, target.memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS || mapped == nullptr) {
            LogVkError("map_ui_buffer", result);
            return false;
        }
        if (overlayBytes > 0) {
            std::memcpy(mapped, overlay.data(), static_cast<size_t>(overlayBytes));
        }
        if (iconBytes > 0) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + kUiIconOffset, icons.data(),
                static_cast<size_t>(iconBytes));
        }
        if (textBytes > 0) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + kUiTextOffset, text.data(),
                static_cast<size_t>(textBytes));
        }
        vkUnmapMemory(device_, target.memory);
        return true;
    }

    bool RecordFrameCommands(size_t index, uint32_t overlayCount, uint32_t iconCount,
        uint32_t textCount)
    {
        if (index >= commandBuffers_.size() || index >= framebuffers_.size() ||
            index >= postFramebuffers_.size() || index >= descriptorSets_.size() ||
            index >= postDescriptorSets_.size() || index >= uiVertexBuffers_.size() ||
            bloomDsFramebuffers_.size() != kBloomDsLevels ||
            bloomUpFramebuffers_.size() != kBloomUpLevels ||
            bloomThresholdDescriptorSet_ == VK_NULL_HANDLE) {
            return false;
        }
        VkCommandBuffer commandBuffer = commandBuffers_[index];
        VkResult result = vkResetCommandBuffer(commandBuffer, 0);
        if (result != VK_SUCCESS) {
            LogVkError("reset_command_buffer", result);
            return false;
        }
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            LogVkError("begin_command_buffer", result);
            return false;
        }
        VkClearValue clearValues[2]{};
        clearValues[0].color.float32[0] = 0.02f;
        clearValues[0].color.float32[1] = 0.04f;
        clearValues[0].color.float32[2] = 0.08f;
        clearValues[0].color.float32[3] = 1.0f;
        clearValues[1].depthStencil.depth = 1.0f;
        const VkExtent2D sceneExtent = SceneExtent();

        const float helmetAngle = static_cast<float>(SDL_GetTicks() % 600000U)
            * 0.001f * 0.8f + 0.5f;
        const Mat4 helmetTransform = Mat4Multiply(
            Mat4Translation(physics::kHelmetCollider.centerX,
                physics::kHelmetCollider.centerY,
                physics::kHelmetCollider.centerZ),
            Mat4Multiply(Mat4RotationY(helmetAngle),
                Mat4Scale(physics::kHelmetCollider.renderScale)));
        const Mat4 groundTransform = Mat4Multiply(
            Mat4Translation(physics::kGroundCollider.centerX,
                physics::kGroundCollider.centerY,
                physics::kGroundCollider.centerZ),
            Mat4Scale3(physics::kGroundCollider.halfExtentX,
                physics::kGroundCollider.halfExtentY,
                physics::kGroundCollider.halfExtentZ));
        if (modelInstanceData_.size() < 4) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_VULKAN stage=model_instance_buffer_invalid");
            return false;
        }
        std::fill(modelInstanceData_.begin(), modelInstanceData_.end(), CharacterInstanceData{});
        CharacterInstanceData* instances = modelInstanceData_.data();
        instances[0].model = playerMatrix_;
        instances[0].color[0] = 1.0f;
        instances[0].color[1] = 1.0f;
        instances[0].color[2] = 1.0f;
        // The player keeps the source material color; alpha is marker
        // strength, not material opacity.
        instances[0].color[3] = 0.0f;
        instances[0].skinIndex = 0;
        instances[1].model = enemyMatrix_;
        // Enemy marker is fixed red; it no longer uses the previous
        // orange/blue time interpolation.
        instances[1].color[0] = 1.0f;
        instances[1].color[1] = 0.0f;
        instances[1].color[2] = 0.0f;
        instances[1].color[3] = 1.0f;
        instances[1].skinIndex = 1;
        instances[2].model = helmetTransform;
        instances[2].skinIndex = 0;
        instances[3].model = groundTransform;
        instances[3].skinIndex = 0;
        std::size_t nextStaticInstance = 4;
        if (sceneDefinition_ != nullptr) {
            for (const scene::Entity& entity : sceneDefinition_->entities) {
                if (!entity.importAsStaticCube || nextStaticInstance >= modelInstanceData_.size()) {
                    continue;
                }
                instances[nextStaticInstance].model = Mat4FromSceneEntity(*sceneDefinition_, entity);
                instances[nextStaticInstance].skinIndex = 0;
                ++nextStaticInstance;
            }
        }
        const VkDeviceSize instanceDataBytes =
            sizeof(CharacterInstanceData) * modelInstanceData_.size();
        if (!UploadToMappedBuffer(modelInstanceBuffer_, instances, instanceDataBytes)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_VULKAN stage=model_instance_upload_failed");
            return false;
        }
        const uint32_t characterInstanceCount = enemyVisible_ ? 2u : 1u;

        const bool renderGameplayScene = uiScreen_ == rhi::UiScreen::Gameplay ||
            uiScreen_ == rhi::UiScreen::GameplaySettings ||
            uiScreen_ == rhi::UiScreen::GameplayRetry ||
            uiScreen_ == rhi::UiScreen::GameplayInventory;
        if (renderGameplayScene && shadowFramebuffer_ != VK_NULL_HANDLE &&
            shadowPipeline_ != VK_NULL_HANDLE) {
            VkClearValue shadowClear{};
            shadowClear.depthStencil.depth = 1.0f;
            VkRenderPassBeginInfo shadowPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            shadowPassInfo.renderPass = shadowRenderPass_;
            shadowPassInfo.framebuffer = shadowFramebuffer_;
            shadowPassInfo.renderArea.extent = {kShadowMapSize, kShadowMapSize};
            shadowPassInfo.clearValueCount = 1;
            shadowPassInfo.pClearValues = &shadowClear;
            vkCmdBeginRenderPass(commandBuffer, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport shadowViewport{};
            shadowViewport.width = static_cast<float>(kShadowMapSize);
            shadowViewport.height = static_cast<float>(kShadowMapSize);
            shadowViewport.maxDepth = 1.0f;
            VkRect2D shadowScissor{};
            shadowScissor.extent = {kShadowMapSize, kShadowMapSize};
            vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
            if (playerAsset_.loaded) {
                RecordShadowDraw(commandBuffer, static_cast<uint32_t>(index), playerAsset_,
                    shadowMatrix_, 0, characterInstanceCount, enemyDissolveAmount_);
            }
            if (helmetAsset_.loaded) {
                RecordShadowDraw(commandBuffer, static_cast<uint32_t>(index), helmetAsset_,
                    shadowMatrix_, sizeof(CharacterInstanceData) * 2, 1);
            }
            if (groundAsset_.loaded) {
                RecordShadowDraw(commandBuffer, static_cast<uint32_t>(index), groundAsset_,
                    shadowMatrix_, sizeof(CharacterInstanceData) * 3, 1);
            }
            if (groundAsset_.loaded && sceneDefinition_ != nullptr) {
                std::size_t staticInstance = 4;
                for (const scene::Entity& entity : sceneDefinition_->entities) {
                    if (!entity.importAsStaticCube) {
                        continue;
                    }
                    if (entity.castShadow) {
                        RecordShadowDraw(commandBuffer, static_cast<uint32_t>(index), groundAsset_,
                            shadowMatrix_, sizeof(CharacterInstanceData) * staticInstance, 1);
                    }
                    ++staticInstance;
                }
            }
            vkCmdEndRenderPass(commandBuffer);
        }

        VkRenderPassBeginInfo sceneRenderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        sceneRenderPassInfo.renderPass = renderPass_;
        sceneRenderPassInfo.framebuffer = framebuffers_[index];
        sceneRenderPassInfo.renderArea.extent = sceneExtent;
        sceneRenderPassInfo.clearValueCount = 2;
        sceneRenderPassInfo.pClearValues = clearValues;
        vkCmdBeginRenderPass(commandBuffer, &sceneRenderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport sceneViewport{};
        sceneViewport.width = static_cast<float>(sceneExtent.width);
        sceneViewport.height = static_cast<float>(sceneExtent.height);
        sceneViewport.maxDepth = 1.0f;
        VkRect2D sceneScissor{};
        sceneScissor.extent = sceneExtent;
        vkCmdSetViewport(commandBuffer, 0, 1, &sceneViewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &sceneScissor);

        if (uiScreen_ == rhi::UiScreen::MainMenu ||
            uiScreen_ == rhi::UiScreen::Settings || renderGameplayScene) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                0, 1, &descriptorSets_[index], 0, nullptr);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            if (renderGameplayScene) {
                if (playerAsset_.loaded || (enemyVisible_ && enemyAsset_.loaded) ||
                    helmetAsset_.loaded || groundAsset_.loaded) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, modelPipeline_);
                    if (playerAsset_.loaded) {
                        RecordModelDraw(commandBuffer, static_cast<uint32_t>(index), playerAsset_,
                            viewProj_, 0, characterInstanceCount, enemyDissolveAmount_);
                    }
                    if (helmetAsset_.loaded) {
                        RecordModelDraw(commandBuffer, static_cast<uint32_t>(index), helmetAsset_,
                            viewProj_, sizeof(CharacterInstanceData) * 2, 1);
                    }
                    if (groundAsset_.loaded) {
                        RecordModelDraw(commandBuffer, static_cast<uint32_t>(index), groundAsset_,
                            viewProj_, sizeof(CharacterInstanceData) * 3, 1);
                    }
                    if (groundAsset_.loaded && sceneDefinition_ != nullptr) {
                        std::size_t staticInstance = 4;
                        for (const scene::Entity& entity : sceneDefinition_->entities) {
                            if (!entity.importAsStaticCube) {
                                continue;
                            }
                            RecordModelDraw(commandBuffer, static_cast<uint32_t>(index),
                                groundAsset_, viewProj_,
                                sizeof(CharacterInstanceData) * staticInstance, 1);
                            ++staticInstance;
                        }
                    }
                } else {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                        0, 1, &descriptorSets_[index], 0, nullptr);
                    const VkDeviceSize cubeVertexOffset = 0;
                    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &cubeVertexBuffer_.buffer, &cubeVertexOffset);
                    vkCmdDraw(commandBuffer, kCubeVertexCount, 1, 0, 0);
                }
            }
        }

        vkCmdEndRenderPass(commandBuffer);

        // GLES bloom chain: threshold at 1/2 resolution, five dual-kernel
        // downsample levels, then five fused dual-kernel upsample levels.
        // The chain runs even when the setting is off so the sampled target is
        // always initialized; the final strength controls whether it affects
        // the image.
        VkClearValue bloomClear{};
        bloomClear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo bloomPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bloomPassInfo.renderPass = bloomRenderPass_;
        bloomPassInfo.clearValueCount = 1;
        bloomPassInfo.pClearValues = &bloomClear;
        bloomPassInfo.renderArea.offset = {0, 0};
        bloomPassInfo.framebuffer = bloomDsFramebuffers_[0];
        bloomPassInfo.renderArea.extent = {bloomDsWidths_[0], bloomDsHeights_[0]};
        vkCmdBeginRenderPass(commandBuffer, &bloomPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport bloomViewport{};
        bloomViewport.width = static_cast<float>(bloomDsWidths_[0]);
        bloomViewport.height = static_cast<float>(bloomDsHeights_[0]);
        bloomViewport.maxDepth = 1.0f;
        VkRect2D bloomScissor{};
        bloomScissor.extent = bloomPassInfo.renderArea.extent;
        vkCmdSetViewport(commandBuffer, 0, 1, &bloomViewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &bloomScissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomThresholdPipeline_);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
            2, 1, &bloomThresholdDescriptorSet_, 0, nullptr);
        const PostProcessPushConstants thresholdConstants = {1.0f, 0.5f, 0.0f, 0.0f};
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(thresholdConstants), &thresholdConstants);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer);

        for (int level = 1; level < kBloomDsLevels; ++level) {
            bloomPassInfo.framebuffer = bloomDsFramebuffers_[level];
            bloomPassInfo.renderArea.extent = {bloomDsWidths_[level], bloomDsHeights_[level]};
            bloomViewport.width = static_cast<float>(bloomDsWidths_[level]);
            bloomViewport.height = static_cast<float>(bloomDsHeights_[level]);
            bloomScissor.extent = bloomPassInfo.renderArea.extent;
            vkCmdBeginRenderPass(commandBuffer, &bloomPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(commandBuffer, 0, 1, &bloomViewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &bloomScissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomDownPipeline_);
            const VkDescriptorSet downSet = bloomDownDescriptorSets_[level - 1];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                2, 1, &downSet, 0, nullptr);
            const PostProcessPushConstants downConstants = {};
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                sizeof(downConstants), &downConstants);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(commandBuffer);
        }

        for (int level = kBloomUpLevels - 1; level >= 0; --level) {
            bloomPassInfo.framebuffer = bloomUpFramebuffers_[level];
            bloomPassInfo.renderArea.extent = {bloomDsWidths_[level], bloomDsHeights_[level]};
            bloomViewport.width = static_cast<float>(bloomDsWidths_[level]);
            bloomViewport.height = static_cast<float>(bloomDsHeights_[level]);
            bloomScissor.extent = bloomPassInfo.renderArea.extent;
            vkCmdBeginRenderPass(commandBuffer, &bloomPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(commandBuffer, 0, 1, &bloomViewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &bloomScissor);
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomUpPipeline_);
            const VkDescriptorSet upSet = bloomUpDescriptorSets_[level];
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                2, 1, &upSet, 0, nullptr);
            const PostProcessPushConstants upConstants = {};
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                sizeof(upConstants), &upConstants);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(commandBuffer);
        }

        VkClearValue postClear{};
        postClear.color.float32[0] = 0.02f;
        postClear.color.float32[1] = 0.04f;
        postClear.color.float32[2] = 0.08f;
        postClear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo postRenderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        postRenderPassInfo.renderPass = postRenderPass_;
        postRenderPassInfo.framebuffer = postFramebuffers_[index];
        postRenderPassInfo.renderArea.extent = swapchainExtent_;
        postRenderPassInfo.clearValueCount = 1;
        postRenderPassInfo.pClearValues = &postClear;
        vkCmdBeginRenderPass(commandBuffer, &postRenderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent_.width);
        viewport.height = static_cast<float>(swapchainExtent_.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = swapchainExtent_;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline_);
        const VkDescriptorSet postSet = postDescriptorSets_[index];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
            2, 1, &postSet, 0, nullptr);
        const PostProcessPushConstants postConstants = {
            1.3f, bloomEnabled_ ? 0.1f : 0.0f,
            // Texel size of the SCENE target (the sampled source), not the
            // swapchain: the FXAA-style resolve offsets sample sceneTexture,
            // which lives at the scaled offscreen resolution.
            1.0f / static_cast<float>(sceneExtent.width),
            1.0f / static_cast<float>(sceneExtent.height)};
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(postConstants), &postConstants);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

        const UiPushConstants pushConstants = {static_cast<float>(swapchainExtent_.width),
            static_cast<float>(swapchainExtent_.height), 0.0f, 0.0f};
        const VkDeviceSize overlayOffset = 0;
        if (overlayCount > 0) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffers_[index].buffer, &overlayOffset);
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                sizeof(pushConstants), &pushConstants);
            vkCmdDraw(commandBuffer, overlayCount, 1, 0, 0);
        }
        if (iconCount > 0 && iconPipeline_ != VK_NULL_HANDLE) {
            const VkDeviceSize iconOffset = kUiIconOffset;
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, iconPipeline_);
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffers_[index].buffer, &iconOffset);
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                sizeof(pushConstants), &pushConstants);
            const uint32_t iconCountToDraw = std::min<uint32_t>(
                iconCount / 6U, rhi::CameraInput::kActionButtonCount);
            for (uint32_t iconIndex = 0; iconIndex < iconCountToDraw; ++iconIndex) {
                if (actionIconDescriptorSets_[iconIndex] == VK_NULL_HANDLE) {
                    continue;
                }
                const VkDescriptorSet iconSets[] = {descriptorSets_[index],
                    actionIconDescriptorSets_[iconIndex]};
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout_, 0, 2, iconSets, 0, nullptr);
                vkCmdDraw(commandBuffer, 6, 1, iconIndex * 6, 0);
            }
        }
        if (textCount > 0 && overlayDescriptorSet_ != VK_NULL_HANDLE) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, textPipeline_);
            const VkDeviceSize textOffset = kUiTextOffset;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffers_[index].buffer, &textOffset);
            const VkDescriptorSet textSets[] = {descriptorSets_[index], overlayDescriptorSet_};
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                0, 2, textSets, 0, nullptr);
            vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                sizeof(pushConstants), &pushConstants);
            vkCmdDraw(commandBuffer, textCount, 1, 0, 0);
        }
        vkCmdEndRenderPass(commandBuffer);
        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) {
            LogVkError("end_command_buffer", result);
            return false;
        }
        return true;
    }

    bool CreateFramebuffersAndCommands()
    {
        if (commandPool_ == VK_NULL_HANDLE && !CreateCommandPool()) {
            return false;
        }
        if (!CreateShadowTarget()) {
            SDL_Log("SDL3_VULKAN stage=shadow_unavailable reason=target_creation_failed");
        }
        if (!CreateImage(SceneExtent().width, SceneExtent().height, 1, sceneColorFormat_,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
            sceneColorTarget_.image, sceneColorTarget_.memory)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=post_target_image_failed");
            return false;
        }
        sceneColorTarget_.layerCount = 1;
        if (!CreateImageView(sceneColorTarget_.image, sceneColorFormat_, VK_IMAGE_VIEW_TYPE_2D, 1,
            VK_IMAGE_ASPECT_COLOR_BIT, &sceneColorTarget_.view)) {
            DestroyTexture(sceneColorTarget_);
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=post_target_view_failed");
            return false;
        }
        const auto createBloomTexture = [this](TextureResource& texture, uint32_t width,
            uint32_t height) {
            if (!CreateImage(width, height, 1, sceneColorFormat_,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                texture.image, texture.memory)) {
                return false;
            }
            texture.layerCount = 1;
            texture.mipLevels = 1;
            if (!CreateImageView(texture.image, sceneColorFormat_, VK_IMAGE_VIEW_TYPE_2D, 1,
                VK_IMAGE_ASPECT_COLOR_BIT, &texture.view)) {
                DestroyTexture(texture);
                return false;
            }
            return true;
        };
        const VkExtent2D sceneExtent = SceneExtent();
        uint32_t bloomWidth = std::max(sceneExtent.width / 2U, 1U);
        uint32_t bloomHeight = std::max(sceneExtent.height / 2U, 1U);
        for (int level = 0; level < kBloomDsLevels; ++level) {
            bloomDsWidths_[level] = bloomWidth;
            bloomDsHeights_[level] = bloomHeight;
            if (!createBloomTexture(bloomDs_[level], bloomWidth, bloomHeight)) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=bloom_down_target_failed level=%{public}d",
                    level);
                return false;
            }
            bloomWidth = std::max(bloomWidth / 2U, 1U);
            bloomHeight = std::max(bloomHeight / 2U, 1U);
        }
        for (int level = 0; level < kBloomUpLevels; ++level) {
            if (!createBloomTexture(bloomUp_[level], bloomDsWidths_[level], bloomDsHeights_[level])) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=bloom_up_target_failed level=%{public}d",
                    level);
                return false;
            }
        }
        for (int level = 0; level < kBloomDsLevels; ++level) {
            VkImageView attachments[] = {bloomDs_[level].view};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = bloomRenderPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = bloomDsWidths_[level];
            framebufferInfo.height = bloomDsHeights_[level];
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            const VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer);
            if (result != VK_SUCCESS) {
                LogVkError("create_bloom_down_framebuffer", result);
                return false;
            }
            bloomDsFramebuffers_.push_back(framebuffer);
        }
        for (int level = 0; level < kBloomUpLevels; ++level) {
            VkImageView attachments[] = {bloomUp_[level].view};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = bloomRenderPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = bloomDsWidths_[level];
            framebufferInfo.height = bloomDsHeights_[level];
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            const VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer);
            if (result != VK_SUCCESS) {
                LogVkError("create_bloom_up_framebuffer", result);
                return false;
            }
            bloomUpFramebuffers_.push_back(framebuffer);
        }
        SDL_Log("SDL3_VULKAN stage=bloom_targets_ready scene=%ux%u levels=%d+%d",
            sceneExtent.width, sceneExtent.height, kBloomDsLevels, kBloomUpLevels);
        for (VkImageView imageView : imageViews_) {
            VkImageView attachments[] = {sceneColorTarget_.view, depthImageView_};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass_;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = attachments;
            // Scene pass renders into the scaled offscreen targets; the
            // post pass below still writes each swapchain image full size.
            framebufferInfo.width = sceneExtent.width;
            framebufferInfo.height = sceneExtent.height;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            const VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer);
            if (result != VK_SUCCESS) {
                LogVkError("create_framebuffer", result);
                return false;
            }
            framebuffers_.push_back(framebuffer);

            VkImageView postAttachments[] = {imageView};
            VkFramebufferCreateInfo postFramebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            postFramebufferInfo.renderPass = postRenderPass_;
            postFramebufferInfo.attachmentCount = 1;
            postFramebufferInfo.pAttachments = postAttachments;
            postFramebufferInfo.width = swapchainExtent_.width;
            postFramebufferInfo.height = swapchainExtent_.height;
            postFramebufferInfo.layers = 1;
            VkFramebuffer postFramebuffer = VK_NULL_HANDLE;
            const VkResult postResult = vkCreateFramebuffer(device_, &postFramebufferInfo, nullptr,
                &postFramebuffer);
            if (postResult != VK_SUCCESS) {
                LogVkError("create_post_framebuffer", postResult);
                return false;
            }
            postFramebuffers_.push_back(postFramebuffer);
        }
        commandBuffers_.resize(framebuffers_.size());
        VkCommandBufferAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocationInfo.commandPool = commandPool_;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
        VkResult result = vkAllocateCommandBuffers(device_, &allocationInfo, commandBuffers_.data());
        if (result != VK_SUCCESS) {
            LogVkError("allocate_command_buffers", result);
            return false;
        }
        // One UBO + descriptor set per swapchain image; the recorded command
        // buffer for an image reads its own UBO, so a frame can be prepared
        // while another image is still in flight.
        uniformBuffers_.resize(framebuffers_.size());
        descriptorSets_.resize(framebuffers_.size());
        for (BufferResource& buffer : uniformBuffers_) {
            if (!CreateBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer)) {
                return false;
            }
        }
        uiVertexBuffers_.resize(framebuffers_.size());
        for (BufferResource& buffer : uiVertexBuffers_) {
            if (!CreateBuffer(kUiVertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer)) {
                return false;
            }
        }
        if (!CreateImageDescriptorSets()) {
            return false;
        }
        // The model's material sets bind the same per-image uniform buffers,
        // so they have to be (re)built every time the pool is reset.
        if (!CreateModelDescriptorSets()) {
            return false;
        }
        if (!CreateOverlayDescriptorSet()) {
            return false;
        }
        if (!CreateBloomDescriptorSets() || !CreatePostDescriptorSets()) {
            return false;
        }
        for (size_t index = 0; index < commandBuffers_.size(); ++index) {
            if (!RecordFrameCommands(index, 0, 0, 0)) {
                return false;
            }
        }
        return true;
    }

    static void SlerpAnimationQuaternion(const float a[4], const float b[4], float factor,
        float out[4])
    {
        float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        float bx = b[0];
        float by = b[1];
        float bz = b[2];
        float bw = b[3];
        if (dot < 0.0f) {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }
        if (dot > 0.9995f) {
            out[0] = a[0] + (bx - a[0]) * factor;
            out[1] = a[1] + (by - a[1]) * factor;
            out[2] = a[2] + (bz - a[2]) * factor;
            out[3] = a[3] + (bw - a[3]) * factor;
            const float length = std::sqrt(out[0] * out[0] + out[1] * out[1]
                + out[2] * out[2] + out[3] * out[3]);
            if (length > 1.0e-8f) {
                for (int component = 0; component < 4; ++component) {
                    out[component] /= length;
                }
            }
            return;
        }
        const float theta = std::acos(std::min(std::max(dot, -1.0f), 1.0f));
        const float sine = std::sin(theta);
        if (std::abs(sine) < 1.0e-6f) {
            std::memcpy(out, a, sizeof(float) * 4);
            return;
        }
        const float first = std::sin((1.0f - factor) * theta) / sine;
        const float second = std::sin(factor * theta) / sine;
        out[0] = a[0] * first + bx * second;
        out[1] = a[1] * first + by * second;
        out[2] = a[2] * first + bz * second;
        out[3] = a[3] * first + bw * second;
    }

    static void SampleAnimationChannel(const ohos_model::AnimationChannel& channel, float time,
        float* output)
    {
        const size_t frameCount = channel.times.size();
        const size_t componentCount = channel.path == 1 ? 4u : 3u;
        if (frameCount == 0 || channel.values.size() < componentCount) {
            output[0] = 0.0f;
            output[1] = 0.0f;
            output[2] = 0.0f;
            output[3] = 1.0f;
            return;
        }
        auto copyFrame = [&](size_t frame) {
            const size_t base = frame * componentCount;
            for (size_t component = 0; component < componentCount; ++component) {
                output[component] = channel.values[base + component];
            }
        };
        if (time <= channel.times.front()) {
            copyFrame(0);
            return;
        }
        if (time >= channel.times.back()) {
            copyFrame(frameCount - 1);
            return;
        }
        size_t frame = 0;
        while (frame + 1 < frameCount && channel.times[frame + 1] <= time) {
            ++frame;
        }
        if (channel.interpolation == 1 || channel.times[frame + 1] <= channel.times[frame]) {
            copyFrame(frame);
            return;
        }
        const float factor = (time - channel.times[frame]) /
            (channel.times[frame + 1] - channel.times[frame]);
        const size_t current = frame * componentCount;
        const size_t next = (frame + 1) * componentCount;
        if (componentCount == 4) {
            SlerpAnimationQuaternion(&channel.values[current], &channel.values[next], factor, output);
        } else {
            for (size_t component = 0; component < componentCount; ++component) {
                output[component] = channel.values[current + component]
                    + (channel.values[next + component] - channel.values[current + component]) * factor;
            }
        }
    }

    static const char* AnimationStateName(ModelAnimationState state)
    {
        switch (state) {
        case ModelAnimationState::Idle: return "IDLE";
        case ModelAnimationState::Walk: return "WALK";
        case ModelAnimationState::Sprint: return "RUN";
        case ModelAnimationState::Attack: return "ATTACK";
        case ModelAnimationState::JumpStart:
        case ModelAnimationState::JumpLoop: return "JUMP";
        case ModelAnimationState::JumpLand: return "LAND";
        case ModelAnimationState::Crouch: return "CROUCH";
        case ModelAnimationState::Hit: return "HIT";
        case ModelAnimationState::Death: return "DEAD";
        }
        return "IDLE";
    }

    ModelAnimationState GroundedPlayerAnimationState() const
    {
        if (crouchEnabled_) {
            return ModelAnimationState::Crouch;
        }
        if (!playerMoving_) {
            return ModelAnimationState::Idle;
        }
        return playerSprinting_ ? ModelAnimationState::Sprint : ModelAnimationState::Walk;
    }

    static float AnimationClipDurationOr(const ModelGpuAsset& asset, int clipIndex, float fallback)
    {
        return clipIndex >= 0 && clipIndex < static_cast<int>(asset.animationClips.size()) &&
            asset.animationClips[clipIndex].duration > 0.0f
            ? asset.animationClips[clipIndex].duration : fallback;
    }

    void EnterPlayerAnimationState(ModelAnimationState next)
    {
        ModelGpuAsset& asset = playerAsset_;
        if (asset.animationState == next) {
            return;
        }
        int targetClip = asset.walkClip;
        bool targetLoop = true;
        switch (next) {
        case ModelAnimationState::Idle:
            targetClip = asset.idleClip; targetLoop = true; break;
        case ModelAnimationState::Walk:
            targetClip = asset.walkClip; targetLoop = true; break;
        case ModelAnimationState::Sprint:
            targetClip = asset.sprintClip; targetLoop = true; break;
        case ModelAnimationState::Crouch:
            targetClip = asset.crouchClip; targetLoop = true; break;
        case ModelAnimationState::Attack:
            targetClip = asset.attackClip; targetLoop = false; break;
        case ModelAnimationState::JumpStart:
            targetClip = asset.jumpStartClip; targetLoop = false; break;
        case ModelAnimationState::JumpLoop:
            targetClip = asset.jumpLoopClip; targetLoop = true; break;
        case ModelAnimationState::JumpLand:
            targetClip = asset.jumpLandClip; targetLoop = false; break;
        case ModelAnimationState::Death:
            targetClip = asset.deathClip; targetLoop = false; break;
        case ModelAnimationState::Hit:
            targetClip = asset.hitClip; targetLoop = false; break;
        }
        if (targetClip < 0) {
            // Match GLES' missing-clip fallback: retain a valid grounded loop
            // instead of trying to sample an invalid animation index.
            next = ModelAnimationState::Walk;
            targetClip = asset.walkClip;
            targetLoop = true;
        }
        asset.animationState = next;
        asset.animationStateTime = 0.0f;
        if (asset.animationClipIndex != targetClip) {
            asset.animationClipIndex = targetClip;
            asset.animationTime = 0.0f;
        }
        asset.animationLoop = targetLoop;
        if (targetClip >= 0 && targetClip < static_cast<int>(asset.animationClips.size())) {
            SDL_Log("SDL3_VULKAN stage=animation_state state=%s clip=%s loop=%d",
                AnimationStateName(asset.animationState),
                asset.animationClips[targetClip].name.c_str(), targetLoop ? 1 : 0);
        }
    }

    void UpdatePlayerAnimationStateMachine(float dt)
    {
        ModelGpuAsset& asset = playerAsset_;
        asset.animationStateTime += dt;

        // Consume action edges here, after UpdateSceneUniforms has computed
        // movement/sprint state.  Clearing them in input consumption made the
        // Vulkan path silently ignore all three one-shot/toggle actions.
        const int pressed = cameraInput_.actionButtonPressedMask;
        cameraInput_.actionButtonPressedMask = 0;
        // Mikan hit-stun: while the control lock is active the Hit one-shot
        // overrides everything and all action edges are swallowed.
        if (playerHitLockTimer_ > 0.0f) {
            if (asset.animationState != ModelAnimationState::Hit) {
                EnterPlayerAnimationState(ModelAnimationState::Hit);
            }
            return;
        }
        // UpdateEnemyAi admits the attack edge only when the shared combat
        // cooldown is ready.  Reuse that decision here so the animation and
        // the hit query cannot disagree when the button is tapped rapidly.
        if (playerAttackAcceptedThisFrame_) {
            EnterPlayerAnimationState(ModelAnimationState::Attack);
        }
        if ((pressed & 2) != 0 && asset.animationState == GroundedPlayerAnimationState()) {
            EnterPlayerAnimationState(ModelAnimationState::JumpStart);
        }
        if ((pressed & 8) != 0) {
            crouchEnabled_ = !crouchEnabled_;
            if (asset.animationState == ModelAnimationState::Walk ||
                asset.animationState == ModelAnimationState::Idle ||
                asset.animationState == ModelAnimationState::Crouch) {
                EnterPlayerAnimationState(GroundedPlayerAnimationState());
            }
        }

        if (asset.animationState == ModelAnimationState::Walk ||
            asset.animationState == ModelAnimationState::Idle ||
            asset.animationState == ModelAnimationState::Sprint) {
            const ModelAnimationState grounded = GroundedPlayerAnimationState();
            if (asset.animationState != grounded) {
                EnterPlayerAnimationState(grounded);
            }
        }

        switch (asset.animationState) {
        case ModelAnimationState::Attack:
            if (asset.animationStateTime >= AnimationClipDurationOr(asset, asset.attackClip, 0.87f)) {
                EnterPlayerAnimationState(GroundedPlayerAnimationState());
            }
            break;
        case ModelAnimationState::JumpStart:
            if (asset.animationStateTime >= 0.35f) {
                EnterPlayerAnimationState(ModelAnimationState::JumpLoop);
            }
            break;
        case ModelAnimationState::JumpLoop:
            if (asset.animationStateTime >= 0.5f) {
                EnterPlayerAnimationState(ModelAnimationState::JumpLand);
            }
            break;
        case ModelAnimationState::JumpLand:
            if (asset.animationStateTime >= 0.45f) {
                EnterPlayerAnimationState(GroundedPlayerAnimationState());
            }
            break;
        case ModelAnimationState::Idle:
        case ModelAnimationState::Walk:
        case ModelAnimationState::Sprint:
        case ModelAnimationState::Crouch:
            break;
        case ModelAnimationState::Hit:
            // Lock expired this frame: snap back to the grounded loop.
            if (playerHitLockTimer_ <= 0.0f) {
                EnterPlayerAnimationState(GroundedPlayerAnimationState());
            }
            break;
        case ModelAnimationState::Death:
            break;
        }
    }

    void UpdatePlayerAnimation()
    {
        ModelGpuAsset& asset = playerAsset_;
        if (!asset.loaded || !asset.skinned || asset.jointBuffer.memory == VK_NULL_HANDLE ||
            asset.animationNodes.empty() || asset.animationSkin.JointCount() == 0) {
            return;
        }
        if (asset.animationClips.empty()) {
            return;
        }

        const Uint64 nowTicks = SDL_GetTicks();
        float dt = 0.0f;
        if (asset.animationLastTicks != 0) {
            const float elapsed = static_cast<float>(nowTicks - asset.animationLastTicks) * 0.001f;
            dt = std::max(0.0f, std::min(elapsed, 0.1f));
        }
        asset.animationLastTicks = nowTicks;
        UpdatePlayerAnimationStateMachine(dt);
        if (asset.animationClipIndex < 0 ||
            asset.animationClipIndex >= static_cast<int>(asset.animationClips.size())) {
            return;
        }
        const ohos_model::AnimationClip& clip = asset.animationClips[asset.animationClipIndex];
        if (clip.duration > 0.0f) {
            if (asset.animationLoop) {
                asset.animationTime = std::fmod(asset.animationTime + dt, clip.duration);
            } else {
                asset.animationTime = std::min(asset.animationTime + dt, clip.duration);
            }
        }

        const size_t nodeCount = asset.animationNodes.size();
        std::vector<float> translations(nodeCount * 3, 0.0f);
        std::vector<float> rotations(nodeCount * 4, 0.0f);
        std::vector<float> scales(nodeCount * 3, 1.0f);
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const ohos_model::Node& node = asset.animationNodes[nodeIndex];
            for (int component = 0; component < 3; ++component) {
                translations[nodeIndex * 3 + component] = node.translation[component];
                scales[nodeIndex * 3 + component] = node.scale[component];
            }
            for (int component = 0; component < 4; ++component) {
                rotations[nodeIndex * 4 + component] = node.rotation[component];
            }
        }
        float sampled[4]{};
        for (const ohos_model::AnimationChannel& channel : clip.channels) {
            if (channel.nodeIndex >= nodeCount) {
                continue;
            }
            SampleAnimationChannel(channel, asset.animationTime, sampled);
            if (channel.path == 0) {
                std::memcpy(&translations[channel.nodeIndex * 3], sampled, sizeof(float) * 3);
            } else if (channel.path == 1) {
                std::memcpy(&rotations[channel.nodeIndex * 4], sampled, sizeof(float) * 4);
            } else if (channel.path == 2) {
                std::memcpy(&scales[channel.nodeIndex * 3], sampled, sizeof(float) * 3);
            }
        }

        std::vector<Mat4> globals(nodeCount, Mat4Identity());
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const ohos_model::Node& node = asset.animationNodes[nodeIndex];
            const Mat4 local = Mat4ComposeTrs(&translations[nodeIndex * 3],
                &rotations[nodeIndex * 4], &scales[nodeIndex * 3]);
            if (node.parent >= 0 && static_cast<size_t>(node.parent) < nodeIndex) {
                globals[nodeIndex] = Mat4Multiply(globals[static_cast<size_t>(node.parent)], local);
            } else {
                globals[nodeIndex] = local;
            }
        }

        asset.jointMatrices.assign(kMaxJoints, Mat4Identity());
        const size_t jointCount = std::min(asset.animationSkin.JointCount(), kMaxJoints);
        for (size_t joint = 0; joint < jointCount; ++joint) {
            const uint32_t nodeIndex = asset.animationSkin.jointNodes[joint];
            if (nodeIndex >= globals.size() ||
                asset.animationSkin.inverseBindMatrices.size() < (joint + 1) * 16) {
                continue;
            }
            const Mat4 inverseBind = Mat4FromArray(
                &asset.animationSkin.inverseBindMatrices[joint * 16]);
            asset.jointMatrices[joint] = Mat4Multiply(asset.animationNormalise,
                Mat4Multiply(globals[nodeIndex], inverseBind));
        }
        if (!UploadToMappedBuffer(asset.jointBuffer, asset.jointMatrices.data(),
                sizeof(Mat4) * kMaxJoints)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_MODEL stage=animation_upload_failed clip=%d", asset.animationClipIndex);
            return;
        }
        if (!asset.animationLogged) {
            SDL_Log("SDL3_VULKAN stage=animation_running clip=%s joints=%u",
                clip.name.c_str(), static_cast<unsigned int>(jointCount));
            asset.animationLogged = true;
        }
    }

    ModelAnimationState GroundedEnemyAnimationState() const
    {
        return enemyMoving_ ? ModelAnimationState::Walk : ModelAnimationState::Idle;
    }

    void EnterEnemyAnimationState(ModelAnimationState next)
    {
        ModelGpuAsset& asset = enemyAsset_;
        if (asset.animationState == next) {
            return;
        }
        int targetClip = asset.walkClip;
        bool targetLoop = true;
        switch (next) {
        case ModelAnimationState::Idle:
            targetClip = asset.idleClip; targetLoop = true; break;
        case ModelAnimationState::Walk:
            targetClip = asset.walkClip; targetLoop = true; break;
        case ModelAnimationState::Sprint:
            targetClip = asset.sprintClip; targetLoop = true; break;
        case ModelAnimationState::Crouch:
            targetClip = asset.crouchClip; targetLoop = true; break;
        case ModelAnimationState::Attack:
            targetClip = asset.attackClip; targetLoop = false; break;
        case ModelAnimationState::JumpStart:
            targetClip = asset.jumpStartClip; targetLoop = false; break;
        case ModelAnimationState::JumpLoop:
            targetClip = asset.jumpLoopClip; targetLoop = true; break;
        case ModelAnimationState::JumpLand:
            targetClip = asset.jumpLandClip; targetLoop = false; break;
        case ModelAnimationState::Death:
            targetClip = asset.deathClip; targetLoop = false; break;
        case ModelAnimationState::Hit:
            targetClip = asset.hitClip; targetLoop = false; break;
        }
        if (targetClip < 0) {
            next = ModelAnimationState::Walk;
            targetClip = asset.walkClip >= 0 ? asset.walkClip : asset.idleClip;
            targetLoop = true;
        }
        asset.animationState = next;
        asset.animationStateTime = 0.0f;
        if (asset.animationClipIndex != targetClip) {
            asset.animationClipIndex = targetClip;
            asset.animationTime = 0.0f;
        }
        asset.animationLoop = targetLoop;
        if (targetClip >= 0 && targetClip < static_cast<int>(asset.animationClips.size())) {
            SDL_Log("SDL3_VULKAN stage=enemy_animation_state state=%s clip=%s loop=%d",
                AnimationStateName(asset.animationState),
                asset.animationClips[targetClip].name.c_str(), targetLoop ? 1 : 0);
        }
    }

    void UpdateEnemyAnimationStateMachine(float dt)
    {
        ModelGpuAsset& asset = enemyAsset_;
        asset.animationStateTime += dt;
        if (enemyDeathPlaying_) {
            if (asset.animationState != ModelAnimationState::Death) {
                EnterEnemyAnimationState(ModelAnimationState::Death);
            }
            return;
        }
        // Mikan PuppetEnemyScript: hit-stun holds the Hit one-shot and the
        // AI cannot override it until the lock expires.
        if (enemyHitLockTimer_ > 0.0f) {
            if (asset.animationState != ModelAnimationState::Hit) {
                EnterEnemyAnimationState(ModelAnimationState::Hit);
            }
            return;
        }
        if (asset.animationState == ModelAnimationState::Hit) {
            // Lock expired this frame: snap back to the grounded loop.
            EnterEnemyAnimationState(GroundedEnemyAnimationState());
        }
        if (enemyAttackRequested_) {
            EnterEnemyAnimationState(ModelAnimationState::Attack);
            enemyAttackRequested_ = false;
        }
        if (asset.animationState == ModelAnimationState::Walk ||
            asset.animationState == ModelAnimationState::Idle) {
            const ModelAnimationState grounded = GroundedEnemyAnimationState();
            if (asset.animationState != grounded) {
                EnterEnemyAnimationState(grounded);
            }
        }
        if (asset.animationState == ModelAnimationState::Attack &&
            asset.animationStateTime >= AnimationClipDurationOr(asset, asset.attackClip, 0.87f)) {
            EnterEnemyAnimationState(GroundedEnemyAnimationState());
        }
    }

    void UpdateEnemyAnimation()
    {
        ModelGpuAsset& asset = enemyAsset_;
        if (!enemyVisible_ || !asset.loaded || !asset.skinned ||
            asset.jointBuffer.memory == VK_NULL_HANDLE || asset.animationNodes.empty() ||
            asset.animationSkin.JointCount() == 0 || asset.animationClips.empty()) {
            return;
        }
        const Uint64 nowTicks = SDL_GetTicks();
        float dt = 0.0f;
        if (asset.animationLastTicks != 0) {
            const float elapsed = static_cast<float>(nowTicks - asset.animationLastTicks) * 0.001f;
            dt = std::max(0.0f, std::min(elapsed, 0.1f));
        }
        asset.animationLastTicks = nowTicks;
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
                SDL_Log("SDL3_VULKAN stage=enemy_dissolved");
            }
            return;
        }
        if (enemyDeathPlaying_) {
            enemyDeathTime_ += dt;
            const float deathClipDuration = AnimationClipDurationOr(asset, asset.deathClip, 1.35f);
            // Keep the final death pose visible long enough to read; the
            // source Death01 clip itself is not removed or shortened.
            const float deathVisibleDuration = std::max(2.20f, deathClipDuration + 0.85f);
            if (enemyDeathTime_ >= deathVisibleDuration) {
                enemyDeathPlaying_ = false;
                // Hand the corpse to the dissolve phase instead of popping it
                // out of the scene; enemyVisible_ stays true while burning.
                enemyDissolveTimer_ = 0.0f;
                enemyDissolveAmount_ = 0.0f;
                SDL_Log("SDL3_VULKAN stage=enemy_dissolve_start death_duration=%.2f",
                    enemyDeathTime_);
                return;
            }
        }
        UpdateEnemyAnimationStateMachine(dt);
        if (asset.animationClipIndex < 0 ||
            asset.animationClipIndex >= static_cast<int>(asset.animationClips.size())) {
            return;
        }
        const ohos_model::AnimationClip& clip = asset.animationClips[asset.animationClipIndex];
        if (clip.duration > 0.0f) {
            if (asset.animationLoop) {
                asset.animationTime = std::fmod(asset.animationTime + dt, clip.duration);
            } else {
                asset.animationTime = std::min(asset.animationTime + dt, clip.duration);
            }
        }

        const size_t nodeCount = asset.animationNodes.size();
        std::vector<float> translations(nodeCount * 3, 0.0f);
        std::vector<float> rotations(nodeCount * 4, 0.0f);
        std::vector<float> scales(nodeCount * 3, 1.0f);
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const ohos_model::Node& node = asset.animationNodes[nodeIndex];
            for (int component = 0; component < 3; ++component) {
                translations[nodeIndex * 3 + component] = node.translation[component];
                scales[nodeIndex * 3 + component] = node.scale[component];
            }
            for (int component = 0; component < 4; ++component) {
                rotations[nodeIndex * 4 + component] = node.rotation[component];
            }
        }
        float sampled[4]{};
        for (const ohos_model::AnimationChannel& channel : clip.channels) {
            if (channel.nodeIndex >= nodeCount) {
                continue;
            }
            SampleAnimationChannel(channel, asset.animationTime, sampled);
            if (channel.path == 0) {
                std::memcpy(&translations[channel.nodeIndex * 3], sampled, sizeof(float) * 3);
            } else if (channel.path == 1) {
                std::memcpy(&rotations[channel.nodeIndex * 4], sampled, sizeof(float) * 4);
            } else if (channel.path == 2) {
                std::memcpy(&scales[channel.nodeIndex * 3], sampled, sizeof(float) * 3);
            }
        }

        std::vector<Mat4> globals(nodeCount, Mat4Identity());
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const ohos_model::Node& node = asset.animationNodes[nodeIndex];
            const Mat4 local = Mat4ComposeTrs(&translations[nodeIndex * 3],
                &rotations[nodeIndex * 4], &scales[nodeIndex * 3]);
            if (node.parent >= 0 && static_cast<size_t>(node.parent) < nodeIndex) {
                globals[nodeIndex] = Mat4Multiply(globals[static_cast<size_t>(node.parent)], local);
            } else {
                globals[nodeIndex] = local;
            }
        }

        asset.jointMatrices.assign(kMaxJoints, Mat4Identity());
        const size_t jointCount = std::min(asset.animationSkin.JointCount(), kMaxJoints);
        for (size_t joint = 0; joint < jointCount; ++joint) {
            const uint32_t nodeIndex = asset.animationSkin.jointNodes[joint];
            if (nodeIndex >= globals.size() ||
                asset.animationSkin.inverseBindMatrices.size() < (joint + 1) * 16) {
                continue;
            }
            const Mat4 inverseBind = Mat4FromArray(
                &asset.animationSkin.inverseBindMatrices[joint * 16]);
            asset.jointMatrices[joint] = Mat4Multiply(asset.animationNormalise,
                Mat4Multiply(globals[nodeIndex], inverseBind));
        }
        if (!UploadToMappedBuffer(asset.jointBuffer, asset.jointMatrices.data(),
                sizeof(Mat4) * kMaxJoints)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_MODEL stage=enemy_animation_upload_failed clip=%d",
                asset.animationClipIndex);
        }
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
        TickCombatTimer(playerAttackInvulnerability_);
        TickCombatTimer(playerHitInvulnerability_);
        TickCombatTimer(enemyHitInvulnerability_);
        TickCombatTimer(enemyHitLockTimer_);
        playerAttackAcceptedThisFrame_ = false;
        enemyDesiredVelocityX_ = 0.0f;
        enemyDesiredVelocityZ_ = 0.0f;

        // Keep attack cadence tied to the clip while enforcing a floor for
        // reduced or malformed assets.  The accepted edge drives both the
        // animation and the damage query below.
        if ((pressed & 1) != 0 && playerAttackCooldown_ <= 0.0f &&
            playerHitLockTimer_ <= 0.0f && playerHealth_ > 0.0f) {
            const float clipDuration = AnimationClipDurationOr(
                playerAsset_, playerAsset_.attackClip, 0.87f);
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
                SDL_Log("SDL3_VULKAN stage=enemy_defeated");
            } else {
                enemyHitLockTimer_ = kPlayerHitDuration;
                SDL_Log("SDL3_VULKAN stage=enemy_hit health=%.0f/%.0f",
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
            enemyAttackCooldown_ = 1.15f;
            enemyAttackRequested_ = true;
            if (playerAttackInvulnerability_ <= 0.0f &&
                playerHitInvulnerability_ <= 0.0f) {
                playerHealth_ = std::max(0.0f, playerHealth_ - 8.0f);
                AudioManager::GetInstance().PlayAudio("hit",
                    playerHealth_ <= 0.0f ? 0.95f : 0.70f);
                playerHitInvulnerability_ = kHitInvulnerability;
                playerHitLockTimer_ = kPlayerHitDuration;
                playerHitFlashTimer_ = kHitFlashDuration;
                SDL_Log("SDL3_VULKAN stage=enemy_attack player_health=%.0f/%.0f",
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
                    SDL_Log("SDL3_VULKAN stage=player_defeated screen=retry");
                    return;
                }
            } else {
                SDL_Log("SDL3_VULKAN stage=enemy_attack_blocked reason=player_iframe");
            }
        }
    }

    bool UpdateSceneUniforms(BufferResource& target)
    {
        if (target.memory == VK_NULL_HANDLE || cubeVertexBuffer_.memory == VK_NULL_HANDLE ||
            swapchainExtent_.height == 0) {
            return false;
        }
        const float aspect = static_cast<float>(swapchainExtent_.width) /
            static_cast<float>(swapchainExtent_.height);
        float fieldOfViewDegrees = 60.0f;
        float nearPlane = 0.1f;
        float farPlane = 100.0f;
        if (sceneDefinition_ != nullptr && sceneDefinition_->mainCamera.present) {
            fieldOfViewDegrees = sceneDefinition_->mainCamera.fieldOfViewDegrees;
            nearPlane = sceneDefinition_->mainCamera.nearPlane;
            farPlane = sceneDefinition_->mainCamera.farPlane;
        }
        const Mat4 projection = Mat4Perspective(
            fieldOfViewDegrees * 3.14159265358979323846f / 180.0f,
            aspect, nearPlane, farPlane);

        // Match GLES' third-person camera target and camera-relative joystick
        // movement.  Vulkan keeps the same Vulkan-depth projection, but the
        // view and object transforms are otherwise identical to the GL path.
        const bool gameplayInput = uiScreen_ == rhi::UiScreen::Gameplay;
        float dt = 0.016f;
        const Uint64 nowTicks = SDL_GetTicks();
        if (moveLastTicks_ != 0) {
            const float dtMs = static_cast<float>(nowTicks - moveLastTicks_);
            if (dtMs > 0.0f && dtMs < 100.0f) {
                dt = dtMs * 0.001f;
            }
        }
        moveLastTicks_ = nowTicks;
        if (uiScreen_ == rhi::UiScreen::MainMenu || uiScreen_ == rhi::UiScreen::Settings) {
            // Slow environment orbit, matching the GLES menu backdrops.
            // Starting gameplay resets the camera snapshot to its baseline.
            camYaw_ += dt * 0.12f;
        }
        const Mat4 camToWorld = Mat4Multiply(Mat4RotationY(-camYaw_), Mat4RotationX(-camPitch_));
        float stickMagnitude = gameplayInput ? std::sqrt(cameraInput_.moveX * cameraInput_.moveX
            + cameraInput_.moveY * cameraInput_.moveY) : 0.0f;
        if (stickMagnitude > 1.0f) {
            stickMagnitude = 1.0f;
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
        if (stickMagnitude > 0.15f && !playerLocked) {
            float forwardX = -camToWorld.value[8];
            float forwardZ = -camToWorld.value[10];
            const float forwardLength = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
            if (forwardLength > 1.0e-4f) {
                forwardX /= forwardLength;
                forwardZ /= forwardLength;
            }
            float directionX = camToWorld.value[0] * cameraInput_.moveX
                + forwardX * -cameraInput_.moveY;
            float directionZ = camToWorld.value[2] * cameraInput_.moveX
                + forwardZ * -cameraInput_.moveY;
            const float directionLength = std::sqrt(directionX * directionX + directionZ * directionZ);
            if (directionLength > 1.0e-4f) {
                directionX /= directionLength;
                directionZ /= directionLength;
                playerSprinting_ = cameraInput_.actionButtonHeld[2] && !crouchEnabled_;
                const float speed = playerSprinting_ ? 2.6f : 1.3f;
                desiredVelocityX = directionX * speed * stickMagnitude;
                desiredVelocityZ = directionZ * speed * stickMagnitude;
                hasMovementDirection = true;
                if (!physicsReady) {
                    playerX_ += desiredVelocityX * dt;
                    playerZ_ += desiredVelocityZ * dt;
                    constexpr float kGroundBound = 8.5f;
                    playerX_ = std::max(-kGroundBound, std::min(kGroundBound, playerX_));
                    playerZ_ = std::max(-kGroundBound, std::min(kGroundBound, playerZ_));
                    playerY_ = 0.0f;
                }
                const float targetYaw = std::atan2(directionX, directionZ);
                float yawDelta = targetYaw - playerYaw_;
                constexpr float kPi = 3.14159265358979323846f;
                while (yawDelta > kPi) {
                    yawDelta -= 2.0f * kPi;
                }
                while (yawDelta < -kPi) {
                    yawDelta += 2.0f * kPi;
                }
                const float maxTurn = 12.0f * dt;
                playerYaw_ += std::max(-maxTurn, std::min(maxTurn, yawDelta));
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
        playerMoving_ = stickMagnitude > 0.15f && !playerLocked;
        // Keep the action edge available for UpdatePlayerAnimationStateMachine;
        // the same edge is used here for an in-range enemy hit.
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
            const float followLerp = 1.0f - std::exp(-8.0f * dt);
            camPanX_ += (playerX_ - camPanX_) * followLerp;
            camPanY_ += (playerY_ - camPanY_) * followLerp;
            camPanZ_ += (playerZ_ - camPanZ_) * followLerp;
        }

        const Mat4 view = Mat4Multiply(Mat4Translation(0.0f, 0.0f, -camDistance_),
            Mat4Multiply(Mat4RotationX(camPitch_),
                Mat4Multiply(Mat4RotationY(camYaw_),
                    Mat4Translation(-camPanX_, -camPanY_, -camPanZ_))));
        // Keep the lighting camera in the same world space as GLES.  The
        // view matrix is target-relative, so recover the eye from the inverse
        // orbit rotation rather than approximating it from the target.
        camPosX_ = camPanX_ + camToWorld.value[8] * camDistance_ + camToWorld.value[12];
        camPosY_ = camPanY_ + camToWorld.value[9] * camDistance_ + camToWorld.value[13];
        camPosZ_ = camPanZ_ + camToWorld.value[10] * camDistance_ + camToWorld.value[14];
        viewProj_ = Mat4Multiply(projection, view);
        playerMatrix_ = Mat4Multiply(Mat4Translation(playerX_, playerY_, playerZ_),
            Mat4RotationY(playerYaw_));
        playerMvp_ = Mat4Multiply(viewProj_, playerMatrix_);
        enemyMatrix_ = Mat4Multiply(Mat4Translation(enemyX_, enemyY_, enemyZ_),
            Mat4RotationY(enemyYaw_));
        shadowMatrix_ = Mat4Multiply(Mat4ShadowProjection(), Mat4ShadowView());
        SceneUniforms uniforms{};
        uniforms.cubeMvp = playerMvp_;
        uniforms.skyMvp = projection;
        uniforms.modelMvp = playerMvp_;
        uniforms.modelMatrix = playerMatrix_;
        uniforms.cameraPosition[0] = camPosX_;
        uniforms.cameraPosition[1] = camPosY_;
        uniforms.cameraPosition[2] = camPosZ_;
        // cameraPosition.w carries the environment LOD range used by the
        // GLES-equivalent GGX prefilter chain (six 128px levels).  Fall back
        // to the raw skybox chain only if the optional CPU convolution could
        // not be allocated on a constrained device.
        const TextureResource& environmentTexture = prefilteredTexture_.view != VK_NULL_HANDLE
            ? prefilteredTexture_ : skyboxTexture_;
        uniforms.cameraPosition[3] = environmentTexture.mipLevels > 0
            ? static_cast<float>(environmentTexture.mipLevels - 1) : 0.0f;
        uniforms.cameraToWorld = camToWorld;
        uniforms.shadowMvp = shadowMatrix_;
        uniforms.viewProj = viewProj_;
        return UploadToMappedBuffer(target, &uniforms, sizeof(uniforms));
    }

    bool CreateInstance()
    {
        Uint32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        if (sdlExtensions == nullptr || sdlExtensionCount == 0) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=sdl_instance_extensions_failed error=%{public}s",
                SDL_GetError());
            return false;
        }
        uint32_t extensionCount = 0;
        VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        if (result != VK_SUCCESS) {
            LogVkError("enumerate_instance_extensions", result);
            return false;
        }
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount > 0) {
            result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
            if (result != VK_SUCCESS) {
                LogVkError("enumerate_instance_extensions", result);
                return false;
            }
        }
        std::vector<const char*> requiredExtensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
        for (const char* extension : requiredExtensions) {
            if (!HasExtension(extensions, extension)) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=instance_extension_missing name=%{public}s", extension);
                return false;
            }
        }
        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "SDL3 OHOS Vulkan Skybox Cube";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.pEngineName = "SDL3 OHOS Vulkan scene sample";
        applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = sdlExtensionCount;
        createInfo.ppEnabledExtensionNames = sdlExtensions;
        result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            LogVkError("create_instance", result);
            return false;
        }
        return true;
    }

    bool CreateSurface()
    {
        if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=sdl_surface_create_failed error=%{public}s",
                SDL_GetError());
            return false;
        }
        return true;
    }

    bool DeviceHasSwapchainExtension(VkPhysicalDevice device) const
    {
        uint32_t extensionCount = 0;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount > 0 && vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
            extensions.data()) != VK_SUCCESS) {
            return false;
        }
        return HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    bool PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
        if (result != VK_SUCCESS || deviceCount == 0) {
            LogVkError("enumerate_physical_devices", result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        result = vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
        if (result != VK_SUCCESS) {
            LogVkError("enumerate_physical_devices", result);
            return false;
        }
        for (VkPhysicalDevice device : devices) {
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
            for (uint32_t index = 0; index < queueFamilyCount; ++index) {
                if ((queueFamilies[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
                    queueFamilies[index].queueCount == 0) {
                    continue;
                }
                VkBool32 supportsPresent = VK_FALSE;
                result = vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface_, &supportsPresent);
                if (result != VK_SUCCESS || supportsPresent == VK_FALSE || !DeviceHasSwapchainExtension(device)) {
                    continue;
                }
                physicalDevice_ = device;
                queueFamilyIndex_ = index;
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(device, &properties);
                OH_LOG_INFO(LOG_APP, "NATIVE_VULKAN stage=physical_device_selected name=%{public}s api=%{public}u.%{public}u.%{public}u",
                    properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                    VK_VERSION_PATCH(properties.apiVersion));
                return true;
            }
        }
        OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=no_present_graphics_queue");
        return false;
    }

    bool CreateDevice()
    {
        const float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex_;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = deviceExtensions;
        const VkResult result = vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            LogVkError("create_device", result);
            return false;
        }
        vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);
        if (queue_ == VK_NULL_HANDLE) {
            OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=queue_missing");
            return false;
        }
        return true;
    }

    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);
        if (result != VK_SUCCESS || (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
            LogVkError("surface_capabilities", result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result);
            return false;
        }
        uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            LogVkError("surface_formats", result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : result);
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
        if (result != VK_SUCCESS) {
            LogVkError("surface_formats", result);
            return false;
        }
        uint32_t presentModeCount = 0;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
        if (result != VK_SUCCESS || presentModeCount == 0) {
            LogVkError("present_modes", result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result);
            return false;
        }
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount,
            presentModes.data());
        if (result != VK_SUCCESS) {
            LogVkError("present_modes", result);
            return false;
        }
        // Rollback to the verified-stable configuration: FIFO is the only
        // present mode that survived long runs on the emulator's all_out
        // Vulkan proxy (MAILBOX/IMMEDIATE allowed the loop to present at an
        // unbounded rate and froze the whole guest).
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        SDL_Log("SDL3_VULKAN present_mode_selected=%d (count=%u)", static_cast<int>(presentMode),
            presentModeCount);
        VkSurfaceFormatKHR surfaceFormat = formats.front();
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
                surfaceFormat = candidate;
                break;
            }
        }
        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = ClampExtent(static_cast<uint32_t>(width_), capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width);
            extent.height = ClampExtent(static_cast<uint32_t>(height_), capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            return false;
        }
        // A HarmonyOS surface can report a compositor transform (often a
        // 90-degree rotation) even after the ability has been locked to
        // landscape.  Rendering into that transformed swapchain and then
        // presenting it makes the full-screen skybox appear sideways.  When
        // the surface advertises identity as a valid transform, keep the
        // Vulkan image in the same orientation as the landscape XComponent.
        VkSurfaceTransformFlagBitsKHR preTransform = capabilities.currentTransform;
        if ((capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
            preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        }
        OH_LOG_INFO(LOG_APP,
            "NATIVE_VULKAN stage=swapchain_orientation current=%{public}u selected=%{public}u extent=%{public}ux%{public}u",
            static_cast<uint32_t>(capabilities.currentTransform), static_cast<uint32_t>(preTransform), extent.width,
            extent.height);
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }
        VkCompositeAlphaFlagBitsKHR compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(0);
        const VkCompositeAlphaFlagBitsKHR alphaOptions[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (VkCompositeAlphaFlagBitsKHR candidate : alphaOptions) {
            if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
                compositeAlpha = candidate;
                break;
            }
        }
        if (compositeAlpha == 0) {
            return false;
        }
        VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = preTransform;
        createInfo.compositeAlpha = compositeAlpha;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            LogVkError("create_swapchain", result);
            return false;
        }
        uint32_t imageCapacity = std::max(imageCount, 8U);
        bool imagesLoaded = false;
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            swapchainImages_.assign(imageCapacity, VK_NULL_HANDLE);
            uint32_t actualImageCount = imageCapacity;
            result = vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, swapchainImages_.data());
            if (result == VK_SUCCESS) {
                if (actualImageCount == 0 || actualImageCount > imageCapacity) {
                    return false;
                }
                swapchainImages_.resize(actualImageCount);
                imagesLoaded = true;
                break;
            }
            if (result != VK_INCOMPLETE) {
                LogVkError("get_swapchain_images", result);
                return false;
            }
            imageCapacity = actualImageCount > imageCapacity ? actualImageCount : imageCapacity * 2U;
        }
        if (!imagesLoaded) {
            LogVkError("get_swapchain_images", VK_INCOMPLETE);
            return false;
        }
        swapchainFormat_ = surfaceFormat.format;
        sceneColorFormat_ = FindSceneColorFormat();
        if (sceneColorFormat_ == VK_FORMAT_UNDEFINED) {
            sceneColorFormat_ = swapchainFormat_;
        }
        const bool sceneHdr = sceneColorFormat_ == VK_FORMAT_R16G16B16A16_SFLOAT ||
            sceneColorFormat_ == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        SDL_Log("SDL3_VULKAN scene_target_format=%d hdr=%d", static_cast<int>(sceneColorFormat_),
            sceneHdr ? 1 : 0);
        swapchainExtent_ = extent;
        imageViews_.reserve(swapchainImages_.size());
        for (VkImage image : swapchainImages_) {
            VkImageView imageView = VK_NULL_HANDLE;
            if (!CreateImageView(image, swapchainFormat_, VK_IMAGE_VIEW_TYPE_2D, 1,
                VK_IMAGE_ASPECT_COLOR_BIT, &imageView)) {
                return false;
            }
            imageViews_.push_back(imageView);
        }
        return CreateDepthResources();
    }

    bool CreateRenderPass()
    {
        // Resolve the depth format before building the scene attachment list.
        // CreateSwapchain() normally does this already, but resolving it here
        // as well keeps render-pass creation independent of call order.
        if (depthFormat_ == VK_FORMAT_UNDEFINED) {
            depthFormat_ = FindDepthFormat();
            if (depthFormat_ == VK_FORMAT_UNDEFINED) {
                OH_LOG_ERROR(LOG_APP, "NATIVE_VULKAN stage=depth_format_missing");
                return false;
            }
        }

        // Scene pass: render the sky/models into a sampled intermediate image.
        // The final shader pass then owns the swapchain image, which gives the
        // Vulkan path the same scene -> post -> UI ordering as GLES.
        VkAttachmentDescription sceneColorAttachment{};
        sceneColorAttachment.format = sceneColorFormat_;
        sceneColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        sceneColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sceneColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sceneColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        sceneColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        sceneColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sceneColorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = depthFormat_;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentDescription sceneAttachments[] = {sceneColorAttachment, depthAttachment};
        VkAttachmentReference sceneColorReference{};
        sceneColorReference.attachment = 0;
        sceneColorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depthReference{};
        depthReference.attachment = 1;
        depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sceneSubpass{};
        sceneSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sceneSubpass.colorAttachmentCount = 1;
        sceneSubpass.pColorAttachments = &sceneColorReference;
        sceneSubpass.pDepthStencilAttachment = &depthReference;
        VkSubpassDependency sceneDependencies[2]{};
        sceneDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        sceneDependencies[0].dstSubpass = 0;
        sceneDependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        sceneDependencies[0].dstStageMask = sceneDependencies[0].srcStageMask;
        sceneDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sceneDependencies[1].srcSubpass = 0;
        sceneDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        sceneDependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        sceneDependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        sceneDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sceneDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo sceneCreateInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        sceneCreateInfo.attachmentCount = 2;
        sceneCreateInfo.pAttachments = sceneAttachments;
        sceneCreateInfo.subpassCount = 1;
        sceneCreateInfo.pSubpasses = &sceneSubpass;
        sceneCreateInfo.dependencyCount = 2;
        sceneCreateInfo.pDependencies = sceneDependencies;
        VkResult result = vkCreateRenderPass(device_, &sceneCreateInfo, nullptr, &renderPass_);
        if (result != VK_SUCCESS) {
            LogVkError("create_scene_render_pass", result);
            return false;
        }

        // Post/UI pass: load the acquired swapchain image, resolve the scene,
        // then draw the crisp overlay and SDF text on top of the result.
        VkAttachmentDescription postColorAttachment{};
        postColorAttachment.format = swapchainFormat_;
        postColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        postColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        postColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        postColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        postColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        postColorAttachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        postColorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference postColorReference{};
        postColorReference.attachment = 0;
        postColorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription postSubpass{};
        postSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        postSubpass.colorAttachmentCount = 1;
        postSubpass.pColorAttachments = &postColorReference;
        VkSubpassDependency postDependency{};
        postDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        postDependency.dstSubpass = 0;
        postDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        postDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        postDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo postCreateInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        postCreateInfo.attachmentCount = 1;
        postCreateInfo.pAttachments = &postColorAttachment;
        postCreateInfo.subpassCount = 1;
        postCreateInfo.pSubpasses = &postSubpass;
        postCreateInfo.dependencyCount = 1;
        postCreateInfo.pDependencies = &postDependency;
        result = vkCreateRenderPass(device_, &postCreateInfo, nullptr, &postRenderPass_);
        if (result != VK_SUCCESS) {
            LogVkError("create_post_render_pass", result);
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
            return false;
        }
        // Each GLES bloom level is a separate color target.  Reusing one
        // render pass keeps the command path identical for threshold,
        // downsample and upsample levels while their framebuffers carry the
        // different extents.
        VkAttachmentDescription bloomAttachment{};
        bloomAttachment.format = sceneColorFormat_;
        bloomAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        bloomAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        bloomAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bloomAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        bloomAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        bloomAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bloomAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference bloomReference{};
        bloomReference.attachment = 0;
        bloomReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription bloomSubpass{};
        bloomSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        bloomSubpass.colorAttachmentCount = 1;
        bloomSubpass.pColorAttachments = &bloomReference;
        VkSubpassDependency bloomDependencies[2]{};
        bloomDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        bloomDependencies[0].dstSubpass = 0;
        bloomDependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bloomDependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bloomDependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bloomDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bloomDependencies[1].srcSubpass = 0;
        bloomDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        bloomDependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bloomDependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        bloomDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bloomDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo bloomCreateInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        bloomCreateInfo.attachmentCount = 1;
        bloomCreateInfo.pAttachments = &bloomAttachment;
        bloomCreateInfo.subpassCount = 1;
        bloomCreateInfo.pSubpasses = &bloomSubpass;
        bloomCreateInfo.dependencyCount = 2;
        bloomCreateInfo.pDependencies = bloomDependencies;
        result = vkCreateRenderPass(device_, &bloomCreateInfo, nullptr, &bloomRenderPass_);
        if (result != VK_SUCCESS) {
            LogVkError("create_bloom_render_pass", result);
            vkDestroyRenderPass(device_, postRenderPass_, nullptr);
            postRenderPass_ = VK_NULL_HANDLE;
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
            return false;
        }

        // One depth-only pass feeds the model receiver's manual 3x3 PCF. The
        // final read-only layout and external dependency make the sampled map
        // visible to the later forward material pass in this same command
        // buffer without a separate barrier.
        VkAttachmentDescription shadowAttachment{};
        shadowAttachment.format = depthFormat_;
        shadowAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        shadowAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadowAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        shadowAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        shadowAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        shadowAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        shadowAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference shadowReference{};
        shadowReference.attachment = 0;
        shadowReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription shadowSubpass{};
        shadowSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        shadowSubpass.colorAttachmentCount = 0;
        shadowSubpass.pDepthStencilAttachment = &shadowReference;
        VkSubpassDependency shadowDependencies[2]{};
        shadowDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        shadowDependencies[0].dstSubpass = 0;
        shadowDependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        shadowDependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        shadowDependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        shadowDependencies[1].srcSubpass = 0;
        shadowDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        shadowDependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        shadowDependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        shadowDependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        shadowDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo shadowCreateInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        shadowCreateInfo.attachmentCount = 1;
        shadowCreateInfo.pAttachments = &shadowAttachment;
        shadowCreateInfo.subpassCount = 1;
        shadowCreateInfo.pSubpasses = &shadowSubpass;
        shadowCreateInfo.dependencyCount = 2;
        shadowCreateInfo.pDependencies = shadowDependencies;
        result = vkCreateRenderPass(device_, &shadowCreateInfo, nullptr, &shadowRenderPass_);
        if (result != VK_SUCCESS) {
            LogVkError("create_shadow_render_pass", result);
            vkDestroyRenderPass(device_, bloomRenderPass_, nullptr);
            bloomRenderPass_ = VK_NULL_HANDLE;
            vkDestroyRenderPass(device_, postRenderPass_, nullptr);
            postRenderPass_ = VK_NULL_HANDLE;
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    bool CreateSyncObjects()
    {
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t slot = 0; slot < kInFlightFrames; ++slot) {
            VkResult result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_[slot]);
            if (result != VK_SUCCESS) {
                LogVkError("create_image_available_semaphore", result);
                return false;
            }
            result = vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_[slot]);
            if (result != VK_SUCCESS) {
                LogVkError("create_render_finished_semaphore", result);
                return false;
            }
            result = vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[slot]);
            if (result != VK_SUCCESS) {
                LogVkError("create_in_flight_fence", result);
                return false;
            }
        }
        return true;
    }

    SDL_Window* window_ = nullptr;
    uint64_t width_ = 0;
    uint64_t height_ = 0;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat sceneColorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> imageViews_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkRenderPass postRenderPass_ = VK_NULL_HANDLE;
    VkRenderPass bloomRenderPass_ = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline skyboxPipeline_ = VK_NULL_HANDLE;
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    VkPipeline iconPipeline_ = VK_NULL_HANDLE;
    VkPipeline textPipeline_ = VK_NULL_HANDLE;
    VkPipeline postPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomThresholdPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomDownPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomUpPipeline_ = VK_NULL_HANDLE;
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkFramebuffer> postFramebuffers_;
    std::vector<VkFramebuffer> bloomDsFramebuffers_;
    std::vector<VkFramebuffer> bloomUpFramebuffers_;
    VkFramebuffer shadowFramebuffer_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    // Rollback to a single in-flight frame: wait fence -> acquire -> upload ->
    // submit -> present are fully serialized every frame.  This is the exact
    // flow that ran 240 s+ with zero faultlog on the emulator (the double
    // buffered variant destabilized the guest).
    static constexpr uint32_t kInFlightFrames = 1;
    VkFence inFlightFences_[kInFlightFrames] = {VK_NULL_HANDLE};
    VkSemaphore imageAvailable_[kInFlightFrames] = {VK_NULL_HANDLE};
    VkSemaphore renderFinished_[kInFlightFrames] = {VK_NULL_HANDLE};
    uint32_t currentSlot_ = 0;
    TextureResource cubeTexture_;
    TextureResource whiteTexture_;
    TextureResource neutralMrTexture_;
    TextureResource neutralNormalTexture_;
    TextureResource neutralAoTexture_;
    TextureResource blackTexture_;
    TextureResource skyboxTexture_;
    TextureResource irradianceTexture_;
    TextureResource prefilteredTexture_;
    TextureResource brdfLutTexture_;
    TextureResource textAtlasTexture_;
    std::array<TextureResource, rhi::CameraInput::kActionButtonCount> actionIconTextures_{};
    TextureResource sceneColorTarget_;
    TextureResource shadowMap_;
    std::array<TextureResource, kBloomDsLevels> bloomDs_{};
    std::array<TextureResource, kBloomUpLevels> bloomUp_{};
    std::array<uint32_t, kBloomDsLevels> bloomDsWidths_{};
    std::array<uint32_t, kBloomDsLevels> bloomDsHeights_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    BufferResource cubeVertexBuffer_;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout overlayDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout postDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    // Per-swapchain-image uniform buffer + descriptor set: the command buffer
    // recorded for an image reads its own UBO, so re-recording a frame never
    // touches a buffer the GPU may still be reading.
    std::vector<BufferResource> uniformBuffers_;
    std::vector<BufferResource> uiVertexBuffers_;
    std::vector<VkDescriptorSet> descriptorSets_;
    std::vector<VkDescriptorSet> postDescriptorSets_;
    VkDescriptorSet bloomThresholdDescriptorSet_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kBloomDsLevels - 1> bloomDownDescriptorSets_{};
    std::array<VkDescriptorSet, kBloomUpLevels> bloomUpDescriptorSets_{};
    VkDescriptorSet overlayDescriptorSet_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, rhi::CameraInput::kActionButtonCount> actionIconDescriptorSets_{};
    BufferResource modelInstanceBuffer_;
    std::vector<CharacterInstanceData> modelInstanceData_;
    BufferResource identityJointBuffer_;
    // glTF model residency.  The three assets are loaded once from the HAP's
    // rawfile bundle; if one fails to load the remaining scene stays usable.
    VkPipeline modelPipeline_ = VK_NULL_HANDLE;
    ModelGpuAsset playerAsset_;
    ModelGpuAsset enemyAsset_;
    ModelGpuAsset helmetAsset_;
    ModelGpuAsset groundAsset_;
    static constexpr const char* kModelPath =
        "models/Quaternius/AnimationLibrary_Standard.glb";
    static constexpr const char* kHelmetPath =
        "models/DamagedHelmet/glTF/DamagedHelmet.gltf";
    static constexpr const char* kCubePath = "models/BaseModel/cube.glb";
    bool sceneReady_ = false;
    bool swapchainDirty_ = false;
    bool surfaceNeedsRecreate_ = false;
    bool surfaceRecreateFailureLogged_ = false;
    bool deviceLost_ = false;
    bool firstFrameLogged_ = false;
    rhi::CameraInput cameraInput_{};
    const scene::Definition* sceneDefinition_ = nullptr;
    physics::JoltGameplayPhysics* physicsWorld_ = nullptr;
    bool physicsReset_ = false;
    float camYaw_ = 0.0f;
    float camPitch_ = 0.0f;
    float camDistance_ = 2.8f;
    float camPanX_ = 0.0f;
    float camPanY_ = 0.0f;
    float camPanZ_ = 0.0f;
    float camPosX_ = 0.0f;
    float camPosY_ = 0.0f;
    float camPosZ_ = 2.8f;
    float playerX_ = 0.0f;
    float playerY_ = 0.0f;
    float playerZ_ = 0.0f;
    float playerYaw_ = 0.0f;
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
    // Timer < 0 means inactive; enemyDissolveAmount_ feeds the push constants.
    static constexpr float kEnemyDissolveDuration = 1.2f;
    float enemyDissolveTimer_ = -1.0f;
    float enemyDissolveAmount_ = 0.0f;
    bool enemyMoving_ = false;
    bool enemyAttackRequested_ = false;
    float enemyDesiredVelocityX_ = 0.0f;
    float enemyDesiredVelocityZ_ = 0.0f;
    bool playerMoving_ = false;
    bool playerSprinting_ = false;
    bool crouchEnabled_ = false;
    Uint64 moveLastTicks_ = 0;
    Mat4 viewProj_{};
    Mat4 playerMvp_{};
    Mat4 playerMatrix_{};
    Mat4 enemyMatrix_{};
    Mat4 shadowMatrix_{};
    rhi::UiScreen uiScreen_ = rhi::UiScreen::MainMenu;
    bool settingFps_ = false;
    int sensIndex_ = 1;
    float lookSensitivityScale_ = 1.0f;
    static constexpr float kSensitivityChoices[3] = {0.5f, 1.0f, 2.0f};
    bool bloomEnabled_ = true;
    float renderScale_ = 1.0f;
    int resIndex_ = 0;
    // Global UI alpha multiplier: every overlay append primitive scales its
    // fill alpha by this factor, so one setting tunes the whole interface
    // while each element keeps its relative hierarchy.
    float uiOpacity_ = 0.80f;             // default 80%
    int uiOpacityIndex_ = 1;              // index into kOpacityChoices
    static constexpr float kOpacityChoices[5] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
    inventory::State inventory_;  // bag grid + equipment slots
    // Set by the settings RES tap; consumed at the top of DrawOnce, before
    // the next image acquire, by RecreateSwapchain().
    bool renderTargetsDirty_ = false;
    static constexpr float kScaleChoices[4] = {1.0f, 0.75f, 0.5f, 0.25f};
    static constexpr float kMinimumPlayerAttackCooldown = 0.75f;
    static constexpr float kPlayerAttackInvulnerability = 0.20f;
    static constexpr float kHitInvulnerability = 0.45f;
    // Mikan PlayerWalkScript hit-stun constants (hitDuration / flash 0.18 s).
    static constexpr float kPlayerHitDuration = 0.333f;
    static constexpr float kHitFlashDuration = 0.18f;
    Uint64 fpsLastTicks_ = 0;
    float fpsAvgMs_ = 0.0f;
};

} // namespace

namespace rhi {

// The only symbol this backend exports.  The concrete renderer stays in an
// anonymous namespace: nothing outside needs its type, only the interface.
IRenderer* CreateVulkanRenderer()
{
    return new VulkanRenderer();
}

} // namespace rhi
