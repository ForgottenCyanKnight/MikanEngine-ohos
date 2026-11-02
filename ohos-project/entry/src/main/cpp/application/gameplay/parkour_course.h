#pragma once
#include "../scene/scene_definition.h"
#include <cmath>
#include <cstddef>

namespace gameplay {
// Progress survives falls, but resets on a new visit or explicit retry.
struct ParkourProgress {
    std::size_t checkpoint = 0;
    int falls = 0;
    float seconds = 0;
    bool finished = false;
    bool rewardClaimed = false;
    int ClaimCompletionReward(const scene::ParkourCourse& course) {
        if(!finished || rewardClaimed || !course.enabled)return 0;
        rewardClaimed=true;
        return course.completionReward;
    }

    // Returns true when the caller must restore position/velocity at checkpoint.
    bool Update(const scene::ParkourCourse& course, scene::Vec3 position,
                bool grounded, float dt) {
        if (!course.enabled || course.checkpoints.empty()) return false;
        if (!finished) seconds += dt;
        if (position.y < course.fallY) { ++falls; return true; }
        if (!grounded || finished) return false;
        const auto next = checkpoint + 1;
        if (next < course.checkpoints.size()) {
            const auto& c = course.checkpoints[next];
            if (std::fabs(position.x-c.spawn.x) <= c.halfExtents.x &&
                std::fabs(position.z-c.spawn.z) <= c.halfExtents.z &&
                std::fabs(position.y-c.spawn.y) <= c.halfExtents.y) {
                checkpoint = next;
                finished = next + 1 == course.checkpoints.size();
            }
        }
        return false;
    }
};
} // namespace gameplay
