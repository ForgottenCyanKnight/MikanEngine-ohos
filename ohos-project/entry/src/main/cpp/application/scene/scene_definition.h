#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scene {

constexpr std::uint32_t kNoParent = 0xffffffffu;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform {
    Vec3 position{};
    Quaternion rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Mikan material data is retained as authored metadata. Runtime material
// texture binding is intentionally a later adapter step; the current static
// cube entities use the material embedded in SDL's matching cube GLB.
struct Material {
    bool present = false;
    std::string albedoPath;
    std::string normalPath;
    std::string roughnessPath;
    std::string metallicPath;
    std::string aoPath;
    std::string emissivePath;
    Vec3 albedoColor{1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ambientOcclusion = 1.0f;
    float emissiveIntensity = 0.0f;
    bool useAlbedoTexture = false;
    bool useNormalTexture = false;
    bool useRoughnessTexture = false;
    bool useMetallicTexture = false;
    bool useAoTexture = false;
    bool useEmissiveTexture = false;
};

struct ColliderSettings {
    bool present = false;
    int shapeType = 0; // Mikan: 0 box, 1 sphere, 2 capsule.
    Vec3 size{2.0f, 2.0f, 2.0f};
    Vec3 offset{};
    bool isTrigger = false;
    bool useOBB = false;
    bool syncWithModel = true;
    bool autoFitToModel = false;
};

struct StaticBoxCollider {
    std::string entityName;
    Vec3 center{};
    Vec3 halfExtents{1.0f, 1.0f, 1.0f};
    Quaternion rotation{};
};

struct Terrain {
    bool present = false;
    bool enabled = true;
    std::string heightmapPath;
    std::string sculptedHeightmapPath;
    float worldSizeX = 256.0f;
    float worldSizeZ = 256.0f;
    float heightScale = 64.0f;
    float heightOffset = 0.0f;
    int chunkCount = 8;
    int patchResolution = 33;
    bool collisionEnabled = true;
    int collisionResolution = 257;
    float materialTiling = 8.0f;
    float blendSharpness = 1.0f;
    std::string layerPaths[4];
    std::string controlMapPath;
    std::string paintedControlMapPath;
};

enum class MeshKind {
    None,
    UnitCube,
    StaticModel,
    Unsupported,
};

struct Entity {
    std::uint32_t id = 0;
    std::uint32_t parentId = kNoParent;
    bool dynamicBody = false;
    std::uint32_t motionParentId = kNoParent; // World-space trim follows a translating platform.
    std::string name;
    Transform transform{};
    bool hasMesh = false;
    std::string modelPath;
    MeshKind meshKind = MeshKind::None;
    bool visible = true;
    bool castShadow = true;
    bool receiveShadow = true;
    bool hasRigidBody = false;
    int rigidBodyType = -1;
    ColliderSettings collider{};
    bool groundSurface = false;
    bool importAsStaticCube = false;
    bool importAsStaticModel = false;
    Material material{};
    Terrain terrain{};
};

struct Camera {
    bool present = false;
    bool isMainCamera = false;
    float fieldOfViewDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    bool thirdPersonEnabled = false;
    std::string thirdPersonTargetName;
    Vec3 thirdPersonTargetOffset{};
    float thirdPersonDistance = 4.0f;
    float thirdPersonYawDegrees = 0.0f;
    float thirdPersonPitchDegrees = 0.0f;
};

struct Light {
    int type = 0; // Mikan: 0 directional, 1 point, 2 spot.
    Transform transform{};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngleDegrees = 45.0f;
    bool castShadow = false;
};

struct Skybox {
    bool present = false;
    bool enabled = true;
    std::string textureName;
    Vec3 tint{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

// OHOS fixed sea plane, in runtime world coordinates (no ground alignment).
struct WaterSurface {
    bool enabled = false;
    float height = -2.6f;
    float size = 128.0f;
    Vec3 center{}; // X/Z centre; height still controls the water plane.
    Vec3 halfExtents{}; // Zero X/Z keeps the original square size contract.
    float roughness = 0.08f;
    Vec3 color{0.035f, 0.18f, 0.22f}; // sRGB body tint
};

struct Portal {
    std::string target;
    std::string label;
    Vec3 position{};
    float radius = 0.9f;
    Vec3 color{0.08f, 0.85f, 0.4f};
    float opacity = 0.32f;
};

struct CourseCheckpoint {
    Vec3 spawn{}; // Render origin, as with playerSpawn.
    Vec3 halfExtents{1, 0.2f, 1}; // Grounded activation area around spawn.
};

struct ParkourCourse {
    bool enabled = false;
    float fallY = 0;
    int revision = 1;
    int completionReward = 0;
    std::vector<CourseCheckpoint> checkpoints; // Start, rest stops, finish.
};

struct Definition {
    std::string sourcePath; // Runtime identity, set by the rawfile loader.
    // Optional runtime extension; absent fields preserve the original demo.
    Vec3 playerSpawn{}; // Render-model origin in runtime coordinates.
    bool enemyEnabled = true;
    bool demoPropsEnabled = true;
    bool helmetEnabled = false;
    bool groundEnabled = true;
    std::vector<Portal> portals;
    std::vector<Vec3> savePoints; // Safe grounded render-origin positions.
    struct Arena { bool enabled=false; std::vector<Vec3> spawnGates; } arena;
    ParkourCourse parkour{};
    // Optional workshop: three cube entities become live Jolt bodies.
    struct Workshop {
        bool enabled = false;
        std::uint32_t crateId = kNoParent, gateId = kNoParent, liftId = kNoParent;
        Vec3 plate{};
        float liftTravel = 4.0f;
        struct MovingPlatform { std::uint32_t entityId=kNoParent; Vec3 travel{}; float period=12; };
        std::vector<MovingPlatform> movingPlatforms;
    } workshop;
    WaterSurface water{};
    int formatVersion = 0;
    std::string game;
    std::vector<Entity> entities;
    Camera mainCamera{};
    std::vector<Light> lights;
    Skybox skybox{};
    bool hasGroundSurface = false;
    float authoredGroundTopY = 0.0f;
    std::size_t unsupportedVisibleMeshCount = 0;

    std::size_t StaticCubeCount() const;
    std::size_t StaticModelCount() const;
};

// Strict, bounded reader for Mikan SceneSerializer formatVersion 1. It only
// reads JSON; it never writes back to the Mikan project or scene file.
bool ParseMikanScene(const char* json, std::size_t size, Definition& output,
    std::string& error);

// Read a packaged OHOS rawfile and parse it with the same compatibility path.
bool LoadMikanSceneRawFile(const char* rawFilePath, Definition& output,
    std::string& error);

// Mikan serializes quaternions as [w, x, y, z]. The in-memory fields stay
// named x/y/z/w so matrix construction and other runtime math remain explicit.
// Convert an authored TRS transform to a column-major model matrix.
void BuildModelMatrix(const Transform& transform, float output[16]);

// Apply the shared scene-to-runtime ground alignment used by both renderers
// and the physics importer.
Transform RuntimeTransform(const Definition& definition, const Entity& entity,
    float runtimeGroundTopY);

// Build collision boxes only for the static cube props imported by both RHIs.
// Cubes without authored collider components receive a model-fitted box so a
// newly added scene cube remains solid even though OHOS has no scene editor.
std::vector<StaticBoxCollider> BuildStaticBoxColliders(const Definition& definition,
    float runtimeGroundTopY);

} // namespace scene
