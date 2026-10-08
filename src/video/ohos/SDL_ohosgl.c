#include "SDL_internal.h"
#ifdef SDL_VIDEO_DRIVER_OHOS
#include "../../core/ohos/SDL_ohos.h"
#include "SDL_ohosvideo.h"

bool OHOS_GLES_MakeCurrent(SDL_VideoDevice *_this, SDL_Window *window, SDL_GLContext context)
{
    if (window && context) {
        OHOS_windowDataFill(window);
        if (window->internal == NULL || window->internal->native_window == NULL ||
            window->internal->egl_surface == EGL_NO_SURFACE) {
            SDL_SetError("OHOS native window surface is not ready");
            return false;
        }
        return SDL_EGL_MakeCurrent(_this, window->internal->egl_surface, context);
    } else {
        return SDL_EGL_MakeCurrent(_this, NULL, NULL);
    }
}

SDL_GLContext OHOS_GLES_CreateContext(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_GLContext result;

    OHOS_LockPage();
    
    OHOS_windowDataFill(window);
    if (window == NULL || window->internal == NULL ||
        window->internal->native_window == NULL ||
        window->internal->egl_surface == EGL_NO_SURFACE) {
        SDL_SetError("OHOS native window surface is not ready for EGL context creation");
        OHOS_UnlockPage();
        return NULL;
    }
    result = SDL_EGL_CreateContext(_this, window->internal->egl_surface);

    OHOS_UnlockPage();

    return result;
}

bool OHOS_GLES_SwapWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    bool result;

    OHOS_LockPage();

    result = SDL_EGL_SwapBuffers(_this, window->internal->egl_surface);

    OHOS_UnlockPage();

    return result;
}

bool OHOS_GLES_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    return SDL_EGL_LoadLibrary(_this, path, (NativeDisplayType)0, 0);
}

#endif
