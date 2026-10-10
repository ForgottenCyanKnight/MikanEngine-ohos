// CPU-side glTF 2.0 model loading for the SDL3 OHOS Vulkan sample.
//
// The loader is deliberately small and self contained: it turns a .gltf (or
// .glb) that lives inside the HAP rawfile bundle into plain arrays that the
// renderer can upload with a single staging copy.  Nothing here talks to
// Vulkan.
//
// Design notes that matter for this project:
//   * Node transforms are baked into the vertex data at load time.  The
//     OHOS emulator's Vulkan calls each round-trip to a host proxy process,
//     so every per-frame draw setup we can delete is worth deleting.
//   * The baked geometry is centred on the origin and uniformly scaled so
//     its largest extent is exactly 2.0 units.  Assets arrive with wildly
//     different scales and off-centre pivots (MetalRoughSpheres spans
//     x[-12.2, 11.9] while sitting at y[-1, 9]), so guessing a camera from
//     the raw bounds is not reliable.
//   * Only what a static opaque draw needs is parsed: POSITION, NORMAL,
//     TEXCOORD_0, indices and the metallic-roughness material set (base
//     colour / metallicRoughness / normal / occlusion / emissive textures
//     plus their factors).
//   * Skinned meshes are supported: JOINTS_0/WEIGHTS_0 vertices, one skin
//     (joint nodes + inverse bind matrices) and preloaded animation clips.
//     Skinned vertices stay in bind-pose space -- the node hierarchy is NOT
//     baked into them and the centring/normalisation is exposed as
//     Model::normaliseMatrix for the renderer to fold into the joint
//     matrices (see the renderer's UpdateAnimation).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ohos_model {

// Matches the model pipeline vertex input: 3 float position, 3 float normal,
// 2 float texture coordinate, 4 u8 joint indices, 4 u8 weights = 40 bytes.
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    // glTF JOINTS_0 / WEIGHTS_0.  Joint indices index Model::skin.jointNodes;
    // weights are quantised to u8 (255 == 1.0, renormalised at load).  All
    // zeros for static (unskinned) meshes.
    std::uint8_t joints[4] = {0, 0, 0, 0};
    std::uint8_t weights[4] = {0, 0, 0, 0};
};

// One skeleton node.  Parents always precede children in Model::nodes order,
// so global transforms can be computed with a single forward pass.
struct Node {
    std::string name;
    std::int32_t parent = -1;
    // Default local TRS (glTF node data; a matrix-only node is decomposed at
    // load).  Animation channels override individual components per frame.
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x y z w
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

// glTF skin: joint node indices in attribute order (JOINTS_0 values index
// this array) plus the matching inverse bind matrices, column-major.
struct Skin {
    std::vector<std::uint32_t> jointNodes;
    std::vector<float> inverseBindMatrices;  // 16 floats per joint

    std::size_t JointCount() const { return jointNodes.size(); }
};

// Preloaded animation data (cgltf-independent copies).  glTF's per-property
// samplers are kept as-is; the renderer composes TRS per node per frame.
struct AnimationChannel {
    std::uint32_t nodeIndex = 0;
    int path = 0;           // 0 translation, 1 rotation, 2 scale
    int interpolation = 0;  // 0 linear, 1 step
    std::vector<float> times;   // seconds, ascending
    std::vector<float> values;  // 3 floats (t/s) or 4 floats (r, x y z w) per key
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;  // seconds
    std::vector<AnimationChannel> channels;
};

struct Mesh {
    bool castShadow = true;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t materialIndex = 0;
    // 2 when every index fits in 16 bits, otherwise 4.  metalRoughSpheres
    // peaks at 62664 vertices per mesh, so the 16 bit path is used and the
    // uploaded index data stays at 3 MB instead of 6 MB.
    std::uint32_t indexBytes = 2;
};

struct Material {
    bool waterSurface = false;
    bool worldSpaceUv = false; // Cube faces repeat once per world metre.
    // glTF material name, e.g. "M_Joints" -- the renderer uses it to skip
    // debug-only sub-meshes (Quaternius' weight-paint shell lives INSIDE the
    // body and pokes through the surface when rendered opaquely).
    std::string name;
    std::int32_t baseColorImage = -1;
    std::int32_t metallicRoughnessImage = -1;
    std::int32_t normalImage = -1;
    std::int32_t aoImage = -1;
    std::int32_t emissiveImage = -1;
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};
    float normalScale = 1.0f;
};

struct Image {
    std::string uri;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Always decoded to 4 channels so the uploader can assume RGBA8.
    std::vector<std::uint8_t> rgba;
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Image> images;
    // Skeleton scene graph (parents precede children), skin and animations.
    std::vector<Node> nodes;
    Skin skin;
    std::vector<AnimationClip> animations;
    // True when at least one primitive carried JOINTS_0/WEIGHTS_0 and a skin
    // was resolved.  Skinned vertices stay in raw bind-pose model space: the
    // centre/normalise transform is NOT baked into them (it would desync the
    // inverse bind matrices) but applied to the joint matrices instead, via
    // normaliseMatrix below.
    bool skinned = false;
    // Model space -> centred/normalised space (largest extent 2.0).
    // Column-major.  Identity for static models (whose vertices are baked).
    float normaliseMatrix[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    // Bounds after centring and normalisation (largest extent == 2.0).
    float minBounds[3] = {0.0f, 0.0f, 0.0f};
    float maxBounds[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t triangleCount = 0;
    std::uint32_t vertexCount = 0;

    void Clear()
    {
        meshes.clear();
        materials.clear();
        images.clear();
        nodes.clear();
        skin.jointNodes.clear();
        skin.inverseBindMatrices.clear();
        animations.clear();
        skinned = false;
        for (int c = 0; c < 16; ++c) {
            normaliseMatrix[c] = (c % 5 == 0) ? 1.0f : 0.0f;
        }
        triangleCount = 0;
        vertexCount = 0;
    }
};

// `gltfPath` is relative to the rawfile root, for example
// "models/MetalRoughSpheres/MetalRoughSpheres.gltf".  Buffers and images
// referenced by a relative URI resolve against the same directory.
//
// Returns false and fills `error` on failure.  A failure is not fatal for the
// caller: the renderer keeps rendering its built-in scene and logs the reason.
bool LoadModelFromRawFile(const char* gltfPath, Model& model, std::string& error);

} // namespace ohos_model
