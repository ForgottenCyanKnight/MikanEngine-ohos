#include "touch_controller.h"
#include "weapon_gameplay.h"
#include "SDL_ohostouch_filter.h"
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
extern "C" void SDL_Log(const char*,...) {}
static int checks=0;
static void Check(bool ok,const char* message) {
    ++checks; if (!ok) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
int main() {
    constexpr float w=2412,h=1084;
    TouchController controller;
    std::set<int> active;
    int duplicateDownCancels=0;
    // Exercise the production snapshot filter plus the real app controller.
    // Reproduce SDL_SendTouch's cancel-before-repeated-DOWN behaviour as well.
    auto point=[&](Uint32 callback,int changed,int id,bool pressed,float x,float y) {
        const Uint32 type=SDL_OHOS_NormalizeTouchAction(callback,id==changed,pressed);
        if (!type) return;
        SDL_Event event{}; event.type=type; event.tfinger.fingerID=id;
        event.tfinger.x=x/w; event.tfinger.y=y/h;
        if (type==SDL_EVENT_FINGER_DOWN) {
            if (active.count(id)) {
                auto cancel=event; cancel.type=SDL_EVENT_FINGER_CANCELED;
                controller.HandleEvent(cancel,w,h); ++duplicateDownCancels;
            }
            active.insert(id);
        } else if (type==SDL_EVENT_FINGER_UP || type==SDL_EVENT_FINGER_CANCELED) active.erase(id);
        controller.HandleEvent(event,w,h);
    };
    auto moving=[&]() {
        const auto input=controller.FrameInput();
        Check(input.stickActive && std::fabs(input.moveX)<.001f && input.moveY<-.99f,
            "left stick stays fully forward across right-finger changes");
        Check(std::fabs(input.stickBaseX-320)<.01f && std::fabs(input.stickBaseY-760)<.01f,
            "floating joystick keeps captured touchdown anchor");
        return input;
    };
    point(SDL_EVENT_FINGER_DOWN,1,1,true,320,760);
    point(SDL_EVENT_FINGER_MOTION,1,1,true,320,640);
    moving();
    for (int repeat=0;repeat<8;++repeat) for (int action=0;action<4;++action) {
        const auto button=gameplay::ActionButton(action,w,h);
        // The left snapshot can still contain its original DOWN. Production
        // dispatch uses callback.id/type, never that stale per-point type.
        point(SDL_EVENT_FINGER_DOWN,2,1,true,320,640);
        point(SDL_EVENT_FINGER_DOWN,2,2,true,button.x,button.y);
        auto input=moving();
        Check(input.actionButtonPressedMask==(1<<action),"right action fires once without reacquiring left touch");
        Check(controller.FrameInput().actionButtonPressedMask==0,"action edge is consumed exactly once");
        // Reverse snapshot ordering on lift, including a released stale point.
        point(SDL_EVENT_FINGER_UP,2,2,false,button.x,button.y);
        point(SDL_EVENT_FINGER_UP,2,1,true,320,640);
        moving();
        point(SDL_EVENT_FINGER_MOTION,1,2,false,button.x,button.y);
        point(SDL_EVENT_FINGER_MOTION,1,1,true,320,640);
        Check(controller.FrameInput().actionButtonPressedMask==0,"old released button snapshot cannot retrigger action");
    }
    Check(duplicateDownCancels==0,"no synthetic cancellation from duplicate native DOWN snapshots");
    point(SDL_EVENT_FINGER_UP,1,1,false,320,640);
    auto input=controller.FrameInput();
    Check(!input.stickActive && input.moveX==0 && input.moveY==0,"actual left release stops movement");
    point(SDL_EVENT_FINGER_DOWN,1,1,true,320,760);
    point(SDL_EVENT_FINGER_MOTION,1,1,true,320,640);
    const auto jump=gameplay::ActionButton(1,w,h);
    point(SDL_EVENT_FINGER_DOWN,2,1,true,320,640);
    point(SDL_EVENT_FINGER_DOWN,2,2,true,jump.x,jump.y);
    point(SDL_EVENT_FINGER_CANCELED,2,1,false,320,640);
    point(SDL_EVENT_FINGER_CANCELED,2,2,false,jump.x,jump.y);
    input=controller.FrameInput();
    Check(!input.stickActive && !input.actionButtonHeld[1],"surface cancellation clears both finger roles");
    std::printf("Multi-touch tests passed: %d checks, 32 simultaneous movement/action cycles.\n",checks);
}
