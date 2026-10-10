#pragma once
#include "gameplay_lights.h"
#include <cmath>

namespace gameplay {
inline constexpr float kEnemyActivityRadius = 6.0f;
inline constexpr float kEnemyDetectionRadius = 5.0f;
struct EnemyGoal { float dx; float dz; float distance; bool chase; };
inline EnemyGoal EnemyActivityGoal(float x, float z, float playerX, float playerZ,
    bool& returning)
{
    const float hx=x-kEnemySpawnX, hz=z-kEnemySpawnZ;
    const float home=std::sqrt(hx*hx+hz*hz);
    const float px=playerX-kEnemySpawnX, pz=playerZ-kEnemySpawnZ;
    const float dx=playerX-x, dz=playerZ-z;
    const float distance=std::sqrt(dx*dx+dz*dz);
    const bool playerInside=px*px+pz*pz <= kEnemyActivityRadius*kEnemyActivityRadius;
    if (home > kEnemyActivityRadius || !playerInside || distance > kEnemyDetectionRadius)
        returning=true;
    if (home <= 0.25f) returning=false;
    const bool chase=!returning && playerInside && distance<=kEnemyDetectionRadius;
    return chase ? EnemyGoal{dx,dz,distance,true} : EnemyGoal{-hx,-hz,home,false};
}
// Bound the intended step instead of teleporting the Jolt character.
inline void LimitEnemyStep(float x,float z,float dt,float& vx,float& vz)
{
    if (!(dt > 0.0f)) { vx=0; vz=0; return; }
    float tx=x+vx*dt-kEnemySpawnX, tz=z+vz*dt-kEnemySpawnZ;
    const float distance=std::sqrt(tx*tx+tz*tz);
    const float hx=x-kEnemySpawnX, hz=z-kEnemySpawnZ;
    // A displaced actor returns normally, avoiding a sudden position snap.
    if (hx*hx+hz*hz > kEnemyActivityRadius*kEnemyActivityRadius) return;
    if (distance > kEnemyActivityRadius) {
        tx*=kEnemyActivityRadius/distance; tz*=kEnemyActivityRadius/distance;
        vx=(tx+kEnemySpawnX-x)/dt; vz=(tz+kEnemySpawnZ-z)/dt;
    }
}
} // namespace gameplay
