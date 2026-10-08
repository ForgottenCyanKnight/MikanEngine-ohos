#include "SDL_ohostouch.h"
#include "../../events/SDL_touch_c.h"
#include "SDL_internal.h"

void OHOS_OnTouch(SDL_OHOSTouchEvent event)
{
    if (SDL_AddTouch(event.deviceId, SDL_TOUCH_DEVICE_DIRECT, "") < 0) {
        SDL_Log("Cannot add touch");
        return;
    }
    // Diagnostics: DOWN/UP/CANCEL forwarding with the touch device id, so a
    // deviceId change between gestures (fresh uinput virtual device) is
    // visible next to the app-level SDL3_TOUCH logs.
    if (event.type != SDL_EVENT_FINGER_MOTION) {
        SDL_Log("SDL3_OHOS_TOUCH send type=%d dev=%lld finger=%lld x=%.3f y=%.3f",
            (int)event.type, (long long)event.deviceId,
            (long long)event.fingerId, event.x, event.y);
    }

    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    {
        SDL_SendTouch(event.timestamp, event.deviceId, event.fingerId, NULL, SDL_EVENT_FINGER_DOWN, event.x, event.y, event.p);
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
    {
        SDL_SendTouchMotion(event.timestamp, event.deviceId, event.fingerId, NULL, event.x, event.y, event.p);
        break;
    }
    case SDL_EVENT_FINGER_UP:
    {
        SDL_SendTouch(event.timestamp, event.deviceId, event.fingerId, NULL, SDL_EVENT_FINGER_UP, event.x, event.y, event.p);
        break;
    }
    case SDL_EVENT_FINGER_CANCELED:
    {
        SDL_SendTouch(event.timestamp, event.deviceId, event.fingerId, NULL, SDL_EVENT_FINGER_CANCELED, event.x, event.y, event.p);
        break;
    }
    }
}
