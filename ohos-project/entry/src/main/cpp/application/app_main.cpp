// Application entry point.
//
// All this does is pick a rendering backend, bring it up on a window that
// matches it, and run the frame loop.  Everything device-specific lives behind
// rhi::IRenderer -- see rhi/rhi.h for why the backend has to be selectable at
// all on this platform.

#include "rhi/rhi.h"
#include "audio/audio_manager.h"
#include "physics/jolt_gameplay_physics.h"
#include "scene/scene_definition.h"
#include "touch_controller.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

// Exported by libSDL3.so (core/ohos/SDL_ohos.c).  Notifies the ArkTS page
// that the engine is about to show its first frame, so the splash overlay
// can be removed -- the same hook MikanEngine uses on Android.
extern "C" void OHOS_HideNativeSplash(void);

namespace {

constexpr int kInitialWidth = 1024;
constexpr int kInitialHeight = 1024;

std::atomic<bool> gAppInBackground{false};
std::atomic<bool> gRendererResumePending{false};
std::timed_mutex gRendererFrameMutex;
std::mutex gRendererPauseMutex;
std::condition_variable gRendererPauseCondition;
bool gRendererPauseAcknowledged = false;

bool SDLCALL ApplicationLifecycleEventWatch(void*, SDL_Event* event)
{
    if (event == nullptr) {
        return true;
    }

    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        // OHOS invokes lifecycle events from its Ability thread. Publish the
        // pause before taking this barrier so the render thread cannot start
        // another frame after the in-flight frame has drained.
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND &&
            !gAppInBackground.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> pauseLock(gRendererPauseMutex);
            gRendererPauseAcknowledged = false;
        }
        gAppInBackground.store(true, std::memory_order_release);
        {
            // This watch runs synchronously on the OHOS Ability thread. Drain
            // a normal in-flight frame when possible, but never let a stalled
            // driver keep the platform lifecycle callback blocked indefinitely.
            std::unique_lock<std::timed_mutex> frameBarrier(gRendererFrameMutex,
                std::defer_lock);
            if (!frameBarrier.try_lock_for(std::chrono::milliseconds(100))) {
                SDL_Log("SDL3_LIFE stage=renderer_frame_drain_timeout");
            }
        }
        if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            std::unique_lock<std::mutex> pauseLock(gRendererPauseMutex);
            const bool acknowledged = gRendererPauseCondition.wait_for(
                pauseLock, std::chrono::milliseconds(300), [] {
                    return gRendererPauseAcknowledged;
                });
            if (!acknowledged) {
                SDL_Log("SDL3_LIFE stage=renderer_pause_timeout");
            }
        }
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        gRendererResumePending.store(true, std::memory_order_release);
        gAppInBackground.store(false, std::memory_order_release);
        break;
    default:
        break;
    }
    return true;
}

struct Session {
    SDL_Window* window = nullptr;
    std::unique_ptr<rhi::IRenderer> renderer;
    int width = 0;
    int height = 0;
};

// Brings one backend all the way up.  On any failure the window is torn down
// again, so the caller can retry the other backend from a clean slate rather
// than leaving a half-initialised window behind.
Session BringUp(rhi::Backend backend, const scene::Definition& sceneDefinition)
{
    Session session;
    session.window = rhi::CreateWindow(backend, "SDL3 OHOS Skybox Model", kInitialWidth, kInitialHeight);
    if (session.window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_RHI stage=window_create_failed backend=%s error=%s",
            rhi::BackendName(backend), SDL_GetError());
        return session;
    }

    SDL_GetWindowSize(session.window, &session.width, &session.height);
    session.renderer.reset(rhi::CreateRenderer(backend));
    if (session.renderer != nullptr) {
        session.renderer->SetSceneDefinition(sceneDefinition);
    }
    if (session.renderer == nullptr ||
        !session.renderer->Initialize(session.window, static_cast<uint64_t>(session.width),
            static_cast<uint64_t>(session.height))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_RHI stage=backend_init_failed backend=%s error=%s",
            rhi::BackendName(backend), SDL_GetError());
        session.renderer.reset();
        SDL_DestroyWindow(session.window);
        session.window = nullptr;
        return session;
    }

    SDL_Log("SDL3_RHI stage=backend_ready backend=%s size=%dx%d",
        session.renderer->Name(), session.width, session.height);
    return session;
}

} // namespace

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_RHI stage=sdl_init_failed error=%s",
            SDL_GetError());
        return 1;
    }
    scene::Definition sceneDefinition{};
    std::string sceneError;
    const bool mikanSceneLoaded = scene::LoadMikanSceneRawFile(
        "scenes/main.json", sceneDefinition, sceneError);
    if (mikanSceneLoaded) {
        SDL_Log("SDL3_SCENE stage=loaded format=%d game=%s entities=%zu static_cubes=%zu "
                "lights=%zu unsupported_meshes=%zu camera_fov=%.1f",
            sceneDefinition.formatVersion, sceneDefinition.game.c_str(),
            sceneDefinition.entities.size(), sceneDefinition.StaticCubeCount(),
            sceneDefinition.lights.size(), sceneDefinition.unsupportedVisibleMeshCount,
            sceneDefinition.mainCamera.fieldOfViewDegrees);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_SCENE stage=load_failed path=scenes/main.json error=%s; using built-in scene",
            sceneError.c_str());
    }
    AudioManager& audioManager = AudioManager::GetInstance();
    if (!audioManager.Initialize()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_AUDIO stage=renderer_init_failed; continuing without audio");
    } else if (!audioManager.LoadAudio("hit", "audio/hit.wav")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_AUDIO stage=hit_clip_load_failed path=audio/hit.wav");
    } else {
        SDL_Log("SDL3_AUDIO stage=hit_clip_ready path=audio/hit.wav");
    }
    const bool lifecycleWatchInstalled = SDL_AddEventWatch(ApplicationLifecycleEventWatch, nullptr);
    if (!lifecycleWatchInstalled) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_LIFE stage=event_watch_failed error=%s", SDL_GetError());
    }

    physics::JoltGameplayPhysics physicsWorld;
    if (!physicsWorld.Initialize()) {
        // Keep the renderer bring-up path usable on a device where a Jolt
        // allocation or ABI check fails; the RHI has a conservative movement
        // fallback, while the failure remains visible in the native log.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SDL3_PHYSICS stage=jolt_init_failed fallback=renderer_movement");
    } else if (mikanSceneLoaded) {
        const float runtimeGroundTopY = physics::kGroundCollider.centerY +
            physics::kGroundCollider.halfExtentY;
        const std::vector<scene::StaticBoxCollider> sceneColliders =
            scene::BuildStaticBoxColliders(sceneDefinition, runtimeGroundTopY);
        if (!physicsWorld.SetStaticSceneColliders(sceneColliders)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "SDL3_SCENE stage=collider_sync_failed boxes=%zu",
                sceneColliders.size());
        } else {
            SDL_Log("SDL3_SCENE stage=colliders_synced static_cubes=%zu boxes=%zu",
                sceneDefinition.StaticCubeCount(), sceneColliders.size());
        }
    }

    if (mikanSceneLoaded && physicsWorld.IsReady() &&
        !physicsWorld.SetTerrainColliders(sceneDefinition)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_TERRAIN stage=collision_sync_failed");
    }

    const rhi::Backend preferred = rhi::SelectBackend();
    Session session = BringUp(preferred, sceneDefinition);

    // Exactly one fallback hop.  The purpose is robustness on hardware we have
    // not seen: if a device cannot create a Vulkan device, running the same
    // scene on GLES beats refusing to start.  It never triggers on the
    // x86_64 emulator, where GLES is already the preferred backend.
    if (session.window == nullptr) {
        const rhi::Backend alternative =
            preferred == rhi::Backend::kVulkan ? rhi::Backend::kGles : rhi::Backend::kVulkan;
        SDL_Log("SDL3_RHI stage=fallback backend=%s", rhi::BackendName(alternative));
        session = BringUp(alternative, sceneDefinition);
    }
    if (session.window == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL3_RHI stage=no_backend_available");
        audioManager.Shutdown();
        SDL_Quit();
        return 1;
    }
    session.renderer->SetPhysicsWorld(&physicsWorld);

    bool running = true;
    bool splashHidden = false;
    bool rendererSuspended = false;
    bool restoreFailureLogged = false;
    TouchController touch;
    while (running) {
        SDL_Event event;
        // Sync the renderer's screen state before polling: the touch layer
        // routes taps vs stick/look/buttons based on it.  The value is one
        // frame old (screens only change during DrawOnce), which is fine --
        // a tap misrouted across a screen switch lands on a same-family
        // layout and costs nothing.
        touch.SetScreen(session.renderer->CurrentScreen());
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
                event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
                // SDL lifecycle events are primarily handled by the event
                // watch above; this is a fallback if an event is also queued.
                gAppInBackground.store(true, std::memory_order_release);
            } else if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
                gRendererResumePending.store(true, std::memory_order_release);
                gAppInBackground.store(false, std::memory_order_release);
            } else {
                touch.HandleEvent(event, static_cast<float>(session.width),
                    static_cast<float>(session.height));
            }
        }
        if (!running) {
            break;
        }
        bool shouldWaitForSurface = false;
        {
            std::lock_guard<std::timed_mutex> frameLock(gRendererFrameMutex);
            if (gAppInBackground.load(std::memory_order_acquire)) {
                if (!rendererSuspended) {
                    session.renderer->OnApplicationBackground();
                    audioManager.Pause();
                    rendererSuspended = true;
                    {
                        std::lock_guard<std::mutex> pauseLock(gRendererPauseMutex);
                        gRendererPauseAcknowledged = true;
                    }
                    gRendererPauseCondition.notify_all();
                    SDL_Log("SDL3_LIFE stage=renderer_paused backend=%s", session.renderer->Name());
                }
                shouldWaitForSurface = true;
            } else {
                const bool foregroundEvent =
                    gRendererResumePending.exchange(false, std::memory_order_acq_rel);
                if (rendererSuspended || foregroundEvent) {
                    if (!session.renderer->OnApplicationForeground()) {
                        rendererSuspended = true;
                        gRendererResumePending.store(true, std::memory_order_release);
                        shouldWaitForSurface = true;
                        if (!restoreFailureLogged) {
                            SDL_Log("SDL3_LIFE stage=renderer_resume_wait backend=%s error=%s",
                                session.renderer->Name(), SDL_GetError());
                            restoreFailureLogged = true;
                        }
                    } else {
                        rendererSuspended = false;
                        audioManager.Resume();
                        restoreFailureLogged = false;
                        SDL_Log("SDL3_LIFE stage=renderer_resume_requested backend=%s",
                            session.renderer->Name());
                    }
                }

                if (!rendererSuspended && !shouldWaitForSurface) {
                    // The drawable size is re-read every frame: OHOS may
                    // replace its native window together with the surface.
                    SDL_GetWindowSize(session.window, &session.width, &session.height);
                    if (session.width > 0 && session.height > 0) {
                        touch.SetViewport(static_cast<float>(session.width),
                            static_cast<float>(session.height));
                        // Do not issue DrawOnce against stale graphics state
                        // when a surface/swapchain rebuild is still pending.
                        if (session.renderer->Initialize(session.window,
                            static_cast<uint64_t>(session.width),
                            static_cast<uint64_t>(session.height))) {
                            restoreFailureLogged = false;
                            session.renderer->PushCameraInput(touch.FrameInput());
                            const bool framePresented = session.renderer->DrawOnce();
                            if (framePresented && !splashHidden) {
                                splashHidden = true;
                                SDL_Log("SDL3_RHI stage=first_frame_presented hide_splash");
                                OHOS_HideNativeSplash();
                            }
                        } else {
                            shouldWaitForSurface = true;
                            if (!restoreFailureLogged) {
                                SDL_Log("SDL3_LIFE stage=renderer_initialize_pending backend=%s error=%s",
                                    session.renderer->Name(), SDL_GetError());
                                restoreFailureLogged = true;
                            }
                        }
                    } else {
                        shouldWaitForSurface = true;
                    }
                }
            }
        }
        SDL_Delay(shouldWaitForSurface ? 80 : 16);
    }

    if (lifecycleWatchInstalled) {
        SDL_RemoveEventWatch(ApplicationLifecycleEventWatch, nullptr);
    }
    session.renderer->Destroy();
    session.renderer.reset();
    physicsWorld.Shutdown();
    audioManager.StopAll();
    audioManager.Shutdown();
    SDL_DestroyWindow(session.window);
    SDL_Quit();
    return 0;
}
