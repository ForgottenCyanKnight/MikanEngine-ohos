#pragma once
#include <algorithm>
#include <cmath>

namespace gameplay {
inline constexpr float kPlayerSpawnX = 0.0f;
inline constexpr float kPlayerSpawnZ = 0.0f;
inline constexpr float kEnemySpawnX = 12.0f;
inline constexpr float kEnemySpawnZ = -12.0f;
inline constexpr float kHealingLightX = -5.0f;
inline constexpr float kHealingLightY = -0.64f;
inline constexpr float kHealingLightZ = 5.0f;
inline constexpr float kHealingRadius = 2.0f;
inline constexpr float kHealingPerSecond = 15.0f;

// Shared by both render paths. Use a 3D radius so other elevations cannot heal.
inline float HealNearLight(float health, float maximum, float x, float y,
    float z, float dt)
{
    if (health <= 0.0f || health >= maximum || !(dt > 0.0f) || !std::isfinite(dt))
        return health;
    const float dx = x-kHealingLightX, dy = y-kHealingLightY, dz = z-kHealingLightZ;
    if (dx*dx + dy*dy + dz*dz > kHealingRadius*kHealingRadius) return health;
    return std::min(maximum, health + kHealingPerSecond*dt);
}
} // namespace gameplay
