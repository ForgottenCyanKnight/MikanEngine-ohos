#include "terrain/heightmap_terrain.h"
#include "water/water_surface.h"
#include <SDL3/SDL_stdinc.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// The host harness uses the real stb PNG decoder and real terrain implementation.
// It replaces only platform rawfile IO and SDL's decoder-error reporting.
static bool SDL_SetError(const char *, ...) { return false; }
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STBI_OHOS_TERRAIN
#include "stb_image.h"

static std::string root;
extern "C" int OHOS_ReadRawFile(const char *path, void **data, size_t *size)
{
    std::ifstream file(root + "/" + path, std::ios::binary);
    if (!file)
        return 0;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    *size = bytes.size();
    *data = std::malloc(*size);
    if (!*data)
        return 0;
    std::memcpy(*data, bytes.data(), *size);
    return 1;
}
extern "C" void OHOS_FreeRawFile(void *data) { std::free(data); }

static int checks = 0;
static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.0003f; }

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    root = argv[1];
    scene::Definition definition;
    scene::Entity entity;
    entity.id = 1;
    entity.name = "test terrain";
    entity.terrain.present = true;
    auto &t = entity.terrain;
    t.heightmapPath = "gradient16.png";
    t.worldSizeX = 4;
    t.worldSizeZ = 2;
    t.heightScale = 10;
    t.heightOffset = 2;
    t.chunkCount = 2;
    t.patchResolution = 3;
    t.layerPaths[0] = "red.png";
    t.layerPaths[1] = "blue.png";
    t.controlMapPath = "control.png";
    definition.entities.push_back(entity);
    ohos_model::Model model;
    std::string error;
    Check(terrain::BuildModel(definition, 0, model, error), "load 16-bit grayscale PNG");
    Check(model.meshes.size() == 4 && model.vertexCount == 36 && model.triangleCount == 32, "chunk topology");
    const auto &first = model.meshes[0].vertices[0];
    const auto localNormal = model.meshes[0].vertices[4];
    Check(Near(first.position[0], -2) && Near(first.position[2], -1) && Near(first.position[1], 12), "bottom PNG row maps to -Z");
    Check(Near(model.meshes[2].vertices[6].position[1], 2), "top PNG row maps to +Z");
    for (int i = 0; i < 3; ++i) {
        const auto &a = model.meshes[0].vertices[size_t(i) * 3 + 2];
        const auto &b = model.meshes[1].vertices[size_t(i) * 3];
        Check(std::memcmp(a.position, b.position, sizeof(a.position)) == 0, "bit-identical chunk edge positions");
        Check(std::memcmp(a.normal, b.normal, sizeof(a.normal)) == 0, "continuous chunk edge normals");
    }
    const auto &color = model.images[0].rgba;
    Check(color[0] == 255 && color[1] == 0 && color[2] == 0 && color[3] == 255, "RGBA control selects authored layer without tint");
    Check(first.uv[0] > 0 && first.uv[1] > 0 && model.meshes.back().vertices.back().uv[0] < 1,
          "baked texel center UVs avoid repeat-sampler border wrapping");
    for (const auto &mesh : model.meshes)
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            const auto &a = mesh.vertices[mesh.indices[i]];
            const auto &b = mesh.vertices[mesh.indices[i + 1]];
            const auto &c = mesh.vertices[mesh.indices[i + 2]];
            const float y = (b.position[2] - a.position[2]) * (c.position[0] - a.position[0]) - (b.position[0] - a.position[0]) * (c.position[2] - a.position[2]);
            Check(y > 0, "upward winding");
        }
    Check(terrain::BuildModel(definition, 0, model, error, true) && model.images.empty() && model.meshes.size() == 1, "collision uses same geometry domain without textures");
    Check(Near(model.meshes[0].vertices[0].position[1], 12), "collision orientation matches rendering");
    definition.entities[0].terrain.controlMapPath = "mix_control.png";
    Check(terrain::BuildModel(definition, 0, model, error), "load mixed control map");
    Check(model.images[0].rgba[0] >= 187 && model.images[0].rgba[0] <= 189 &&
              model.images[0].rgba[2] >= 187 && model.images[0].rgba[2] <= 189,
          "layers blend in linear light then encode sRGB");
    definition.entities[0].terrain.collisionEnabled = false;
    Check(terrain::BuildModel(definition, 0, model, error, true) && model.meshes.empty(), "collision disabled");
    definition.entities[0].terrain.enabled = false;
    Check(terrain::BuildModel(definition, 0, model, error) && model.meshes.empty(), "terrain disabled");
    definition.entities[0] = entity;
    definition.entities[0].terrain.heightmapPath = "invalid8.png";
    Check(!terrain::BuildModel(definition, 0, model, error), "reject 8-bit height loss");
    definition.entities[0].terrain.heightmapPath = "../gradient16.png";
    Check(!terrain::BuildModel(definition, 0, model, error), "reject path traversal");
    definition.entities[0] = entity;
    definition.entities[0].terrain.sculptedHeightmapPath = "missing.png";
    Check(!terrain::BuildModel(definition, 0, model, error), "sculpted heightmap takes precedence");
    definition.entities[0] = entity;
    definition.entities[0].terrain.heightmapPath.clear();
    definition.entities[0].terrain.layerPaths[0].clear();
    definition.entities[0].terrain.layerPaths[1].clear();
    definition.entities[0].terrain.controlMapPath.clear();
    Check(terrain::BuildModel(definition, 0, model, error), "procedural flat terrain");
    Check(Near(model.meshes[0].vertices[0].position[1], 2) && Near(model.meshes[0].vertices[0].normal[1], 1), "flat heightOffset and normal");
    definition.entities[0] = entity;
    definition.entities[0].transform.scale = { -2, 3, 4 };
    definition.entities[0].transform.rotation = { 0, 0.38268343f, 0, 0.92387953f };
    Check(terrain::BuildModel(definition, 0, model, error), "rotated mirrored nonuniform transform");
    const float invX = localNormal.normal[0] / -2, invY = localNormal.normal[1] / 3, invZ = localNormal.normal[2] / 4;
    const float expectedX = 0.70710678f * (invX + invZ), expectedZ = 0.70710678f * (-invX + invZ);
    const float length = std::sqrt(expectedX * expectedX + invY * invY + expectedZ * expectedZ);
    const auto &actual = model.meshes[0].vertices[4];
    Check(Near(actual.normal[0], expectedX / length) && Near(actual.normal[1], invY / length) &&
              Near(actual.normal[2], expectedZ / length),
          "inverse-transpose normal matches analytic reference");
    for (const auto &mesh : model.meshes) {
        const auto &a = mesh.vertices[0];
        Check(Near(a.normal[0] * a.normal[0] + a.normal[1] * a.normal[1] + a.normal[2] * a.normal[2], 1), "world normals normalized");
        Check(a.normal[1] > 0, "mirrored scale keeps normal on upper surface");
    }
    definition.entities[0] = entity;
    definition.entities[0].parentId = 2;
    scene::Entity parent;
    parent.id = 2;
    parent.transform.position = { 10, 3, 5 };
    definition.entities.push_back(parent);
    Check(terrain::BuildModel(definition, 0, model, error) && Near(model.meshes[0].vertices[0].position[0], 8), "parent transform composition");
    definition.entities[1].parentId = 1;
    Check(!terrain::BuildModel(definition, 0, model, error), "hierarchy cycle rejected");
    definition.entities = { entity };
    definition.entities[0].castShadow = false;
    Check(terrain::BuildModel(definition, 0, model, error) && !model.meshes[0].castShadow,
          "castShadow preserved for both backend shadow paths");
    definition.entities[0].visible = false;
    Check(terrain::BuildModel(definition, 0, model, error) && model.meshes.empty(), "invisible terrain not rendered");
    Check(terrain::BuildModel(definition, 0, model, error, true) && !model.meshes.empty(), "invisible collision remains independent");
    definition.entities = { entity };
    definition.entities[0].terrain.chunkCount = 1000;
    Check(!terrain::BuildModel(definition, 0, model, error), "direct builder enforces budgets");
    const char *json = R"({"formatVersion":1,"entities":[{"id":1,"terrain":{"worldSize":[4,2],"chunkCount":2,"patchResolution":3,"heightmapPath":"gradient16.png","layer0Path":"red.png"}}]})";
    Check(scene::ParseMikanScene(json, std::strlen(json), definition, error), "Mikan scene terrain parser");
    Check(definition.entities[0].terrain.worldSizeZ == 2 && definition.entities[0].terrain.layerPaths[0] == "red.png", "terrain fields preserved");
    const char *invalid = R"({"formatVersion":1,"entities":[{"terrain":{"chunkCount":1000}}]})";
    Check(!scene::ParseMikanScene(invalid, std::strlen(invalid), definition, error), "scene geometry budget rejected");
    Check(scene::LoadMikanSceneRawFile("scenes/terrain_demo.json", definition, error), "packaged demo parses");
    Check(terrain::BuildModel(definition, 0, model, error) && model.meshes.size() == 16 &&
              model.vertexCount == 4624 && model.triangleCount == 8192,
          "packaged demo render topology");
    Check(Near(model.meshes[5].vertices.back().position[1], 1.46f), "packaged demo peak height");
    Check(terrain::BuildModel(definition, 0, model, error, true) && model.vertexCount == 4225 &&
              model.triangleCount == 8192,
          "packaged demo collision topology");
    Check(scene::LoadMikanSceneRawFile("scenes/default_main.json", definition, error), "default scene parses");
    Check(definition.hasGroundSurface && definition.StaticCubeCount() > 0, "default cube floor and props preserved");
    Check(terrain::BuildModel(definition, -0.94f, model, error) && !model.meshes.empty(), "default terrain renders");
    float minY = 1e20f, maxY = -1e20f, minX = 1e20f, maxX = -1e20f;
    for (const auto& mesh : model.meshes) for (const auto& vertex : mesh.vertices) {
        minY = std::min(minY, vertex.position[1]); maxY = std::max(maxY, vertex.position[1]);
        minX = std::min(minX, vertex.position[0]); maxX = std::max(maxX, vertex.position[0]);
        if (std::fabs(vertex.position[0]) <= 21 && std::fabs(vertex.position[2]) <= 21) {
            Check(Near(vertex.position[1], -1.34f), "entire floor footprint and apron sit on the support plane");
        }
    }
    Check(Near(minY, -6.5f) && maxY > 2.8f && maxY < 3.1f, "archipelago has deep seabed and elevated island peaks");
    Check(Near(minX, -96) && Near(maxX, 96), "expanded terrain surrounds the cube floor");
    const float islandCenters[5][2] = {{-54,-43},{54,-46},{57,43},{-51,48},{0,69}};
    for (const auto& center : islandCenters) {
        float distance = 1e20f, height = -100;
        for (const auto& mesh : model.meshes) for (const auto& vertex : mesh.vertices) {
            const float dx = vertex.position[0] - center[0], dz = vertex.position[2] - center[1];
            if (dx*dx + dz*dz < distance) { distance = dx*dx + dz*dz; height = vertex.position[1]; }
        }
        Check(distance < 2 && height > 0.8f, "each of five surrounding islands rises above sea level");
    }
    size_t grassPixels=0, soilPixels=0, rockPixels=0;
    for (const auto& image : model.images) for (size_t i=0; i<image.rgba.size(); i+=4) {
        const int r=image.rgba[i], g=image.rgba[i+1], b=image.rgba[i+2];
        grassPixels += g > r+15 && g > b+15;
        soilPixels += r > g+10 && g > b+10;
        rockPixels += b >= g && g >= r && r > 100;
    }
    Check(grassPixels > 100 && grassPixels > rockPixels && soilPixels > 100,
          "baked slope materials include grass, transitional soil and steep rock");
    Check(terrain::BuildModel(definition, -0.94f, model, error, true) && !model.meshes.empty(), "default terrain collision builds");
    for (const auto& mesh : model.meshes) for (const auto& vertex : mesh.vertices) {
        if (std::fabs(vertex.position[0]) <= 21 && std::fabs(vertex.position[2]) <= 21) {
            Check(Near(vertex.position[1], -1.34f), "collision support plane matches rendered terrain under whole floor");
        }
    }
    const auto collisionVertexCount = model.vertexCount;
    Check(definition.water.enabled && Near(definition.water.height, -2.6f), "default sea level parsed in runtime coordinates");
    ohos_model::Model sea;
    water::AppendSurface(definition, sea);
    Check(sea.meshes.size() == 1 && sea.vertexCount == 4 && sea.triangleCount == 4, "two-sided sea plane topology");
    Check(sea.materials[0].waterSurface && !sea.meshes[0].castShadow, "water shader flag and no shadow casting");
    for (const auto& vertex : sea.meshes[0].vertices)
        Check(Near(vertex.position[1], -2.6f) && Near(std::fabs(vertex.position[0]), 192) &&
              Near(std::fabs(vertex.position[2]), 192), "sea plane size and fixed height");
    Check(terrain::BuildModel(definition, -0.94f, model, error, true) && model.vertexCount == collisionVertexCount,
          "visual water does not create a solid physics plane");
    const auto terrainMeshes = model.meshes.size();
    const auto terrainMaterials = model.materials.size();
    water::AppendSurface(definition, model);
    Check(model.meshes.size() == terrainMeshes + 1 && model.meshes.back().materialIndex == terrainMaterials,
          "sea appends without disturbing terrain material indices");
    definition.water.enabled = false;
    sea = {};
    water::AppendSurface(definition, sea);
    Check(sea.meshes.empty(), "disabled water produces no geometry");
    const char* noWater = R"({"formatVersion":1,"entities":[]})";
    Check(scene::ParseMikanScene(noWater, std::strlen(noWater), definition, error) && !definition.water.enabled,
          "legacy scenes have no implicit sea");
    const char* badWater = R"({"formatVersion":1,"entities":[],"water":{"size":-1}})";
    Check(!scene::ParseMikanScene(badWater, std::strlen(badWater), definition, error), "invalid water extent rejected");
    std::printf("PASS: %d terrain and water checks\n", checks);
}
