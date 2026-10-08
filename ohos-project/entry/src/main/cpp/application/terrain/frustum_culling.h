#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace terrain {
// Terrain vertices already contain their scene-to-world transform.
struct Bounds {
    float minimum[3] = {INFINITY, INFINITY, INFINITY};
    float maximum[3] = {-INFINITY, -INFINITY, -INFINITY};
    void Add(const float* p) {
        for (int i=0; i<3; ++i) {
            minimum[i] = std::min(minimum[i], p[i]);
            maximum[i] = std::max(maximum[i], p[i]);
        }
    }
};
// Animated actors use conservative local envelopes instead of bind-pose mesh bounds.
inline Bounds ActorBounds(bool enemy) {
    Bounds b; const float radius = enemy ? 3.0f : 2.0f;
    float lo[3]={-radius,-radius,-radius}, hi[3]={radius,radius,radius};
    b.Add(lo); b.Add(hi); return b;
}
inline Bounds TransformBounds(const Bounds& b, const float* m) {
    Bounds result;
    for (int corner=0; corner<8; ++corner) {
        float p[3];
        for(int axis=0; axis<3; ++axis) p[axis]=(corner&(1<<axis)) ? b.maximum[axis] : b.minimum[axis];
        float q[3];
        for(int row=0; row<3; ++row)
            q[row]=m[row]*p[0]+m[4+row]*p[1]+m[8+row]*p[2]+m[12+row];
        result.Add(q);
    }
    return result;
}
struct Frustum {
    float planes[6][4]{};
    // Column-major clip matrix; Vulkan Z is [0,w], GLES Z is [-w,w].
    Frustum(const float* m, bool zeroToOne) {
        for (int c=0; c<4; ++c) {
            planes[0][c]=m[c*4+3]+m[c*4];
            planes[1][c]=m[c*4+3]-m[c*4];
            planes[2][c]=m[c*4+3]+m[c*4+1];
            planes[3][c]=m[c*4+3]-m[c*4+1];
            planes[4][c]=zeroToOne ? m[c*4+2] : m[c*4+3]+m[c*4+2];
            planes[5][c]=m[c*4+3]-m[c*4+2];
        }
    }
    bool Intersects(const Bounds& b) const {
        for (int i=0; i<3; ++i)
            if (!std::isfinite(b.minimum[i]) || !std::isfinite(b.maximum[i])) return true;
        for (const auto& p : planes) {
            float distance=p[3];
            for (int i=0; i<3; ++i) distance += p[i]*(p[i]>=0 ? b.maximum[i] : b.minimum[i]);
            // One centimetre of world-space tolerance, independent of plane scale.
            const float tolerance=0.01f*std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            if (distance < -tolerance) return false;
        }
        return true;
    }
};
} // namespace terrain
