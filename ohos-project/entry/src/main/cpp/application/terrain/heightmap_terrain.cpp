#include "heightmap_terrain.h"
#include <SDL3/SDL_stdinc.h>
#define STBI_OHOS_TERRAIN
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" int OHOS_ReadRawFile(const char *, void **, size_t *);
extern "C" void OHOS_FreeRawFile(void *);

namespace terrain
{
namespace
{
constexpr unsigned kMaxDimension = 4096;
constexpr unsigned kBakeSize = 512;
constexpr size_t kMaxVertices = 1100000;

struct RawFile
{
    void *data = nullptr;
    size_t size = 0;
    ~RawFile()
    {
        if (data)
            OHOS_FreeRawFile(data);
    }
};

bool Read(const std::string &path, RawFile &file, std::string &error)
{
    // Scene assets are relative to rawfile, never arbitrary workstation paths.
    if (path.empty() || path.front() == '/' || path.find(':') != std::string::npos ||
        path.find("..") != std::string::npos || path.find('\\') != std::string::npos) {
        error = "terrain asset must be a rawfile-relative path: " + path;
        return false;
    }
    if (!OHOS_ReadRawFile(path.c_str(), &file.data, &file.size) ||
        !file.data || file.size == 0 || file.size > 64u * 1024u * 1024u) {
        error = "cannot read terrain rawfile: " + path;
        return false;
    }
    return true;
}

struct Heightmap
{
    int width = 2, height = 2;
    std::vector<uint16_t> pixels = std::vector<uint16_t>(4, 0);
    float Sample(float u, float v) const
    {
        const float x = std::clamp(u, 0.0f, 1.0f) * (width - 1);
        const float y = (1.0f - std::clamp(v, 0.0f, 1.0f)) * (height - 1);
        const int x0 = int(x), y0 = int(y);
        const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
        const auto at = [&](int a, int b) { return pixels[size_t(b) * width + a] / 65535.0f; };
        const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * (x - x0);
        const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * (x - x0);
        return a + (b - a) * (y - y0);
    }
};

bool LoadHeight(const std::string &path, Heightmap &map, std::string &error)
{
    if (path.empty())
        return true; // Mikan's procedural flat terrain.
    RawFile file;
    if (!Read(path, file, error))
        return false;
    const auto *bytes = static_cast<const unsigned char *>(file.data);
    const unsigned char signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (file.size < 33 || std::memcmp(bytes, signature, 8) != 0 ||
        std::memcmp(bytes + 12, "IHDR", 4) != 0 || bytes[24] != 16 ||
        (bytes[25] != 0 && bytes[25] != 4) || bytes[28] != 0) {
        error = "heightmap requires non-interlaced 16-bit grayscale PNG: " + path;
        return false;
    }
    int channels = 0, w = 0, h = 0;
    if (!stbi_info_from_memory(bytes, int(file.size), &w, &h, &channels) ||
        w < 2 || h < 2 || w > int(kMaxDimension) || h > int(kMaxDimension)) {
        error = "heightmap dimensions must be within 2..4096: " + path;
        return false;
    }
    stbi_us *decoded = stbi_load_16_from_memory(bytes, int(file.size), &w, &h, &channels, 1);
    if (!decoded) {
        error = "heightmap PNG decode failed: " + path;
        return false;
    }
    map.width = w;
    map.height = h;
    map.pixels.assign(decoded, decoded + size_t(w) * h);
    stbi_image_free(decoded);
    return true;
}

bool LoadImage(const std::string &path, ohos_model::Image &image, std::string &error)
{
    if (path.empty())
        return true;
    RawFile file;
    if (!Read(path, file, error))
        return false;
    int w = 0, h = 0, channels = 0;
    const auto *bytes = static_cast<const unsigned char *>(file.data);
    if (!stbi_info_from_memory(bytes, int(file.size), &w, &h, &channels) ||
        w < 1 || h < 1 || w > int(kMaxDimension) || h > int(kMaxDimension)) {
        error = "terrain texture dimensions exceed 4096: " + path;
        return false;
    }
    auto *pixels = stbi_load_from_memory(bytes, int(file.size), &w, &h, &channels, 4);
    if (!pixels) {
        error = "terrain texture decode failed: " + path;
        return false;
    }
    image.width = unsigned(w);
    image.height = unsigned(h);
    image.uri = path;
    image.rgba.assign(pixels, pixels + size_t(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

float Channel(const ohos_model::Image &image, float u, float v, int c, bool repeat)
{
    if (image.rgba.empty())
        return 1.0f;
    // Material images follow the same top-left -> terrain UV orientation.
    if (repeat) {
        u -= std::floor(u);
        v -= std::floor(v);
    }
    const float x = std::clamp(u, 0.0f, 1.0f) * (image.width - 1);
    const float y = (1.0f - std::clamp(v, 0.0f, 1.0f)) * (image.height - 1);
    const unsigned x0 = unsigned(x), y0 = unsigned(y);
    const unsigned x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const auto at = [&](unsigned a, unsigned b) { return image.rgba[(size_t(b) * image.width + a) * 4 + c] / 255.0f; };
    const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * (x - x0);
    const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * (x - x0);
    return a + (b - a) * (y - y0);
}

void Normalize(float n[3])
{
    const float len = std::hypot(std::hypot(n[0], n[1]), n[2]);
    for (int i = 0; i < 3; ++i)
        n[i] /= std::max(len, 1e-20f);
}

void Normal(const Heightmap &map, const scene::Terrain &t, float u, float v, float n[3])
{
    const float du = 1.0f / (map.width - 1), dv = 1.0f / (map.height - 1);
    n[0] = -(map.Sample(u + du, v) - map.Sample(u - du, v)) * t.heightScale / (2 * du * t.worldSizeX);
    n[1] = 1.0f;
    n[2] = -(map.Sample(u, v + dv) - map.Sample(u, v - dv)) * t.heightScale / (2 * dv * t.worldSizeZ);
    Normalize(n);
}

float Smooth(float a, float b, float x)
{
    x = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3 - 2 * x);
}

bool Append(const scene::Definition &definition, const scene::Entity &entity,
            float groundY, ohos_model::Model &out, std::string &error, bool collisionOnly)
{
    const auto &t = entity.terrain;
    Heightmap heights;
    if (!LoadHeight(t.sculptedHeightmapPath.empty() ? t.heightmapPath : t.sculptedHeightmapPath, heights, error))
        return false;
    float matrix[16];
    // Compose authored parent matrices before applying the shared runtime offset.
    scene::BuildModelMatrix(entity.transform, matrix);
    uint32_t parentId = entity.parentId;
    size_t depth = 0;
    while (parentId != scene::kNoParent) {
        if (++depth > definition.entities.size()) {
            error = "terrain hierarchy cycle";
            return false;
        }
        const auto it = std::find_if(definition.entities.begin(), definition.entities.end(),
                                     [parentId](const scene::Entity &e) { return e.id == parentId; });
        if (it == definition.entities.end()) {
            error = "terrain parent not found";
            return false;
        }
        float parent[16], result[16]{};
        scene::BuildModelMatrix(it->transform, parent);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    result[c * 4 + r] += parent[k * 4 + r] * matrix[c * 4 + k];
        std::copy(result, result + 16, matrix);
        parentId = it->parentId;
    }
    if (definition.hasGroundSurface)
        matrix[13] += groundY - definition.authoredGroundTopY;
    float cof[9] = {
        matrix[5] * matrix[10] - matrix[9] * matrix[6], matrix[9] * matrix[2] - matrix[1] * matrix[10], matrix[1] * matrix[6] - matrix[5] * matrix[2],
        matrix[8] * matrix[6] - matrix[4] * matrix[10], matrix[0] * matrix[10] - matrix[8] * matrix[2], matrix[4] * matrix[2] - matrix[0] * matrix[6],
        matrix[4] * matrix[9] - matrix[8] * matrix[5], matrix[8] * matrix[1] - matrix[0] * matrix[9], matrix[0] * matrix[5] - matrix[4] * matrix[1]
    };
    const float determinant = matrix[0] * cof[0] + matrix[4] * cof[1] + matrix[8] * cof[2];
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-10f) {
        error = "terrain has singular transform";
        return false;
    }
    const auto worldNormal = [&](float *n) {
        float result[3]{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                result[r] += cof[r * 3 + c] * n[c] / determinant;
        std::copy(result, result + 3, n);
        Normalize(n);
        return std::isfinite(n[0]) && std::isfinite(n[1]) && std::isfinite(n[2]);
    };
    const uint32_t material = uint32_t(out.materials.size());
    ohos_model::Material mat;
    mat.metallicFactor = 0;
    mat.roughnessFactor = 0.85f;
    if (!collisionOnly) {
        ohos_model::Image layers[4], control;
        for (int i = 0; i < 4; ++i)
            if (!LoadImage(t.layerPaths[i], layers[i], error))
                return false;
        if (!LoadImage(t.paintedControlMapPath.empty() ? t.controlMapPath : t.paintedControlMapPath, control, error))
            return false;
        ohos_model::Image baked;
        baked.uri = "terrain-baked-albedo";
        baked.width = baked.height = kBakeSize;
        baked.rgba.resize(kBakeSize * kBakeSize * 4);
        for (unsigned y = 0; y < kBakeSize; ++y)
            for (unsigned x = 0; x < kBakeSize; ++x) {
                const float u = float(x) / (kBakeSize - 1), v = float(y) / (kBakeSize - 1);
                float n[3];
                Normal(heights, t, u, v, n);
                if (!worldNormal(n)) {
                    error = "terrain parameters produced non-finite normal";
                    return false;
                }
                const float slope = std::clamp(1 - n[1], 0.0f, 1.0f);
                // Smooth grass -> soil -> rock transitions, in world slope.
                // Partition of unity keeps each authored material's color intact.
                const float a = Smooth(0.06f, 0.20f, slope), b = Smooth(0.32f, 0.50f, slope);
                float w[4] = { (1 - a) * (1 - b), b, a * (1 - b), 0 };
                float total = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!control.rgba.empty())
                        w[i] = Channel(control, u, v, i, false);
                    w[i] = std::pow(w[i], std::max(0.01f, t.blendSharpness));
                    total += w[i];
                }
                if (total < 1e-8f) {
                    w[0] = 1;
                    total = 1;
                }
                float pos[3] = { (u - 0.5f) * t.worldSizeX, heights.Sample(u, v) * t.heightScale + t.heightOffset, (v - 0.5f) * t.worldSizeZ };
                const float wx = matrix[0] * pos[0] + matrix[4] * pos[1] + matrix[8] * pos[2] + matrix[12];
                const float wz = matrix[2] * pos[0] + matrix[6] * pos[1] + matrix[10] * pos[2] + matrix[14];
                const float materialU = wx * t.materialTiling / t.worldSizeX + 0.5f * t.materialTiling;
                const float materialV = wz * t.materialTiling / t.worldSizeZ + 0.5f * t.materialTiling;
                if (!std::isfinite(materialU) || !std::isfinite(materialV)) {
                    error = "terrain transform produced non-finite texture coordinate";
                    return false;
                }
                for (int c = 0; c < 3; ++c) {
                    float color = 0;
                    for (int i = 0; i < 4; ++i) {
                        const float srgb = Channel(layers[i], materialU, materialV, c, true);
                        const float linear = srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
                        color += linear * w[i] / total;
                    }
                    const float srgb = color <= 0.0031308f ? color * 12.92f : 1.055f * std::pow(color, 1 / 2.4f) - 0.055f;
                    baked.rgba[(size_t(y) * kBakeSize + x) * 4 + c] = uint8_t(std::clamp(srgb, 0.0f, 1.0f) * 255 + 0.5f);
                }
                baked.rgba[(size_t(y) * kBakeSize + x) * 4 + 3] = 255;
            }
        mat.baseColorImage = int(out.images.size());
        out.images.push_back(std::move(baked));
    }
    out.materials.push_back(mat);
    const int chunks = collisionOnly ? 1 : t.chunkCount;
    const int intervals = collisionOnly ? std::min(t.collisionResolution - 1, t.chunkCount * (t.patchResolution - 1)) : t.patchResolution - 1;
    if (size_t(out.vertexCount) + size_t(chunks) * chunks * (intervals + 1) * (intervals + 1) > kMaxVertices) {
        error = "terrain scene vertex budget exceeded";
        return false;
    }
    for (int cz = 0; cz < chunks; ++cz)
        for (int cx = 0; cx < chunks; ++cx) {
            ohos_model::Mesh mesh;
            mesh.materialIndex = material;
            mesh.castShadow = entity.castShadow;
            mesh.vertices.reserve(size_t(intervals + 1) * (intervals + 1));
            for (int z = 0; z <= intervals; ++z)
                for (int x = 0; x <= intervals; ++x) {
                    const float u = float(cx * intervals + x) / float(chunks * intervals), v = float(cz * intervals + z) / float(chunks * intervals);
                    const float pos[3] = { (u - 0.5f) * t.worldSizeX, heights.Sample(u, v) * t.heightScale + t.heightOffset, (v - 0.5f) * t.worldSizeZ };
                    ohos_model::Vertex vertex{};
                    for (int r = 0; r < 3; ++r)
                        vertex.position[r] = matrix[r] * pos[0] + matrix[4 + r] * pos[1] + matrix[8 + r] * pos[2] + matrix[12 + r];
                    Normal(heights, t, u, v, vertex.normal);
                    if (!worldNormal(vertex.normal)) {
                        error = "terrain parameters produced non-finite normal";
                        return false;
                    }
                    // Existing model samplers repeat. Keep terrain sampling inside the
                    // baked texel centers so opposite outer edges never wrap together.
                    vertex.uv[0] = (u * (kBakeSize - 1) + 0.5f) / kBakeSize;
                    vertex.uv[1] = (v * (kBakeSize - 1) + 0.5f) / kBakeSize;
                    for (float value : vertex.position) {
                        if (!std::isfinite(value)) {
                            error = "terrain transform produced non-finite position";
                            return false;
                        }
                    }
                    mesh.vertices.push_back(vertex);
                }
            for (int z = 0; z < intervals; ++z)
                for (int x = 0; x < intervals; ++x) {
                    const uint32_t a = uint32_t(z * (intervals + 1) + x), b = a + 1, c = a + intervals + 1, d = c + 1;
                    const uint32_t indices[6] = { a, c, b, b, c, d };
                    for (int i = 0; i < 6; i += 3) {
                        mesh.indices.push_back(indices[i]);
                        mesh.indices.push_back(indices[i + (determinant < 0 ? 2 : 1)]);
                        mesh.indices.push_back(indices[i + (determinant < 0 ? 1 : 2)]);
                    }
                }
            mesh.indexBytes = mesh.vertices.size() > 65536 ? 4 : 2;
            out.vertexCount += uint32_t(mesh.vertices.size());
            out.triangleCount += uint32_t(mesh.indices.size() / 3);
            out.meshes.push_back(std::move(mesh));
        }
    return true;
}
} // namespace

bool BuildModel(const scene::Definition &definition, float groundY,
                ohos_model::Model &output, std::string &error, bool collisionOnly)
{
    ohos_model::Model candidate;
    size_t count = 0;
    for (const auto &entity : definition.entities) {
        if (!entity.terrain.present || !entity.terrain.enabled ||
            (collisionOnly ? !entity.terrain.collisionEnabled : !entity.visible))
            continue;
        const auto &t = entity.terrain;
        if (!std::isfinite(t.worldSizeX) || !std::isfinite(t.worldSizeZ) ||
            t.worldSizeX <= 0 || t.worldSizeZ <= 0 || t.chunkCount < 1 || t.chunkCount > 16 ||
            t.patchResolution < 2 || t.patchResolution > 129 ||
            t.chunkCount * (t.patchResolution - 1) > 512 || t.collisionResolution < 2 ||
            t.collisionResolution > 513 || !std::isfinite(t.heightScale) ||
            !std::isfinite(t.heightOffset) || !std::isfinite(t.materialTiling) ||
            std::fabs(t.materialTiling) > 1024 || !std::isfinite(t.blendSharpness) || t.blendSharpness <= 0) {
            error = entity.name + ": invalid terrain parameters";
            return false;
        }
        if (++count > 4) {
            error = "OHOS supports at most four terrain entities";
            return false;
        }
        if (!Append(definition, entity, groundY, candidate, error, collisionOnly)) {
            error = entity.name + ": " + error;
            return false;
        }
    }
    output = std::move(candidate);
    error.clear();
    return true;
}
} // namespace terrain
