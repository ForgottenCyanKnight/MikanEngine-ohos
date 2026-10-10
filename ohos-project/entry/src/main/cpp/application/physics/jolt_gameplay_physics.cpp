#include "jolt_gameplay_physics.h"
#include "../terrain/heightmap_terrain.h"
#include "../water/water_surface.h"

#include <SDL3/SDL.h>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace physics {
namespace {

constexpr JPH::ObjectLayer kNonMovingLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;
constexpr JPH::BroadPhaseLayer kNonMovingBroadPhaseLayer(0);
constexpr JPH::BroadPhaseLayer kMovingBroadPhaseLayer(1);

constexpr float kGravityY = -9.81f;
constexpr float kCharacterBottomOffset =
    kPlayerCapsuleHalfHeight + kPlayerCapsuleRadius;
constexpr float kJumpSpeed = 5.0f;
constexpr float kMaxFrameDelta = 0.05f;

void JoltTraceNoop(const char*, ...)
{
    // Jolt tracing is intentionally quiet in the shipping OHOS sample.  The
    // renderer already owns the app log and will report initialization errors.
}

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        return inLayer == kMovingLayer ? kMovingBroadPhaseLayer : kNonMovingBroadPhaseLayer;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        return inLayer == kMovingBroadPhaseLayer ? "MOVING" : "NON_MOVING";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer inObjectLayer,
                       JPH::BroadPhaseLayer inBroadPhaseLayer) const override
    {
        if (inObjectLayer == kNonMovingLayer) {
            return inBroadPhaseLayer == kMovingBroadPhaseLayer;
        }
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer inObjectLayer1,
                       JPH::ObjectLayer inObjectLayer2) const override
    {
        // Static geometry never needs to be paired with other static geometry;
        // both moving characters do need to see one another.
        return inObjectLayer1 != kNonMovingLayer || inObjectLayer2 != kNonMovingLayer;
    }
};

JPH::ShapeRefC CreateCharacterShape()
{
    const JPH::CapsuleShapeSettings capsuleSettings(
        kPlayerCapsuleHalfHeight, kPlayerCapsuleRadius);
    const JPH::ShapeSettings::ShapeResult capsuleResult = capsuleSettings.Create();
    if (!capsuleResult.IsValid()) {
        return {};
    }

    const JPH::ShapeRefC capsule = capsuleResult.Get();
    const JPH::RotatedTranslatedShapeSettings translatedSettings(
        JPH::Vec3(0.0f, kCharacterBottomOffset, 0.0f),
        JPH::Quat::sIdentity(),
        capsule.GetPtr());
    const JPH::ShapeSettings::ShapeResult translatedResult = translatedSettings.Create();
    if (!translatedResult.IsValid()) {
        return {};
    }
    return translatedResult.Get();
}

CharacterState ToState(const JPH::CharacterVirtual* character)
{
    CharacterState result;
    if (character == nullptr) {
        return result;
    }

    const JPH::RVec3 position = character->GetPosition();
    const JPH::Vec3 velocity = character->GetLinearVelocity();
    result.x = static_cast<float>(position.GetX());
    result.y = static_cast<float>(position.GetY());
    result.z = static_cast<float>(position.GetZ());
    result.velocityX = velocity.GetX();
    result.velocityY = velocity.GetY();
    result.velocityZ = velocity.GetZ();
    result.grounded = character->IsSupported();
    return result;
}

} // namespace

struct JoltGameplayPhysics::Impl {
    bool ready = false;
    bool ownsFactory = false;
    bool registeredTypes = false;

    BroadPhaseLayerInterface broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseLayers;
    ObjectLayerPairFilter objectLayerPairs;
    JPH::JobSystemSingleThreaded jobSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    JPH::BodyID floorBody;
    JPH::BodyID helmetBody;
    std::vector<JPH::BodyID> sceneColliderBodies;
    std::vector<JPH::BodyID> terrainBodies;
    JPH::ShapeRefC floorShape;
    JPH::ShapeRefC helmetShape;
    JPH::ShapeRefC characterShape;
    std::unique_ptr<JPH::CharacterVirtual> player;
    std::unique_ptr<JPH::CharacterVirtual> enemy;
    JPH::CharacterVsCharacterCollisionSimple characterCollision;
    scene::Definition waterScene;
    bool playerSwimming = false;
    bool enemyCollisionEnabled = true;

    ~Impl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        // CharacterVirtual holds a pointer to PhysicsSystem, so release it
        // before tearing down the system and its collision filters.
        if (player != nullptr) {
            characterCollision.Remove(player.get());
        }
        if (enemy != nullptr && enemyCollisionEnabled) {
            characterCollision.Remove(enemy.get());
        }
        player.reset();
        enemy.reset();

        if (physicsSystem != nullptr) {
            JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
            for (const JPH::BodyID body : sceneColliderBodies) {
                bodyInterface.RemoveBody(body);
                bodyInterface.DestroyBody(body);
            }
            sceneColliderBodies.clear();
            for (const JPH::BodyID body : terrainBodies) {
                bodyInterface.RemoveBody(body);
                bodyInterface.DestroyBody(body);
            }
            terrainBodies.clear();
            if (!helmetBody.IsInvalid()) {
                bodyInterface.RemoveBody(helmetBody);
                bodyInterface.DestroyBody(helmetBody);
                helmetBody = JPH::BodyID();
            }
            if (!floorBody.IsInvalid()) {
                bodyInterface.RemoveBody(floorBody);
                bodyInterface.DestroyBody(floorBody);
                floorBody = JPH::BodyID();
            }
        }

        physicsSystem.reset();
        tempAllocator.reset();
        characterShape = nullptr;
        helmetShape = nullptr;
        floorShape = nullptr;

        if (registeredTypes) {
            JPH::UnregisterTypes();
            registeredTypes = false;
        }
        if (ownsFactory && JPH::Factory::sInstance != nullptr) {
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
            ownsFactory = false;
        }
        ready = false;
    }

    bool Initialize()
    {
        if (ready) {
            return true;
        }

        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTraceNoop;

        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            ownsFactory = true;
        }
        JPH::RegisterTypes();
        registeredTypes = true;

        jobSystem.Init(1024);
        tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(4u * 1024u * 1024u);
        physicsSystem = std::make_unique<JPH::PhysicsSystem>();
        physicsSystem->Init(
            512,
            0,
            256,
            256,
            broadPhaseLayers,
            objectVsBroadPhaseLayers,
            objectLayerPairs);
        physicsSystem->SetGravity(JPH::Vec3(0.0f, kGravityY, 0.0f));

        // The rendered ground is a [-1, 1] cube scaled by these half extents,
        // so this is an exact OBB, not an undersized plane or an infinite
        // ground fallback.  Keep the body transform paired with the render
        // transform through kGroundCollider in the public header.
        floorShape = new JPH::BoxShape(JPH::Vec3(
            kGroundCollider.halfExtentX,
            kGroundCollider.halfExtentY,
            kGroundCollider.halfExtentZ));
        const JPH::BodyCreationSettings floorSettings(
            floorShape.GetPtr(),
            JPH::RVec3(
                kGroundCollider.centerX,
                kGroundCollider.centerY,
                kGroundCollider.centerZ),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            kNonMovingLayer);
        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        floorBody = bodyInterface.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
        if (floorBody.IsInvalid()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Jolt: failed to create the gameplay floor");
            Shutdown();
            return false;
        }

        // The rotating helmet is a static scene prop.  A sphere is a stable
        // approximation for its collision volume and, because it is round,
        // does not need to be updated as the visual helmet turns.
        helmetShape = new JPH::SphereShape(kHelmetCollider.radius);
        const JPH::BodyCreationSettings helmetSettings(
            helmetShape.GetPtr(),
            JPH::RVec3(
                kHelmetCollider.centerX,
                kHelmetCollider.centerY,
                kHelmetCollider.centerZ),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            kNonMovingLayer);
        helmetBody = bodyInterface.CreateAndAddBody(
            helmetSettings, JPH::EActivation::DontActivate);
        if (helmetBody.IsInvalid()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Jolt: failed to create the helmet sphere");
            Shutdown();
            return false;
        }

        characterShape = CreateCharacterShape();
        if (characterShape == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Jolt: failed to create the character capsule");
            Shutdown();
            return false;
        }

        JPH::CharacterVirtualSettings characterSettings;
        characterSettings.mShape = characterShape;
        characterSettings.mUp = JPH::Vec3::sAxisY();
        characterSettings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
        characterSettings.mCharacterPadding = kGameplayCharacterPadding;
        characterSettings.mPredictiveContactDistance = 0.1f;
        characterSettings.mMaxCollisionIterations = 5;
        characterSettings.mPenetrationRecoverySpeed = 1.0f;
        characterSettings.mInnerBodyLayer = kMovingLayer;

        player = std::make_unique<JPH::CharacterVirtual>(
            &characterSettings,
            JPH::RVec3(0.0, 0.0, 0.0),
            JPH::Quat::sIdentity(),
            physicsSystem.get());
        enemy = std::make_unique<JPH::CharacterVirtual>(
            &characterSettings,
            JPH::RVec3(1.35, 0.0, -0.65),
            JPH::Quat::sIdentity(),
            physicsSystem.get());
        player->SetCharacterVsCharacterCollision(&characterCollision);
        enemy->SetCharacterVsCharacterCollision(&characterCollision);
        characterCollision.Add(player.get());
        characterCollision.Add(enemy.get());
        enemyCollisionEnabled = true;

        ready = true;
        SDL_Log("Jolt: gameplay physics initialized (floor + player/enemy virtual characters)");
        return true;
    }

    bool SetStaticSceneColliders(const std::vector<scene::StaticBoxCollider>& colliders)
    {
        if (!ready || physicsSystem == nullptr) {
            return false;
        }

        JPH::BodyInterface& bodyInterface = physicsSystem->GetBodyInterface();
        std::vector<JPH::BodyID> createdBodies;
        createdBodies.reserve(colliders.size());
        for (const scene::StaticBoxCollider& collider : colliders) {
            if (!std::isfinite(collider.center.x) || !std::isfinite(collider.center.y) ||
                !std::isfinite(collider.center.z) ||
                !std::isfinite(collider.halfExtents.x) ||
                !std::isfinite(collider.halfExtents.y) ||
                !std::isfinite(collider.halfExtents.z) ||
                collider.halfExtents.x <= 1.0e-4f ||
                collider.halfExtents.y <= 1.0e-4f ||
                collider.halfExtents.z <= 1.0e-4f) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Jolt: skipping invalid scene collider name=%s",
                    collider.entityName.c_str());
                continue;
            }

            const JPH::ShapeRefC shape = new JPH::BoxShape(JPH::Vec3(
                collider.halfExtents.x,
                collider.halfExtents.y,
                collider.halfExtents.z));
            JPH::Quat rotation(
                collider.rotation.x,
                collider.rotation.y,
                collider.rotation.z,
                collider.rotation.w);
            if (!rotation.IsNormalized()) {
                rotation = JPH::Quat::sIdentity();
            }
            const JPH::BodyCreationSettings settings(
                shape.GetPtr(),
                JPH::RVec3(collider.center.x, collider.center.y, collider.center.z),
                rotation,
                JPH::EMotionType::Static,
                kNonMovingLayer);
            const JPH::BodyID body = bodyInterface.CreateAndAddBody(
                settings, JPH::EActivation::DontActivate);
            if (body.IsInvalid()) {
                for (const JPH::BodyID created : createdBodies) {
                    bodyInterface.RemoveBody(created);
                    bodyInterface.DestroyBody(created);
                }
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Jolt: failed to create scene collider name=%s; keeping previous set",
                    collider.entityName.c_str());
                return false;
            }
            createdBodies.push_back(body);
        }

        for (const JPH::BodyID body : sceneColliderBodies) {
            bodyInterface.RemoveBody(body);
            bodyInterface.DestroyBody(body);
        }
        sceneColliderBodies.swap(createdBodies);
        SDL_Log("Jolt: synchronized %zu static scene box colliders", sceneColliderBodies.size());
        return true;
    }

    void Reset(float playerX, float playerY, float playerZ,
               float enemyX, float enemyY, float enemyZ)
    {
        if (!ready || player == nullptr || enemy == nullptr) {
            return;
        }

        if (!enemyCollisionEnabled) {
            characterCollision.Add(enemy.get());
            enemyCollisionEnabled = true;
        }
        playerSwimming = false;
        player->SetPosition(JPH::RVec3(playerX, playerY, playerZ));
        enemy->SetPosition(JPH::RVec3(enemyX, enemyY, enemyZ));
        player->SetLinearVelocity(JPH::Vec3::sZero());
        enemy->SetLinearVelocity(JPH::Vec3::sZero());

        const JPH::DefaultBroadPhaseLayerFilter playerBroadPhaseFilter =
            physicsSystem->GetDefaultBroadPhaseLayerFilter(kMovingLayer);
        const JPH::DefaultObjectLayerFilter playerObjectLayerFilter =
            physicsSystem->GetDefaultLayerFilter(kMovingLayer);
        const JPH::BodyFilter bodyFilter;
        const JPH::ShapeFilter shapeFilter;
        player->RefreshContacts(
            playerBroadPhaseFilter,
            playerObjectLayerFilter,
            bodyFilter,
            shapeFilter,
            *tempAllocator);
        enemy->RefreshContacts(
            playerBroadPhaseFilter,
            playerObjectLayerFilter,
            bodyFilter,
            shapeFilter,
            *tempAllocator);
    }

    void MoveCharacter(JPH::CharacterVirtual* character,
                       float desiredVelocityX,
                       float desiredVelocityZ,
                       float deltaSeconds,
                       bool jumpPressed)
    {
        if (!ready || character == nullptr || physicsSystem == nullptr || tempAllocator == nullptr) {
            return;
        }

        const float dt = std::clamp(deltaSeconds, 0.0f, kMaxFrameDelta);
        JPH::Vec3 velocity = character->GetLinearVelocity();
        const bool supported = character->IsSupported();

        velocity.SetX(desiredVelocityX);
        velocity.SetZ(desiredVelocityZ);
        const auto position = character->GetPosition();
        const bool isPlayer = character == player.get();
        if (isPlayer) playerSwimming = water::PlayerSubmerged(waterScene,
            float(position.GetX()), float(position.GetY()), float(position.GetZ()), playerSwimming);
        const bool swimming = isPlayer && playerSwimming;
        if (swimming) {
            // Keep the capsule feet 0.95 m below the sea, with swept collision
            // during ascent so shores and overhead geometry still constrain motion.
            const float targetY = waterScene.water.height - 0.95f;
            velocity.SetY(std::clamp((targetY - float(position.GetY())) * 8.0f, -2.0f, 3.0f));
        } else if (jumpPressed && supported) {
            velocity.SetY(kJumpSpeed);
        } else if (supported && velocity.GetY() < 0.0f) {
            // Keep the virtual character attached to the floor while walking.
            velocity.SetY(-0.1f);
        } else {
            velocity.SetY(velocity.GetY() + kGravityY * dt);
        }
        character->SetLinearVelocity(velocity);

        const JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter =
            physicsSystem->GetDefaultBroadPhaseLayerFilter(kMovingLayer);
        const JPH::DefaultObjectLayerFilter objectLayerFilter =
            physicsSystem->GetDefaultLayerFilter(kMovingLayer);
        const JPH::BodyFilter bodyFilter;
        const JPH::ShapeFilter shapeFilter;
        JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
        updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.5f, 0.0f);
        updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.3f, 0.0f);
        if (swimming) {
            updateSettings.mStickToFloorStepDown = JPH::Vec3::sZero();
            updateSettings.mWalkStairsStepUp = JPH::Vec3::sZero();
        }
        character->ExtendedUpdate(
            dt,
            JPH::Vec3(0.0f, swimming ? 0.0f : kGravityY, 0.0f),
            updateSettings,
            broadPhaseFilter,
            objectLayerFilter,
            bodyFilter,
            shapeFilter,
            *tempAllocator);

        // Keep the swept collision result: the finite platform and terrain
        // determine support height. A global Y clamp would create an invisible
        // infinite floor and cancel gravity everywhere below the platform.
    }
};

JoltGameplayPhysics::JoltGameplayPhysics() = default;

JoltGameplayPhysics::~JoltGameplayPhysics()
{
    Shutdown();
}

bool JoltGameplayPhysics::Initialize()
{
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    return impl_->Initialize();
}

void JoltGameplayPhysics::Shutdown()
{
    if (impl_ == nullptr) {
        return;
    }
    delete impl_;
    impl_ = nullptr;
}

bool JoltGameplayPhysics::IsReady() const
{
    return impl_ != nullptr && impl_->ready;
}

bool JoltGameplayPhysics::SetStaticSceneColliders(
    const std::vector<scene::StaticBoxCollider>& colliders)
{
    return impl_ != nullptr && impl_->SetStaticSceneColliders(colliders);
}

bool JoltGameplayPhysics::SetTerrainColliders(const scene::Definition& definition)
{
    if (!IsReady()) return false;
    ohos_model::Model model;
    std::string error;
    if (!terrain::BuildModel(definition, kGroundCollider.centerY + kGroundCollider.halfExtentY,
            model, error, true)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_TERRAIN stage=collision_load_failed error=%s", error.c_str());
        return false;
    }
    auto& bodies = impl_->physicsSystem->GetBodyInterface();
    std::vector<JPH::BodyID> created;
    const auto rollback = [&]() {
        for (auto body : created) { bodies.RemoveBody(body); bodies.DestroyBody(body); }
    };
    for (const auto& mesh : model.meshes) {
        JPH::MeshShapeSettings shapeSettings;
        for (const auto& vertex : mesh.vertices)
            shapeSettings.mTriangleVertices.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
        for (size_t i = 0; i < mesh.indices.size(); i += 3)
            shapeSettings.mIndexedTriangles.emplace_back(mesh.indices[i], mesh.indices[i+1], mesh.indices[i+2], 0);
        auto shape = shapeSettings.Create();
        if (shape.HasError()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_TERRAIN stage=collision_shape_failed error=%s", shape.GetError().c_str());
            rollback(); return false;
        }
        JPH::BodyCreationSettings settings(shape.Get().GetPtr(), JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static, kNonMovingLayer);
        const auto body = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (body.IsInvalid()) { rollback(); return false; }
        created.push_back(body);
    }
    for (auto body : impl_->terrainBodies) { bodies.RemoveBody(body); bodies.DestroyBody(body); }
    impl_->terrainBodies.swap(created);
    impl_->waterScene.water = definition.water;
    impl_->playerSwimming = false;
    SDL_Log("SDL3_TERRAIN stage=collision_ready bodies=%zu triangles=%u", impl_->terrainBodies.size(), model.triangleCount);
    return true;
}

void JoltGameplayPhysics::Reset(float playerX, float playerY, float playerZ,
                                float enemyX, float enemyY, float enemyZ)
{
    if (impl_ != nullptr) {
        impl_->Reset(playerX, playerY, playerZ, enemyX, enemyY, enemyZ);
    }
}

void JoltGameplayPhysics::MovePlayer(float desiredVelocityX, float desiredVelocityZ,
                                     float deltaSeconds, bool jumpPressed)
{
    if (impl_ != nullptr) {
        impl_->MoveCharacter(impl_->player.get(), desiredVelocityX, desiredVelocityZ,
                             deltaSeconds, jumpPressed);
    }
}

void JoltGameplayPhysics::SetEnemyCollisionEnabled(bool enabled)
{
    if (impl_ == nullptr || !impl_->ready || impl_->enemy == nullptr ||
        impl_->enemyCollisionEnabled == enabled) return;
    if (enabled) impl_->characterCollision.Add(impl_->enemy.get());
    else {
        impl_->characterCollision.Remove(impl_->enemy.get());
        impl_->enemy->SetLinearVelocity(JPH::Vec3::sZero());
    }
    impl_->enemyCollisionEnabled = enabled;
}

void JoltGameplayPhysics::MoveEnemy(float desiredVelocityX, float desiredVelocityZ,
                                    float deltaSeconds)
{
    if (impl_ != nullptr && impl_->enemyCollisionEnabled) {
        impl_->MoveCharacter(impl_->enemy.get(), desiredVelocityX, desiredVelocityZ,
                             deltaSeconds, false);
    }
}

float JoltGameplayPhysics::RaycastWorld(float x,float y,float z,float dx,float dy,float dz,float distance) const
{
    if (!IsReady() || !(distance>0)) return distance;
    JPH::RRayCast ray(JPH::RVec3(x,y,z),JPH::Vec3(dx,dy,dz)*distance);
    JPH::RayCastResult hit;
    if (impl_->physicsSystem->GetNarrowPhaseQuery().CastRay(ray,hit)) return distance*hit.mFraction;
    return distance;
}

CharacterState JoltGameplayPhysics::GetPlayerState() const
{
    return impl_ == nullptr ? CharacterState{} : ToState(impl_->player.get());
}

CharacterState JoltGameplayPhysics::GetEnemyState() const
{
    return impl_ == nullptr ? CharacterState{} : ToState(impl_->enemy.get());
}

} // namespace physics
