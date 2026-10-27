#include "animation/pose_transition.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static int checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static bool Near(float a, float b) { return std::fabs(a - b) < .0001f; }
struct Pose {
    std::vector<float> t{0, 0, 0, 0, 0, 0};
    std::vector<float> r{0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<float> s{1, 1, 1, 1, 1, 1};
};

int main()
{
    animation::PoseTransition fade;
    Pose pose;
    Check(!fade.Apply(0, .016f, .2f, pose.t, pose.r, pose.s), "first pose does not fade from an uninitialised skeleton");
    pose.t[0] = 10; pose.s[0] = 3; pose.r[1] = 1; pose.r[3] = 0;
    Check(fade.Apply(1, .016f, .2f, pose.t, pose.r, pose.s), "new animation starts a transition");
    Check(Near(pose.t[0], 0) && Near(pose.r[3], 1), "first switched frame matches last displayed pose");
    pose.t[0] = 10; pose.s[0] = 3; pose.r[1] = 1; pose.r[3] = 0;
    Check(!fade.Apply(1, .1f, .2f, pose.t, pose.r, pose.s), "same animation does not restart fade");
    Check(Near(pose.t[0], 5) && Near(pose.s[0], 2), "TRS midpoint interpolates position and scale");
    Check(Near(pose.r[1], std::sqrt(.5f)) && Near(pose.r[3], std::sqrt(.5f)), "midpoint rotation is a rigid 90 degree rotation");
    pose.t[0] = 20;
    fade.Apply(2, .016f, .2f, pose.t, pose.r, pose.s);
    Check(Near(pose.t[0], 5), "interruption starts at current mixed pose, not old clip endpoint");
    pose.t[0] = 20;
    fade.Apply(2, .1f, .2f, pose.t, pose.r, pose.s);
    Check(Near(pose.t[0], 12.5f), "interrupted transition progresses continuously");
    pose.t[0] = 20;
    fade.Apply(2, .1f, .2f, pose.t, pose.r, pose.s);
    Check(Near(pose.t[0], 20) && !fade.Active(), "transition converges to playing target");

    float a[4] = {0, .70710678f, 0, .70710678f};
    float b[4] = {0, -.70710678f, 0, -.70710678f}, q[4];
    for (int i = 0; i <= 10; ++i) {
        animation::Slerp(a, b, i / 10.f, q);
        Check(Near(std::fabs(q[1]), std::sqrt(.5f)) && Near(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3], 1),
            "antipodal quaternions remain stable and normalised");
    }
    const float invalid[4] = {0, 0, 0, 0};
    animation::Slerp(invalid, invalid, .5f, q);
    Check(Near(q[3], 1), "degenerate quaternions remain finite");

    fade.Reset(); pose = Pose{};
    std::vector<std::uint8_t> upperBody{0, 1};
    fade.Apply(0, .016f, .2f, pose.t, pose.r, pose.s, &upperBody);
    pose.t[0] = 10; pose.t[3] = 20;
    fade.Apply(1, .016f, .2f, pose.t, pose.r, pose.s, &upperBody);
    Check(Near(pose.t[0], 10) && Near(pose.t[3], 0), "weapon transition preserves live leg motion");
    pose.t[0] = 15; pose.t[3] = 20;
    fade.Apply(1, .1f, .2f, pose.t, pose.r, pose.s, &upperBody);
    Check(Near(pose.t[0], 15) && Near(pose.t[3], 10), "only upper-body nodes crossfade");
    for (float dt : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN()}) {
        pose.t[3] = 20;
        fade.Apply(1, dt, .2f, pose.t, pose.r, pose.s, &upperBody);
        Check(Near(pose.t[3], 10), "paused or invalid frame time cannot advance fade");
    }
    pose.t[3] = 30;
    fade.Apply(2, .016f, 0, pose.t, pose.r, pose.s, &upperBody);
    Check(Near(pose.t[3], 30) && !fade.Active(), "zero-duration transition is immediate");
    fade.Reset(); pose.t[3] = 50;
    fade.Apply(0, .016f, .2f, pose.t, pose.r, pose.s, &upperBody);
    Check(Near(pose.t[3], 50), "retry and teleport clear stale pose history");
    Check(animation::Duration(animation::Transition::Attack) < animation::Duration(animation::Transition::Locomotion),
        "combat entry stays faster than locomotion");
    std::printf("Animation transition tests passed: %d checks.\n", checks);
}
