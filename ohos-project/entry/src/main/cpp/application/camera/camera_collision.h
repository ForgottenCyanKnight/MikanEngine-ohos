#pragma once
#include <algorithm>
#include <cmath>

namespace camera {
// The normalised character is 1.88 units tall; its render origin is at the waist.
inline constexpr float kHeadFocusOffset = .72f;
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Placement { Vec3 focus, eye; float distance = 0; bool obstructed = false; };

// A sphere enclosing the near-plane corners also protects wide landscape FOVs.
inline float CollisionRadius(float nearPlane, float verticalFovDegrees, float aspect)
{
    const float halfHeight = nearPlane * std::tan(verticalFovDegrees * .00872664626f);
    return std::max(.24f, std::sqrt(nearPlane * nearPlane +
        halfHeight * halfHeight * (1 + aspect * aspect)) + .04f);
}

class ObstructionCamera {
public:
    void Reset() { ready_ = false; distance_ = 0; }

    template<class Sweep>
    Placement Resolve(Vec3 anchor, Vec3 desiredFocus, Vec3 backward,
        float desiredDistance, float radius, float dt, Sweep sweep)
    {
        Placement result;
        // Protect the smoothed follow pivot and over-shoulder aiming offset too.
        Vec3 offset{desiredFocus.x-anchor.x, desiredFocus.y-anchor.y, desiredFocus.z-anchor.z};
        const float length = std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
        result.focus = desiredFocus;
        if (length > .0001f) {
            const float hit = sweep(anchor.x, anchor.y, anchor.z,
                offset.x/length, offset.y/length, offset.z/length, length, radius);
            if (hit < length) {
                const float fraction = std::max(0.0f, hit-.04f) / length;
                result.focus = {anchor.x+offset.x*fraction, anchor.y+offset.y*fraction, anchor.z+offset.z*fraction};
                result.obstructed = true;
            }
        }
        const float hit = sweep(result.focus.x, result.focus.y, result.focus.z,
            backward.x, backward.y, backward.z, desiredDistance, radius);
        const float allowed = hit < desiredDistance ? std::max(0.0f, hit-.04f) : desiredDistance;
        result.obstructed = result.obstructed || allowed < desiredDistance-.001f;
        // Safety always wins over smoothing when a wall moves into the orbit.
        // Only recovery is damped; never force a minimum through a close wall.
        if (!ready_ || allowed < distance_) distance_ = allowed;
        else if (std::isfinite(dt) && dt > 0)
            distance_ += (allowed-distance_) * (1-std::exp(-6.0f*dt));
        distance_ = std::clamp(distance_, 0.0f, allowed);
        ready_ = true;
        result.distance = distance_;
        result.eye = {result.focus.x+backward.x*distance_, result.focus.y+backward.y*distance_,
            result.focus.z+backward.z*distance_};
        return result;
    }

private:
    bool ready_ = false;
    float distance_ = 0;
};
} // namespace camera
