// Implementation of the glTF 2.0 loader declared in model_loader.h.
//
// cgltf is compiled into this translation unit only.  Note that
// STB_IMAGE_IMPLEMENTATION is deliberately NOT defined here: it is already
// defined in vulkan_sdl.cpp and both files are linked into the same
// libentry.so, so defining it twice would be a duplicate symbol error.
// We only need the declarations to call stbi_load_from_memory().

#include "model_loader.h"

// SDL's vendored stb_image.h is patched to use SDL's integer typedefs
// (Uint8/Uint16) instead of <stdlib.h> types, so SDL_stdinc.h has to be in
// scope before it is included.
#include "SDL3/SDL_stdinc.h"
#include "stb_image.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf.h"

extern "C" int OHOS_ReadRawFile(const char* path, void** data, size_t* size);
extern "C" void OHOS_FreeRawFile(void* data);

namespace ohos_model {
namespace {

constexpr const char* kLogTag = "SDL3_MODEL";
// Target largest extent after centring.  Matches the camera the renderer
// already uses for its built-in cube (60 degree vertical FOV at distance
// 4.5), so the model lands comfortably inside the frame.
constexpr float kNormalizedExtent = 2.0f;

// Mirrors cgltf_combine_paths(): keep the directory part of `base` and append
// `uri`.  cgltf uses this internally for buffers; images need the same rule.
std::string CombinePath(const std::string& base, const std::string& uri)
{
    const std::size_t slash = base.find_last_of("/\\");
    if (slash == std::string::npos) {
        return uri;
    }
    return base.substr(0, slash + 1) + uri;
}

cgltf_result RawFileRead(const cgltf_memory_options* /*memoryOptions*/,
    const cgltf_file_options* /*fileOptions*/, const char* path, cgltf_size* size, void** data)
{
    void* bytes = nullptr;
    std::size_t bytesSize = 0;
    if (!OHOS_ReadRawFile(path, &bytes, &bytesSize) || bytes == nullptr || bytesSize == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s rawfile_open_failed path=%s", kLogTag, path);
        return cgltf_result_file_not_found;
    }
    *data = bytes;
    *size = static_cast<cgltf_size>(bytesSize);
    return cgltf_result_success;
}

void RawFileRelease(const cgltf_memory_options* /*memoryOptions*/,
    const cgltf_file_options* /*fileOptions*/, void* data, cgltf_size /*size*/)
{
    OHOS_FreeRawFile(data);
}

// Column-major 4x4 helpers, matching the convention used by the renderer's
// own Mat4 type and by glTF.
void TransformPoint(const float m[16], const float in[3], float out[3])
{
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

// Normal matrix = inverse transpose of the upper-left 3x3.  Assets are free to
// carry non-uniform scale, and a plain mat3 multiply would then skew normals
// and break the shading.  Falls back to the plain 3x3 for degenerate matrices.
void BuildNormalMatrix(const float m[16], float out[9])
{
    const float a = m[0], b = m[4], c = m[8];
    const float d = m[1], e = m[5], f = m[9];
    const float g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) {
        out[0] = a; out[1] = b; out[2] = c;
        out[3] = d; out[4] = e; out[5] = f;
        out[6] = g; out[7] = h; out[8] = i;
        return;
    }
    const float inv = 1.0f / det;
    // N = (M^-1)^T = cofactor(M) / det -- the cofactor matrix itself, NOT its
    // transpose.  Writing adj(M) = C^T here would yield plain M^-1, which for
    // a rotated node applies the rotation backwards (normals mirrored against
    // the baked positions -- "green faces down" in the normal debug view).
    // out[] is column-major: out[0..2] is column 0 = (C00, C10, C20).
    out[0] = (e * i - f * h) * inv;
    out[1] = (c * h - b * i) * inv;
    out[2] = (b * f - c * e) * inv;
    out[3] = (f * g - d * i) * inv;
    out[4] = (a * i - c * g) * inv;
    out[5] = (c * d - a * f) * inv;
    out[6] = (d * h - e * g) * inv;
    out[7] = (b * g - a * h) * inv;
    out[8] = (a * e - b * d) * inv;
}

void TransformNormal(const float normalMatrix[9], const float in[3], float out[3])
{
    out[0] = normalMatrix[0] * in[0] + normalMatrix[3] * in[1] + normalMatrix[6] * in[2];
    out[1] = normalMatrix[1] * in[0] + normalMatrix[4] * in[1] + normalMatrix[7] * in[2];
    out[2] = normalMatrix[2] * in[0] + normalMatrix[5] * in[1] + normalMatrix[8] * in[2];
    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (length > 1e-8f) {
        const float scale = 1.0f / length;
        out[0] *= scale;
        out[1] *= scale;
        out[2] *= scale;
    }
}

const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type)
{
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        if (primitive.attributes[i].type == type) {
            return primitive.attributes[i].data;
        }
    }
    return nullptr;
}

// General column-major 4x4 inverse (adjugate method) -- used for computing
// inverse bind matrices when a skin omits them.
void InvertMatrix16(const float s[16], float inv[16])
{
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
        + s[8] * s[1] * s[7] - s[8] * s[3] * s[5];
    inv[15] = s[0] * s[5] * s[10] - s[0] * s[6] * s[9] - s[4] * s[1] * s[10] + s[4] * s[2] * s[9]
        + s[8] * s[1] * s[6] - s[8] * s[2] * s[5];
    const float determinant = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
    if (determinant == 0.0f) {
        for (int i = 0; i < 16; ++i) {
            inv[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        return;
    }
    const float inverseDeterminant = 1.0f / determinant;
    for (int i = 0; i < 16; ++i) {
        inv[i] *= inverseDeterminant;
    }
}

// Reads a glTF image from either its URI (external file inside the HAP) or its
// buffer view (embedded in the .bin / .glb), then decodes it to RGBA8.
bool LoadImage(const cgltf_image& source, const std::string& gltfPath, Image& out)
{
    void* encoded = nullptr;
    std::size_t encodedSize = 0;
    bool ownsEncoded = false;

    if (source.uri != nullptr && source.uri[0] != '\0') {
        out.uri = source.uri;
        const std::string path = CombinePath(gltfPath, source.uri);
        if (!OHOS_ReadRawFile(path.c_str(), &encoded, &encodedSize) || encoded == nullptr ||
            encodedSize == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s image_rawfile_failed path=%s", kLogTag,
                path.c_str());
            return false;
        }
        ownsEncoded = true;
    } else if (source.buffer_view != nullptr && source.buffer_view->buffer != nullptr &&
        source.buffer_view->buffer->data != nullptr) {
        const unsigned char* base =
            static_cast<const unsigned char*>(source.buffer_view->buffer->data) + source.buffer_view->offset;
        encoded = const_cast<unsigned char*>(base);
        encodedSize = static_cast<std::size_t>(source.buffer_view->size);
        out.uri = source.name != nullptr ? source.name : "<bufferView>";
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s image_source_missing", kLogTag);
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(encoded),
        static_cast<int>(encodedSize), &width, &height, &channels, 4);
    if (ownsEncoded) {
        OHOS_FreeRawFile(encoded);
    }
    if (pixels == nullptr || width <= 0 || height <= 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s image_decode_failed uri=%s", kLogTag,
            out.uri.c_str());
        return false;
    }

    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    SDL_Log("%s image_ready uri=%s size=%dx%d bytes=%u", kLogTag, out.uri.c_str(), width, height,
        static_cast<unsigned int>(out.rgba.size()));
    return true;
}

// Collects the nodes that actually belong to the scene graph, in scene order.
void CollectSceneNodes(const cgltf_data& data, std::vector<const cgltf_node*>& out)
{
    const cgltf_scene* scene = data.scene != nullptr ? data.scene :
        (data.scenes_count > 0 ? &data.scenes[0] : nullptr);
    if (scene == nullptr || scene->nodes_count == 0) {
        for (cgltf_size i = 0; i < data.nodes_count; ++i) {
            out.push_back(&data.nodes[i]);
        }
        return;
    }
    std::vector<const cgltf_node*> stack(scene->nodes, scene->nodes + scene->nodes_count);
    while (!stack.empty()) {
        const cgltf_node* node = stack.back();
        stack.pop_back();
        out.push_back(node);
        for (cgltf_size i = 0; i < node->children_count; ++i) {
            stack.push_back(node->children[i]);
        }
    }
}

} // namespace

bool LoadModelFromRawFile(const char* gltfPath, Model& model, std::string& error)
{
    model.Clear();
    if (gltfPath == nullptr || gltfPath[0] == '\0') {
        error = "empty gltf path";
        return false;
    }

    // The .gltf bytes must outlive cgltf_free(), because parts of cgltf_data
    // point straight into the parsed JSON.
    void* gltfBytes = nullptr;
    std::size_t gltfSize = 0;
    if (!OHOS_ReadRawFile(gltfPath, &gltfBytes, &gltfSize) || gltfBytes == nullptr || gltfSize == 0) {
        error = std::string("rawfile open failed: ") + gltfPath;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s gltf_rawfile_failed path=%s", kLogTag, gltfPath);
        return false;
    }
    SDL_Log("%s gltf_read path=%s bytes=%u", kLogTag, gltfPath, static_cast<unsigned int>(gltfSize));

    cgltf_options options{};
    options.file.read = &RawFileRead;
    options.file.release = &RawFileRelease;

    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse(&options, gltfBytes, gltfSize, &data);
    if (result != cgltf_result_success || data == nullptr) {
        error = "cgltf_parse failed";
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s cgltf_parse_failed code=%d", kLogTag,
            static_cast<int>(result));
        OHOS_FreeRawFile(gltfBytes);
        return false;
    }
    // External buffers referenced by URI are resolved through RawFileRead, so
    // "MetalRoughSpheres0.bin" becomes
    // "models/MetalRoughSpheres/MetalRoughSpheres0.bin".
    result = cgltf_load_buffers(&options, data, gltfPath);
    if (result != cgltf_result_success) {
        error = "cgltf_load_buffers failed";
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s cgltf_load_buffers_failed code=%d", kLogTag,
            static_cast<int>(result));
        cgltf_free(data);
        OHOS_FreeRawFile(gltfBytes);
        return false;
    }

    bool ok = true;

    // ---- materials -------------------------------------------------------
    model.materials.resize(data->materials_count > 0 ? data->materials_count : 1);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& source = data->materials[i];
        Material& material = model.materials[i];
        if (source.name != nullptr) {
            material.name = source.name;
        }
        for (int c = 0; c < 4; ++c) {
            material.baseColorFactor[c] = source.pbr_metallic_roughness.base_color_factor[c];
        }
        const cgltf_texture_view& baseColor =
            source.pbr_metallic_roughness.base_color_texture;
        if (baseColor.texture != nullptr && baseColor.texture->image != nullptr) {
            material.baseColorImage = static_cast<std::int32_t>(
                baseColor.texture->image - data->images);
        }
        const cgltf_texture_view& metalRough =
            source.pbr_metallic_roughness.metallic_roughness_texture;
        if (metalRough.texture != nullptr && metalRough.texture->image != nullptr) {
            material.metallicRoughnessImage = static_cast<std::int32_t>(
                metalRough.texture->image - data->images);
        }
        material.metallicFactor = source.pbr_metallic_roughness.metallic_factor;
        material.roughnessFactor = source.pbr_metallic_roughness.roughness_factor;
        const cgltf_texture_view& normalTex = source.normal_texture;
        if (normalTex.texture != nullptr && normalTex.texture->image != nullptr) {
            material.normalImage = static_cast<std::int32_t>(
                normalTex.texture->image - data->images);
            material.normalScale = normalTex.scale;
        }
        const cgltf_texture_view& occlusionTex = source.occlusion_texture;
        if (occlusionTex.texture != nullptr && occlusionTex.texture->image != nullptr) {
            material.aoImage = static_cast<std::int32_t>(
                occlusionTex.texture->image - data->images);
        }
        const cgltf_texture_view& emissiveTex = source.emissive_texture;
        if (emissiveTex.texture != nullptr && emissiveTex.texture->image != nullptr) {
            material.emissiveImage = static_cast<std::int32_t>(
                emissiveTex.texture->image - data->images);
        }
        for (int c = 0; c < 3; ++c) {
            material.emissiveFactor[c] = source.emissive_factor[c] *
                (source.has_emissive_strength ? source.emissive_strength.emissive_strength : 1.0f);
        }
    }

    // ---- images ----------------------------------------------------------
    model.images.resize(data->images_count);
    for (cgltf_size i = 0; i < data->images_count && ok; ++i) {
        ok = LoadImage(data->images[i], gltfPath, model.images[i]);
        if (!ok) {
            error = "image load failed";
        }
    }

    // ---- skeleton scene graph ---------------------------------------------
    // DFS over the scene roots; parents are assigned indices before their
    // children, so the renderer can propagate global transforms in one
    // forward pass.  Node TRS defaults are kept per component (not as a
    // matrix) so animation channels can override individual parts.
    std::unordered_map<const cgltf_node*, std::uint32_t> nodeLookup;
    if (ok) {
        const cgltf_scene* scene = data->scene != nullptr ? data->scene :
            (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        std::vector<std::pair<const cgltf_node*, std::int32_t>> stack;
        if (scene != nullptr && scene->nodes_count > 0) {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
                stack.push_back({scene->nodes[i], -1});
            }
        } else {
            for (cgltf_size i = 0; i < data->nodes_count; ++i) {
                stack.push_back({&data->nodes[i], -1});
            }
        }
        while (!stack.empty()) {
            const cgltf_node* node = stack.back().first;
            const std::int32_t parent = stack.back().second;
            stack.pop_back();
            const std::uint32_t index = static_cast<std::uint32_t>(model.nodes.size());
            Node entry;
            entry.name = node->name != nullptr ? node->name : "";
            entry.parent = parent;
            if (node->has_translation) {
                for (int c = 0; c < 3; ++c) {
                    entry.translation[c] = node->translation[c];
                }
            }
            if (node->has_rotation) {
                for (int c = 0; c < 4; ++c) {
                    entry.rotation[c] = node->rotation[c];
                }
            }
            if (node->has_scale) {
                for (int c = 0; c < 3; ++c) {
                    entry.scale[c] = node->scale[c];
                }
            }
            if (node->has_matrix) {
                // Matrix-only node: decompose to TRS.  Assumes no skew (glTF
                // forbids it) -- translation from column 3, scale from column
                // lengths, rotation from the normalised basis via quaternion.
                const float* m = node->matrix;
                for (int c = 0; c < 3; ++c) {
                    entry.translation[c] = m[12 + c];
                    const float len = std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1]
                        + m[c * 4 + 2] * m[c * 4 + 2]);
                    entry.scale[c] = len;
                }
                // orthonormalise the rotation basis
                float r[3][3];
                for (int col = 0; col < 3; ++col) {
                    float v[3] = {m[col * 4], m[col * 4 + 1], m[col * 4 + 2]};
                    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (len > 1e-12f) {
                        v[0] /= len; v[1] /= len; v[2] /= len;
                    }
                    r[0][col] = v[0]; r[1][col] = v[1]; r[2][col] = v[2];
                }
                // shear removal: re-orthogonalise rows 1,2 against row 0
                for (int row = 1; row < 3; ++row) {
                    float dot01 = r[0][0] * r[row][0] + r[0][1] * r[row][1] + r[0][2] * r[row][2];
                    for (int col = 0; col < 3; ++col) {
                        r[row][col] -= dot01 * r[0][col];
                    }
                }
                const float trace = r[0][0] + r[1][1] + r[2][2];
                if (trace > 0.0f) {
                    const float t = std::sqrt(trace + 1.0f) * 2.0f;
                    entry.rotation[3] = 0.25f * t;
                    entry.rotation[0] = (r[2][1] - r[1][2]) / t;
                    entry.rotation[1] = (r[0][2] - r[2][0]) / t;
                    entry.rotation[2] = (r[1][0] - r[0][1]) / t;
                } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
                    const float t = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
                    entry.rotation[3] = (r[2][1] - r[1][2]) / t;
                    entry.rotation[0] = 0.25f * t;
                    entry.rotation[1] = (r[0][1] + r[1][0]) / t;
                    entry.rotation[2] = (r[0][2] + r[2][0]) / t;
                } else if (r[1][1] > r[2][2]) {
                    const float t = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
                    entry.rotation[3] = (r[0][2] - r[2][0]) / t;
                    entry.rotation[0] = (r[0][1] + r[1][0]) / t;
                    entry.rotation[1] = 0.25f * t;
                    entry.rotation[2] = (r[1][2] + r[2][1]) / t;
                } else {
                    const float t = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
                    entry.rotation[3] = (r[1][0] - r[0][1]) / t;
                    entry.rotation[0] = (r[0][2] + r[2][0]) / t;
                    entry.rotation[1] = (r[1][2] + r[2][1]) / t;
                    entry.rotation[2] = 0.25f * t;
                }
            }
            nodeLookup.emplace(node, index);
            model.nodes.push_back(entry);
            for (cgltf_size c = 0; c < node->children_count; ++c) {
                stack.push_back({node->children[c], static_cast<std::int32_t>(index)});
            }
        }
    }

    // ---- skin ---------------------------------------------------------------
    if (ok && data->skins_count > 0) {
        const cgltf_skin& source = data->skins[0];
        bool jointsOk = source.joints_count > 0;
        model.skin.jointNodes.reserve(source.joints_count);
        for (cgltf_size j = 0; jointsOk && j < source.joints_count; ++j) {
            const auto it = nodeLookup.find(source.joints[j]);
            if (it == nodeLookup.end()) {
                jointsOk = false;
                break;
            }
            model.skin.jointNodes.push_back(it->second);
        }
        if (jointsOk) {
            const cgltf_accessor* ibm = source.inverse_bind_matrices;
            model.skin.inverseBindMatrices.assign(source.joints_count * 16, 0.0f);
            if (ibm != nullptr && ibm->count >= source.joints_count
                && ibm->type == cgltf_type_mat4) {
                for (cgltf_size j = 0; j < source.joints_count; ++j) {
                    cgltf_accessor_read_float(ibm, j,
                        &model.skin.inverseBindMatrices[j * 16], 16);
                }
            } else {
                // Skin without IBM accessor: invert the joint world transforms.
                for (cgltf_size j = 0; j < source.joints_count; ++j) {
                    float world[16];
                    cgltf_node_transform_world(source.joints[j], world);
                    InvertMatrix16(world, &model.skin.inverseBindMatrices[j * 16]);
                }
            }
            SDL_Log("%s skin_ready joints=%u ibm_from_accessor=%d", kLogTag,
                static_cast<unsigned int>(source.joints_count), ibm != nullptr ? 1 : 0);
        } else {
            model.skin.jointNodes.clear();
            model.skin.inverseBindMatrices.clear();
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s skin_joints_outside_scene", kLogTag);
        }
    }

    // ---- animations (preloaded, cgltf-independent) ---------------------------
    if (ok) {
        for (cgltf_size a = 0; a < data->animations_count; ++a) {
            const cgltf_animation& source = data->animations[a];
            AnimationClip clip;
            clip.name = source.name != nullptr ? source.name : "";
            float duration = 0.0f;
            bool clipOk = true;
            for (cgltf_size ch = 0; clipOk && ch < source.channels_count; ++ch) {
                const cgltf_animation_channel& channel = source.channels[ch];
                int path = -1;
                if (channel.target_path == cgltf_animation_path_type_translation) {
                    path = 0;
                } else if (channel.target_path == cgltf_animation_path_type_rotation) {
                    path = 1;
                } else if (channel.target_path == cgltf_animation_path_type_scale) {
                    path = 2;
                }
                if (path < 0 || channel.target_node == nullptr || channel.sampler == nullptr) {
                    continue;
                }
                const auto it = nodeLookup.find(channel.target_node);
                if (it == nodeLookup.end()) {
                    continue;
                }
                const cgltf_animation_sampler& sampler = *channel.sampler;
                if (sampler.input == nullptr || sampler.output == nullptr
                    || sampler.input->count == 0) {
                    continue;
                }
                AnimationChannel out;
                out.nodeIndex = it->second;
                out.path = path;
                out.interpolation =
                    sampler.interpolation == cgltf_interpolation_type_step ? 1 : 0;
                const cgltf_size frames = sampler.input->count;
                out.times.resize(frames);
                for (cgltf_size f = 0; f < frames; ++f) {
                    cgltf_accessor_read_float(sampler.input, f, &out.times[f], 1);
                }
                if (out.times[frames - 1] > duration) {
                    duration = out.times[frames - 1];
                }
                const cgltf_size comps = path == 1 ? 4 : 3;
                out.values.resize(frames * comps);
                if (sampler.interpolation == cgltf_interpolation_type_cubic_spline) {
                    // CUBICSPLINE stores (in-tangent, value, out-tangent)
                    // triples per key; approximate by keeping only the key
                    // values and interpolating linearly between them.
                    for (cgltf_size f = 0; f < frames; ++f) {
                        float triple[12] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                        cgltf_accessor_read_float(sampler.output, f, triple, comps * 3);
                        for (cgltf_size c = 0; c < comps; ++c) {
                            out.values[f * comps + c] = triple[comps + c];
                        }
                    }
                } else {
                    for (cgltf_size f = 0; f < frames; ++f) {
                        cgltf_accessor_read_float(sampler.output, f,
                            &out.values[f * comps], comps);
                    }
                }
                clip.channels.push_back(std::move(out));
            }
            clip.duration = duration;
            if (!clip.channels.empty()) {
                model.animations.push_back(std::move(clip));
            }
            if (model.animations.size() > 64) {
                break;  // safety cap; the test asset carries 46 clips
            }
        }
        SDL_Log("%s animations_ready clips=%u", kLogTag,
            static_cast<unsigned int>(model.animations.size()));
    }

    // ---- geometry --------------------------------------------------------
    if (ok) {
        std::vector<const cgltf_node*> nodes;
        CollectSceneNodes(*data, nodes);
        for (const cgltf_node* node : nodes) {
            if (node->mesh == nullptr) {
                continue;
            }
            float world[16];
            cgltf_node_transform_world(node, world);
            float normalMatrix[9];
            BuildNormalMatrix(world, normalMatrix);

            const cgltf_size meshIndex = static_cast<cgltf_size>(node->mesh - data->meshes);
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& primitive = node->mesh->primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s primitive_skipped mesh=%u mode=%d",
                        kLogTag, static_cast<unsigned int>(meshIndex), static_cast<int>(primitive.type));
                    continue;
                }
                const cgltf_accessor* positions =
                    FindAttribute(primitive, cgltf_attribute_type_position);
                if (positions == nullptr) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s primitive_without_position mesh=%u",
                        kLogTag, static_cast<unsigned int>(meshIndex));
                    continue;
                }
                const cgltf_accessor* normals =
                    FindAttribute(primitive, cgltf_attribute_type_normal);
                const cgltf_accessor* uvs =
                    FindAttribute(primitive, cgltf_attribute_type_texcoord);
                const cgltf_accessor* jointAttr =
                    FindAttribute(primitive, cgltf_attribute_type_joints);
                const cgltf_accessor* weightAttr =
                    FindAttribute(primitive, cgltf_attribute_type_weights);
                // Skinned primitives keep their vertices in raw bind-pose
                // model space: glTF ignores the skinned mesh node's own
                // transform, and the skin matrices are what map the bind-pose
                // vertices into the animated model space.  Baking the node
                // transform here (the static path below) would desync them
                // from the inverse bind matrices.
                const bool primitiveSkinned =
                    jointAttr != nullptr && weightAttr != nullptr && model.skin.JointCount() > 0;
                if (primitiveSkinned) {
                    model.skinned = true;
                }

                Mesh mesh;
                mesh.materialIndex = primitive.material != nullptr ?
                    static_cast<std::uint32_t>(primitive.material - data->materials) : 0;
                mesh.vertices.resize(positions->count);
                for (cgltf_size v = 0; v < positions->count; ++v) {
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_float(positions, v, position, 3);
                    Vertex& vertex = mesh.vertices[v];
                    if (primitiveSkinned) {
                        vertex.position[0] = position[0];
                        vertex.position[1] = position[1];
                        vertex.position[2] = position[2];
                    } else {
                        TransformPoint(world, position, vertex.position);
                    }

                    float uv[2] = {0.0f, 0.0f};
                    if (uvs != nullptr && v < uvs->count) {
                        cgltf_accessor_read_float(uvs, v, uv, 2);
                    }
                    vertex.uv[0] = uv[0];
                    vertex.uv[1] = uv[1];
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 0.0f;
                    vertex.normal[2] = 0.0f;
                }

                // JOINTS_0 (u8 indices into the skin) and WEIGHTS_0, weights
                // renormalised (some assets don't sum exactly to 1) and
                // quantised to u8.  Unused slots stay zero.
                if (primitiveSkinned) {
                    const float jointCount = static_cast<float>(model.skin.JointCount());
                    for (cgltf_size v = 0; v < positions->count; ++v) {
                        float j4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        float w4[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        cgltf_accessor_read_float(jointAttr, v, j4, 4);
                        cgltf_accessor_read_float(weightAttr, v, w4, 4);
                        Vertex& vertex = mesh.vertices[v];
                        const float total = w4[0] + w4[1] + w4[2] + w4[3];
                        for (int c = 0; c < 4; ++c) {
                            float index = j4[c];
                            index = index < 0.0f ? 0.0f : (index > jointCount - 1.0f ? jointCount - 1.0f : index);
                            vertex.joints[c] = static_cast<std::uint8_t>(index + 0.5f);
                            const float weight =
                                total > 1e-6f ? w4[c] / total : 0.0f;
                            vertex.weights[c] = static_cast<std::uint8_t>(
                                std::min(std::max(weight, 0.0f), 1.0f) * 255.0f + 0.5f);
                        }
                    }
                }

                if (primitive.indices != nullptr) {
                    const cgltf_size count = primitive.indices->count;
                    mesh.indices.resize(count);
                    std::uint32_t maxIndex = 0;
                    for (cgltf_size k = 0; k < count; ++k) {
                        const std::uint32_t value =
                            static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, k));
                        mesh.indices[k] = value;
                        maxIndex = std::max(maxIndex, value);
                    }
                    mesh.indexBytes = maxIndex > 0xFFFFu ? 4u : 2u;
                } else {
                    // Non-indexed primitives get a trivial sequential index
                    // buffer so the draw path stays uniform.
                    mesh.indices.resize(positions->count);
                    for (cgltf_size k = 0; k < positions->count; ++k) {
                        mesh.indices[k] = static_cast<std::uint32_t>(k);
                    }
                    mesh.indexBytes = positions->count > 0x10000u ? 4u : 2u;
                }

                if (normals != nullptr && normals->count == positions->count) {
                    for (cgltf_size v = 0; v < positions->count; ++v) {
                        float normal[3] = {0.0f, 1.0f, 0.0f};
                        cgltf_accessor_read_float(normals, v, normal, 3);
                        if (primitiveSkinned) {
                            // Raw bind-pose normal; the vertex shader applies
                            // the skin matrix's 3x3 (and normalises).
                            mesh.vertices[v].normal[0] = normal[0];
                            mesh.vertices[v].normal[1] = normal[1];
                            mesh.vertices[v].normal[2] = normal[2];
                        } else {
                            TransformNormal(normalMatrix, normal, mesh.vertices[v].normal);
                        }
                    }
                } else {
                    // No usable NORMAL stream: derive per-vertex normals by
                    // accumulating the face normals of every touching
                    // triangle.  Positions are already in baked model space,
                    // which is the space the shading wants.
                    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                        Vertex& a = mesh.vertices[mesh.indices[t]];
                        Vertex& b = mesh.vertices[mesh.indices[t + 1]];
                        Vertex& c = mesh.vertices[mesh.indices[t + 2]];
                        const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                            b.position[2] - a.position[2]};
                        const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                            c.position[2] - a.position[2]};
                        const float face[3] = {
                            e1[1] * e2[2] - e1[2] * e2[1],
                            e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0],
                        };
                        for (Vertex* vertex : {&a, &b, &c}) {
                            vertex->normal[0] += face[0];
                            vertex->normal[1] += face[1];
                            vertex->normal[2] += face[2];
                        }
                    }
                    for (Vertex& vertex : mesh.vertices) {
                        const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] +
                            vertex.normal[1] * vertex.normal[1] + vertex.normal[2] * vertex.normal[2]);
                        if (length > 1e-8f) {
                            const float scale = 1.0f / length;
                            vertex.normal[0] *= scale;
                            vertex.normal[1] *= scale;
                            vertex.normal[2] *= scale;
                        } else {
                            vertex.normal[0] = 0.0f;
                            vertex.normal[1] = 1.0f;
                            vertex.normal[2] = 0.0f;
                        }
                    }
                    SDL_Log("%s normals_generated verts=%u", kLogTag,
                        static_cast<unsigned int>(mesh.vertices.size()));
                }

                model.triangleCount += static_cast<std::uint32_t>(mesh.indices.size() / 3);
                model.vertexCount += static_cast<std::uint32_t>(mesh.vertices.size());
                SDL_Log("%s mesh_ready node_mesh=%u prim=%u verts=%u indices=%u indexBytes=%u material=%u",
                    kLogTag, static_cast<unsigned int>(meshIndex), static_cast<unsigned int>(p),
                    static_cast<unsigned int>(mesh.vertices.size()),
                    static_cast<unsigned int>(mesh.indices.size()), mesh.indexBytes, mesh.materialIndex);
                model.meshes.push_back(std::move(mesh));
            }
        }
    }

    // ---- recentre and normalise -----------------------------------------
    if (ok) {
        if (model.meshes.empty()) {
            error = "no drawable primitives";
            ok = false;
        } else {
            float minBounds[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()};
            float maxBounds[3] = {std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
            for (const Mesh& mesh : model.meshes) {
                for (const Vertex& vertex : mesh.vertices) {
                    for (int c = 0; c < 3; ++c) {
                        minBounds[c] = std::min(minBounds[c], vertex.position[c]);
                        maxBounds[c] = std::max(maxBounds[c], vertex.position[c]);
                    }
                }
            }
            SDL_Log("%s bounds_baked min=(%.3f %.3f %.3f) max=(%.3f %.3f %.3f)", kLogTag, minBounds[0],
                minBounds[1], minBounds[2], maxBounds[0], maxBounds[1], maxBounds[2]);

            const float centre[3] = {(minBounds[0] + maxBounds[0]) * 0.5f,
                (minBounds[1] + maxBounds[1]) * 0.5f, (minBounds[2] + maxBounds[2]) * 0.5f};
            float extent = 0.0f;
            for (int c = 0; c < 3; ++c) {
                extent = std::max(extent, maxBounds[c] - minBounds[c]);
            }
            const float scale = extent > 1e-6f ? kNormalizedExtent / extent : 1.0f;
            for (int c = 0; c < 3; ++c) {
                model.minBounds[c] = (minBounds[c] - centre[c]) * scale;
                model.maxBounds[c] = (maxBounds[c] - centre[c]) * scale;
            }
            if (model.skinned) {
                // The vertices must stay in bind-pose space (the inverse bind
                // matrices are relative to it), so the centring/normalisation
                // is exposed as a matrix; the renderer folds it into the
                // joint matrices each frame: joint = normalise * (global * ibm).
                // Column-major T(-centre) * S(scale).
                float* m = model.normaliseMatrix;
                for (int i = 0; i < 16; ++i) {
                    m[i] = 0.0f;
                }
                m[0] = scale;
                m[5] = scale;
                m[10] = scale;
                m[15] = 1.0f;
                m[12] = -centre[0] * scale;
                m[13] = -centre[1] * scale;
                m[14] = -centre[2] * scale;
            } else {
                for (Mesh& mesh : model.meshes) {
                    for (Vertex& vertex : mesh.vertices) {
                        for (int c = 0; c < 3; ++c) {
                            vertex.position[c] = (vertex.position[c] - centre[c]) * scale;
                        }
                    }
                }
            }
            SDL_Log("%s normalised scale=%.6f extent=%.3f bounds=[%.2f %.2f %.2f]-[%.2f %.2f %.2f]",
                kLogTag, scale, extent, model.minBounds[0], model.minBounds[1], model.minBounds[2],
                model.maxBounds[0], model.maxBounds[1], model.maxBounds[2]);
        }
    }

    cgltf_free(data);
    OHOS_FreeRawFile(gltfBytes);

    if (!ok) {
        model.Clear();
        return false;
    }
    SDL_Log("%s model_ready meshes=%u materials=%u images=%u verts=%u tris=%u skinned=%d nodes=%u clips=%u",
        kLogTag,
        static_cast<unsigned int>(model.meshes.size()),
        static_cast<unsigned int>(model.materials.size()),
        static_cast<unsigned int>(model.images.size()), model.vertexCount, model.triangleCount,
        model.skinned ? 1 : 0,
        static_cast<unsigned int>(model.nodes.size()),
        static_cast<unsigned int>(model.animations.size()));
    return true;
}

} // namespace ohos_model
