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
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
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
#include <map>

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
    std::map<std::string, std::vector<JPH::ShapeRefC>> terrainShapeCache;
    JPH::ShapeRefC floorShape;
    JPH::ShapeRefC helmetShape;
    JPH::ShapeRefC characterShape;
    std::unique_ptr<JPH::CharacterVirtual> player;
    std::unique_ptr<JPH::CharacterVirtual> enemy;
    JPH::CharacterVsCharacterCollisionSimple characterCollision;
    scene::Definition waterScene;
    bool playerSwimming = false;
    bool enemyCollisionEnabled = true;
    struct WorkshopBody { std::uint32_t id; JPH::BodyID body; scene::Transform initial; };
    std::vector<WorkshopBody> workshopBodies;
    scene::Definition::Workshop workshop;
    float workshopTime=0, platformTime=0, plateTime=0, gateHeight=0;
    bool gateLatched=false, platePressed=false;

    void ClearWorkshop() {
        if (physicsSystem) {
            auto& bodies=physicsSystem->GetBodyInterface();
            for (const auto& item:workshopBodies) { bodies.RemoveBody(item.body); bodies.DestroyBody(item.body); }
        }
        workshopBodies.clear(); workshop={};
    }

    void ResetWorkshop() {
        workshopTime=platformTime=plateTime=gateHeight=0; gateLatched=platePressed=false;
        auto& bodies=physicsSystem->GetBodyInterface();
        for (const auto& item:workshopBodies) {
            const auto& p=item.initial.position;
            bodies.SetPositionAndRotation(item.body,JPH::RVec3(p.x,p.y,p.z),JPH::Quat::sIdentity(),JPH::EActivation::Activate);
            bodies.SetLinearAndAngularVelocity(item.body,JPH::Vec3::sZero(),JPH::Vec3::sZero());
        }
    }

    bool CreateWorkshop(const scene::Definition& definition) {
        ClearWorkshop(); workshop=definition.workshop;
        if (!workshop.enabled) return true;
        auto& bodies=physicsSystem->GetBodyInterface();
        for (const auto& e:definition.entities) {
            bool moving=false;
            for (const auto& platform:workshop.movingPlatforms) moving |= e.id==platform.entityId;
            if (e.id!=workshop.crateId && e.id!=workshop.gateId && e.id!=workshop.liftId && !moving) continue;
            const auto t=scene::RuntimeTransform(definition,e,kGroundCollider.centerY+kGroundCollider.halfExtentY);
            const auto& p=t.position; const auto& scale=t.scale;
            const bool crate=e.id==workshop.crateId;
            JPH::BodyCreationSettings settings(new JPH::BoxShape(JPH::Vec3(scale.x,scale.y,scale.z)),
                JPH::RVec3(p.x,p.y,p.z),JPH::Quat::sIdentity(),
                crate ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,kMovingLayer);
            settings.mFriction=.55f; settings.mRestitution=0;
            if (crate) {
                settings.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass=12;
                settings.mLinearDamping=.4f;
                settings.mMotionQuality=JPH::EMotionQuality::LinearCast;
            }
            const auto body=bodies.CreateAndAddBody(settings,JPH::EActivation::Activate);
            if (body.IsInvalid()) { ClearWorkshop(); return false; }
            workshopBodies.push_back({e.id,body,t});
        }
        ResetWorkshop(); return workshopBodies.size()==3+workshop.movingPlatforms.size();
    }

    void StepWorkshop(float dt) {
        if (!workshop.enabled || dt<=0) return;
        auto& bodies=physicsSystem->GetBodyInterface();
        platformTime+=dt;
        for (const auto& item:workshopBodies) if (item.id==workshop.crateId) {
            const auto p=bodies.GetPosition(item.body);
            platePressed=std::fabs(float(p.GetX())-workshop.plate.x)<.55f &&
                std::fabs(float(p.GetZ())-workshop.plate.z)<.65f &&
                std::fabs(float(p.GetY())-item.initial.scale.y-workshop.plate.y)<.2f;
            // A misplaced crate can always be recovered by re-entering/retrying.
            if (p.GetY()<-5) ResetWorkshop();
        }
        plateTime=platePressed ? plateTime+dt : 0;
        if (!gateLatched && plateTime>=.6f) {
            gateLatched=true;
            SDL_Log("SDL3_WORKSHOP stage=plate_latched");
        }
        if (gateLatched) { gateHeight=std::min(3.8f,gateHeight+1.8f*dt); workshopTime+=dt; }
        // One second dwell at each end gives the player time to board/alight.
        const float travel=workshop.liftTravel;
        const float phase=std::fmod(std::max(0.f,workshopTime-3.f),2*travel+2);
        const float height=phase<1 ? 0 : (phase<1+travel ? phase-1 :
            (phase<2+travel ? travel : 2+2*travel-phase));
        for (const auto& item:workshopBodies) if (item.id!=workshop.crateId) {
            const auto& p=item.initial.position;
            JPH::RVec3 target(p.x,p.y,p.z);
            if (item.id==workshop.gateId) target+=JPH::Vec3(0,gateHeight,0);
            else if (item.id==workshop.liftId) target+=JPH::Vec3(0,height,0);
            else for (const auto& platform:workshop.movingPlatforms) if (platform.entityId==item.id) {
                const float half=platform.period*.5f;
                const float phase=std::fmod(platformTime,platform.period);
                const float blend=phase<half ? std::clamp((phase-1)/(half-1),0.f,1.f) :
                    1-std::clamp((phase-half-1)/(half-1),0.f,1.f);
                target+=JPH::Vec3(platform.travel.x,platform.travel.y,platform.travel.z)*blend;
            }
            bodies.MoveKinematic(item.body,target,JPH::Quat::sIdentity(),dt);
        }
        // Advance actual rigid bodies once per gameplay frame, before the
        // virtual character samples contacts/platform ground velocity.
        physicsSystem->Update(dt,1,tempAllocator.get(),&jobSystem);
    }

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
            ClearWorkshop();
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
        terrainShapeCache.clear();
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
        if (workshop.enabled) ResetWorkshop();
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
        character->UpdateGroundVelocity();

        const auto position = character->GetPosition();
        const bool isPlayer = character == player.get();
        if (isPlayer) playerSwimming = water::PlayerSubmerged(waterScene,
            float(position.GetX()), float(position.GetY()), float(position.GetZ()), playerSwimming);
        const bool swimming = isPlayer && playerSwimming;
        const auto groundVelocity=character->GetGroundVelocity();
        const bool onGround=supported && velocity.GetY()<=groundVelocity.GetY()+.1f;
        if (onGround && !swimming) {
            velocity.SetX(velocity.GetX()-groundVelocity.GetX());
            velocity.SetZ(velocity.GetZ()-groundVelocity.GetZ());
        }
        if (isPlayer && !swimming) {
            // A released (or briefly interrupted) stick must not delete takeoff
            // momentum. Air steering changes velocity gradually; no input coasts.
            const bool hasInput = desiredVelocityX * desiredVelocityX +
                desiredVelocityZ * desiredVelocityZ > .0001f;
            const bool coasting = !hasInput && (!onGround || jumpPressed);
            if (!coasting) {
                const float acceleration = onGround ? (hasInput ? 18.0f : 26.0f) : 2.5f;
                const float dx = desiredVelocityX - velocity.GetX();
                const float dz = desiredVelocityZ - velocity.GetZ();
                const float difference = std::sqrt(dx*dx + dz*dz);
                const float amount = difference > .0001f ?
                    std::min(1.0f, acceleration * dt / difference) : 1.0f;
                velocity.SetX(velocity.GetX() + dx*amount);
                velocity.SetZ(velocity.GetZ() + dz*amount);
            }
        } else {
            velocity.SetX(desiredVelocityX);
            velocity.SetZ(desiredVelocityZ);
        }
        if (onGround && !swimming) {
            velocity.SetX(velocity.GetX()+groundVelocity.GetX());
            velocity.SetZ(velocity.GetZ()+groundVelocity.GetZ());
        }
        if (swimming) {
            // Keep the capsule feet 0.95 m below the sea, with swept collision
            // during ascent so shores and overhead geometry still constrain motion.
            const float targetY = waterScene.water.height - 0.95f;
            velocity.SetY(std::clamp((targetY - float(position.GetY())) * 8.0f, -2.0f, 3.0f));
        } else if (jumpPressed && supported) {
            velocity.SetY(kJumpSpeed);
            if (isPlayer) SDL_Log("SDL3_MOVE stage=jump vx=%.2f vz=%.2f", velocity.GetX(), velocity.GetZ());
        } else if (onGround) {
            // Keep the virtual character attached to the floor while walking.
            velocity.SetY(character->GetGroundVelocity().GetY()-0.1f);
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
            // Shallow shore steps allow swimming -> walking without requiring
            // a jump out of the water. Deep water remains unsupported.
            updateSettings.mStickToFloorStepDown = JPH::Vec3(0,-.2f,0);
            updateSettings.mWalkStairsStepUp = JPH::Vec3(0,.35f,0);
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

bool JoltGameplayPhysics::PrepareTerrainColliders(const scene::Definition& definition,
    const std::string& cacheKey)
{
    if (!IsReady()) return false;
    if (!cacheKey.empty() && impl_->terrainShapeCache.count(cacheKey)) return true;
    ohos_model::Model model;
    std::string error;
    if (!terrain::BuildModel(definition, kGroundCollider.centerY + kGroundCollider.halfExtentY,
            model, error, true)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_TERRAIN stage=collision_load_failed error=%s", error.c_str());
        return false;
    }
    std::vector<JPH::ShapeRefC> shapes;
    for (const auto& mesh : model.meshes) {
        JPH::MeshShapeSettings settings;
        for (const auto& vertex : mesh.vertices)
            settings.mTriangleVertices.emplace_back(vertex.position[0], vertex.position[1], vertex.position[2]);
        for (size_t i = 0; i < mesh.indices.size(); i += 3)
            settings.mIndexedTriangles.emplace_back(mesh.indices[i], mesh.indices[i+1], mesh.indices[i+2], 0);
        auto shape = settings.Create();
        if (shape.HasError()) return false;
        shapes.push_back(shape.Get());
    }
    impl_->terrainShapeCache[cacheKey] = std::move(shapes);
    SDL_Log("SDL3_TERRAIN stage=collision_preloaded key=%s triangles=%u", cacheKey.c_str(), model.triangleCount);
    return true;
}

bool JoltGameplayPhysics::SetTerrainColliders(const scene::Definition& definition,
    const std::string& cacheKey)
{
    // Empty keys keep the uncached contract used by editable scenes and tests.
    if (!PrepareTerrainColliders(definition, cacheKey)) return false;
    auto& bodies = impl_->physicsSystem->GetBodyInterface();
    std::vector<JPH::BodyID> created;
    for (const auto& shape : impl_->terrainShapeCache.at(cacheKey)) {
        JPH::BodyCreationSettings settings(shape.GetPtr(), JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(), JPH::EMotionType::Static, kNonMovingLayer);
        const auto body = bodies.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (body.IsInvalid()) {
            for (auto previous : created) { bodies.RemoveBody(previous); bodies.DestroyBody(previous); }
            return false;
        }
        created.push_back(body);
    }
    for (auto body : impl_->terrainBodies) { bodies.RemoveBody(body); bodies.DestroyBody(body); }
    impl_->terrainBodies.swap(created);
    if (!impl_->CreateWorkshop(definition)) return false;
    if (!impl_->floorBody.IsInvalid())
        bodies.SetPosition(impl_->floorBody, JPH::RVec3(kGroundCollider.centerX,
            definition.groundEnabled ? kGroundCollider.centerY : -1000.0f,
            kGroundCollider.centerZ), JPH::EActivation::DontActivate);
    impl_->waterScene.water = definition.water;
    impl_->playerSwimming = false;
    // The legacy demo helmet is not part of independently loaded levels.
    if (!impl_->helmetBody.IsInvalid()) {
        bodies.SetPosition(impl_->helmetBody,
            JPH::RVec3(kHelmetCollider.centerX,
                definition.demoPropsEnabled ? kHelmetCollider.centerY : -1000.0f,
                kHelmetCollider.centerZ), JPH::EActivation::DontActivate);
    }
    SDL_Log("SDL3_TERRAIN stage=collision_ready bodies=%zu cache=%s", impl_->terrainBodies.size(), cacheKey.c_str());
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
        impl_->StepWorkshop(std::clamp(deltaSeconds,0.f,kMaxFrameDelta));
        impl_->MoveCharacter(impl_->player.get(), desiredVelocityX, desiredVelocityZ,
                             deltaSeconds, jumpPressed);
    }
}

bool JoltGameplayPhysics::GetWorkshopTransform(std::uint32_t id, scene::Transform& transform) const
{
    if (!IsReady()) return false;
    for (const auto& item:impl_->workshopBodies) if (item.id==id) {
        const auto& bodies=impl_->physicsSystem->GetBodyInterface();
        const auto p=bodies.GetPosition(item.body); const auto q=bodies.GetRotation(item.body);
        transform=item.initial;
        transform.position={float(p.GetX()),float(p.GetY()),float(p.GetZ())};
        transform.rotation={q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
        return true;
    }
    return false;
}

void JoltGameplayPhysics::RespawnPlayer(float x,float feetY,float z)
{
    if (!IsReady()) return;
    impl_->playerSwimming=false;
    impl_->player->SetPosition(JPH::RVec3(x,feetY,z));
    impl_->player->SetLinearVelocity(JPH::Vec3::sZero());
    impl_->player->RefreshContacts(impl_->physicsSystem->GetDefaultBroadPhaseLayerFilter(kMovingLayer),
        impl_->physicsSystem->GetDefaultLayerFilter(kMovingLayer),JPH::BodyFilter{},JPH::ShapeFilter{},*impl_->tempAllocator);
}

JoltGameplayPhysics::WorkshopState JoltGameplayPhysics::GetWorkshopState() const
{
    WorkshopState result;
    if (!IsReady()) return result;
    result.enabled=impl_->workshop.enabled; result.platePressed=impl_->platePressed;
    result.gateOpen=impl_->gateLatched;
    for (const auto& item:impl_->workshopBodies) if (item.id==impl_->workshop.liftId)
        result.liftHeight=float(impl_->physicsSystem->GetBodyInterface().GetPosition(item.body).GetY())-item.initial.position.y;
    return result;
}

void JoltGameplayPhysics::RestoreGateProgress(bool open)
{
    if (!IsReady() || !impl_->workshop.enabled || !open) return;
    impl_->gateLatched=true; impl_->gateHeight=3.8f;
    auto& bodies=impl_->physicsSystem->GetBodyInterface();
    for (const auto& item:impl_->workshopBodies) if(item.id==impl_->workshop.gateId) {
        const auto& p=item.initial.position;
        bodies.SetPosition(item.body,JPH::RVec3(p.x,p.y+3.8f,p.z),JPH::EActivation::Activate);
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

float JoltGameplayPhysics::SweepCamera(float x, float y, float z,
    float dx, float dy, float dz, float distance, float radius) const
{
    if (!IsReady() || !(distance > 0) || !(radius > 0) || !std::isfinite(distance) ||
        !std::isfinite(radius)) return distance;
    JPH::SphereShape sphere(radius);
    const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1),
        JPH::RMat44::sTranslation(JPH::RVec3(x,y,z)), JPH::Vec3(dx,dy,dz)*distance);
    JPH::ShapeCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hits;
    impl_->physicsSystem->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3(x,y,z), hits);
    return hits.HadHit() ? distance * std::clamp(hits.mHit.mFraction, 0.0f, 1.0f) : distance;
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
