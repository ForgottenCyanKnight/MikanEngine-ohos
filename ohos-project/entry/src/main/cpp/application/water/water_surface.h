#pragma once

#include "../model_loader.h"
#include "../scene/scene_definition.h"
#include <cmath>

namespace water {
// Column-major inverse for fullscreen depth reconstruction, shared by RHIs.
inline bool InverseViewProjection(const float* matrix, float* inverse)
{
    double rows[4][8]{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            rows[r][c] = matrix[c * 4 + r];
            rows[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(rows[r][c]) > std::fabs(rows[pivot][c])) pivot = r;
        if (!std::isfinite(rows[pivot][c]) || std::fabs(rows[pivot][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(rows[c][k], rows[pivot][k]);
        const double scale = rows[c][c];
        for (int k = 0; k < 8; ++k) rows[c][k] /= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = rows[r][c];
            for (int k = 0; k < 8; ++k) rows[r][k] -= factor * rows[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inverse[c * 4 + r] = static_cast<float>(rows[r][c + 4]);
    return true;
}

// Waist-depth entry with a shallower exit threshold prevents shoreline jitter.
// feetY is the physics capsule base, not the render model's centre.
inline bool PlayerSubmerged(const scene::Definition& definition, float x, float feetY,
                            float z, bool wasSwimming)
{
    const auto& water = definition.water;
    const float half = water.size * 0.5f;
    if (!water.enabled || std::fabs(x) > half || std::fabs(z) > half) return false;
    return water.height - feetY >= (wasSwimming ? 0.65f : 0.85f);
}

// Optional CPU plane description used by geometry validation. Rendering uses
// fullscreen depth/plane intersection so opaque bottom color is preserved.
// Never enters terrain::BuildModel(collisionOnly): water has no solid collider.
inline void AppendSurface(const scene::Definition& definition, ohos_model::Model& model)
{
    const auto& water = definition.water;
    if (!water.enabled) return;
    ohos_model::Material material;
    material.name = "sea-plane";
    material.waterSurface = true;
    material.metallicFactor = 0;
    material.roughnessFactor = water.roughness;
    material.baseColorFactor[0] = water.color.x;
    material.baseColorFactor[1] = water.color.y;
    material.baseColorFactor[2] = water.color.z;
    ohos_model::Mesh mesh;
    mesh.materialIndex = static_cast<uint32_t>(model.materials.size());
    mesh.castShadow = false;
    const float half = water.size * 0.5f;
    for (int z = 0; z < 2; ++z)
        for (int x = 0; x < 2; ++x) {
            ohos_model::Vertex vertex{};
            vertex.position[0] = x ? half : -half;
            vertex.position[1] = water.height;
            vertex.position[2] = z ? half : -half;
            vertex.normal[1] = 1;
            vertex.uv[0] = float(x);
            vertex.uv[1] = float(z);
            mesh.vertices.push_back(vertex);
        }
    mesh.indices = {0, 2, 1, 1, 2, 3};
    // The sea is also visible from below; the shader faces its normal to the
    // viewer. Duplicate reverse triangles allow both existing backface modes.
    mesh.indices.insert(mesh.indices.end(), {0, 1, 2, 1, 3, 2});
    model.materials.push_back(std::move(material));
    model.meshes.push_back(std::move(mesh));
    model.vertexCount += 4;
    model.triangleCount += 4;
}
} // namespace water
