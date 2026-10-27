#ifndef SDL_OHOSTOUCH_FILTER_H
#define SDL_OHOSTOUCH_FILTER_H
#include <SDL3/SDL_events.h>

/* XComponent reports a snapshot of all points; point.type can still contain
   another finger's previous DOWN/UP. Only callback.id owns the current edge. */
static inline Uint32 SDL_OHOS_NormalizeTouchAction(Uint32 callbackAction, bool changed, bool pressed)
{
    if (callbackAction == SDL_EVENT_FINGER_CANCELED) return callbackAction;
    if (changed) return callbackAction;
    return pressed ? SDL_EVENT_FINGER_MOTION : 0;
}
#endif
