#pragma once

#include "../scene/scene_definition.h"

#include <vector>

namespace physics {

// These values are the single source of truth for the rendered scene props
// and their Jolt collision shapes.  The ground asset is a unit cube with
// vertices in [-1, 1], so its render scale is also the OBB half extent.
struct GroundColliderSpec {
    float centerX;
    float centerY;
    float centerZ;
    float halfExtentX;
    float halfExtentY;
    float halfExtentZ;
};

inline constexpr GroundColliderSpec kGroundCollider{
    0.0f, -1.14f, 0.0f,
    20.0f, 0.2f, 20.0f,
};

struct HelmetColliderSpec {
    float centerX;
    float centerY;
    float centerZ;
    float radius;
    float renderScale;
};

inline constexpr HelmetColliderSpec kHelmetCollider{
    -1.85f, -0.44f, 0.0f,
    0.5f, 0.5f,
};

// The player model is normalised to a height of 1.88 world units.  The
// capsule is built with a cylindrical half-height plus hemispherical caps, so
// its total height is 2 * (halfHeight + radius) and its base is at y=0.
inline constexpr float kPlayerCapsuleRadius = 0.35f;
inline constexpr float kPlayerCapsuleHalfHeight = 0.59f;
inline constexpr float kPlayerCapsuleHeight =
    2.0f * (kPlayerCapsuleHalfHeight + kPlayerCapsuleRadius);
inline constexpr float kGameplayCharacterPadding = 0.02f;
// The Jolt base is the capsule bottom.  The normalised render model is
// centred around its origin, so this offset converts a Jolt base position to
// the model's translation while keeping both feet on the same OBB top plane.
inline constexpr float kPlayerVisualOriginOffset =
    kPlayerCapsuleHalfHeight + kPlayerCapsuleRadius + kGameplayCharacterPadding;

struct CharacterState {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float velocityZ = 0.0f;
    bool grounded = false;
};

// Small shared gameplay physics facade.  The renderer backends only consume
// CharacterState; no backend owns collision or movement rules.
class JoltGameplayPhysics final {
public:
    JoltGameplayPhysics();
    ~JoltGameplayPhysics();

    JoltGameplayPhysics(const JoltGameplayPhysics&) = delete;
    JoltGameplayPhysics& operator=(const JoltGameplayPhysics&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsReady() const;
    bool SetStaticSceneColliders(const std::vector<scene::StaticBoxCollider>& colliders);
    bool SetTerrainColliders(const scene::Definition& definition);

    void Reset(float playerX, float playerY, float playerZ,
               float enemyX, float enemyY, float enemyZ);

    void MovePlayer(float desiredVelocityX, float desiredVelocityZ,
                    float deltaSeconds, bool jumpPressed);
    void MoveEnemy(float desiredVelocityX, float desiredVelocityZ,
                   float deltaSeconds);

    CharacterState GetPlayerState() const;
    CharacterState GetEnemyState() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace physics
