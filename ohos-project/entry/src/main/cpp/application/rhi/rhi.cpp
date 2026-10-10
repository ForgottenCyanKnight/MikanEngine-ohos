#include "rhi.h"

#include <SDL3/SDL.h>

namespace rhi {
namespace {

// Logged once so a run's hilog always states which backend was chosen and why.
Backend ResolveBackend(const char* reason, Backend backend)
{
    SDL_Log("SDL3_RHI backend=%s reason=%s", BackendName(backend), reason);
    return backend;
}

} // namespace

const char* BackendName(Backend backend)
{
    switch (backend) {
    case Backend::kGles:
        return "gles";
    case Backend::kVulkan:
    default:
        return "vulkan";
    }
}

Backend SelectBackend()
{
    const char* requested = SDL_getenv("MIKAN_RHI");
    if (requested != nullptr && requested[0] != '\0') {
        if (SDL_strcasecmp(requested, "gles") == 0 || SDL_strcasecmp(requested, "gl") == 0) {
            return ResolveBackend("env", Backend::kGles);
        }
        if (SDL_strcasecmp(requested, "vulkan") == 0 || SDL_strcasecmp(requested, "vk") == 0) {
            return ResolveBackend("env", Backend::kVulkan);
        }
        SDL_Log("SDL3_RHI stage=unknown_backend_requested value=%s", requested);
    }

#if defined(__x86_64__) || defined(_M_X64)
    // The x86_64 emulator is the only x86 target this sample runs on.  Its
    // express Vulkan bridge is unusable: first observed as a wedge within
    // seconds under sustained present traffic (a 3-vertex triangle was
    // enough), and re-tested on the 2026-09 emulator image it now hard-crashes
    // the whole qemu process ~12s after launch (hilog never even got a chance
    // to flush the backend logs).  The GLES path uses the separate DGLES
    // transport and does not have that failure mode.
    return ResolveBackend("x86_64_emulator_default", Backend::kGles);
#else
    // arm64 real-device baseline: keep the finished GLES route active while
    // Vulkan is brought up to screenshot parity.  Set MIKAN_RHI=vulkan for an
    // explicit Vulkan A/B run without changing the production baseline.
    return ResolveBackend("arm64_gles_default", Backend::kGles);
#endif
}

SDL_Window* CreateWindow(Backend backend, const char* title, int width, int height)
{
    if (backend == Backend::kGles) {
        // All of these must be set before SDL_CreateWindow: on OHOS the EGL
        // surface is created from inside the window-creation path, and the EGL
        // config is chosen there and then -- a depth buffer that was not asked
        // for at that moment cannot be added later.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        return SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    }
    return SDL_CreateWindow(title, width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
}

IRenderer* CreateRenderer(Backend backend)
{
    switch (backend) {
    case Backend::kGles:
        return CreateGlesRenderer();
    case Backend::kVulkan:
    default:
        return CreateVulkanRenderer();
    }
}

} // namespace rhi
