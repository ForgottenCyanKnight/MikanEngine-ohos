// Minimal RHI (render hardware interface) for the SDL3 OHOS sample.
//
// Why this exists
// ---------------
// The x86_64 OHOS emulator routes every Vulkan call through an `all_out` proxy
// process that talks to the host over an express "vring".  Under sustained
// present traffic that bridge wedges: each call then blocks for the proxy's
// internal ~4 s timeout, a frame ends up costing tens of seconds, and the guest
// trips the 6 s appfreeze watchdog -> `Guest No Response` -> the emulator kills
// the VM.  The same emulator runs GLES over a completely different transport
// (DGLES) and is stable, which is what the sibling SDL2 project relies on.
//
// So the rendering backend has to be selectable, not compiled in:
//
//   x86_64  (emulator)     -> GLES   (DGLES bridge, stable)
//   aarch64 (real device)  -> Vulkan (the target the engine is actually built for)
//
// Scope
// -----
// This is a *backend-level* RHI: one interface per renderer, each backend owning
// its own device, swapchain, resources and scene.  It deliberately does NOT try
// to abstract buffers/pipelines/textures into shared handle types -- at this
// size that would only add indirection without sharing real code.  If a second
// consumer ever needs to assemble scenes itself, the natural next step is to
// lift the scene description (meshes, materials, camera) into this header and
// let both backends consume it.

#pragma once

#include <cstdint>

struct SDL_Window;

namespace physics {
class JoltGameplayPhysics;
}

namespace scene {
struct Definition;
}

namespace rhi {

enum class Backend {
    kVulkan,
    kGles,
};

// Top-level app screen.  The touch layer routes input differently outside
// gameplay (raw taps instead of stick/look/buttons); the renderer owns the
// state and the menu layout/hit-testing.
enum class UiScreen {
    MainMenu,
    Settings,
    Gameplay,
    // Gameplay remains rendered behind a compact Mikan-style display-settings
    // sheet; the touch layer routes this screen as menu input.
    GameplaySettings,
    // The scene is frozen behind a Mikan-style death/retry overlay; the
    // touch layer routes RETRY/MENU taps instead of gameplay controls.
    GameplayRetry,
    // Gameplay frozen behind the inventory sheet (equipment slots + item
    // grid); the touch layer routes this screen as menu input.
    GameplayInventory,
};

const char* BackendName(Backend backend);

// Picks the backend for this device.
//
// Order of precedence:
//   1. the MIKAN_RHI environment variable ("vulkan" / "gles"), for A/B testing
//      on a single device without rebuilding;
//   2. the architecture default -- GLES on x86_64 (emulator), Vulkan elsewhere.
Backend SelectBackend();

// Window creation belongs to the backend: the SDL flag differs
// (SDL_WINDOW_VULKAN vs SDL_WINDOW_OPENGL), the OHOS port builds the EGL surface
// from the window's OPENGL flag at creation time (SDL_ohos.c:OHOS_windowDataFill),
// and the GL attributes below only take effect if they are set *before* the
// window exists.
SDL_Window* CreateWindow(Backend backend, const char* title, int width, int height);

// Per-frame camera input assembled by the touch layer (touch_controller.h).
// Deltas are consumed exactly once by the renderer that implements it; the
// Vulkan backend keeps its fixed turntable camera and ignores these for now.
struct CameraInput {
    float lookDeltaX = 0.0f;  // accumulated drag pixels since last frame
    float lookDeltaY = 0.0f;
    float zoomDelta = 0.0f;   // pinch spread change (positive = fingers apart)
    float moveX = 0.0f;       // joystick axis, [-1, 1], +x = right
    float moveY = 0.0f;       // joystick axis, [-1, 1], +y = down

    // On-screen joystick overlay, in window pixels (y down).
    float stickBaseX = 0.0f;
    float stickBaseY = 0.0f;
    float stickKnobX = 0.0f;
    float stickKnobY = 0.0f;
    float stickRadius = 0.0f;
    float knobRadius = 0.0f;
    bool stickActive = false;
    bool showOverlay = false;

    // Action buttons (MikanEngine InputController::TouchAction style), docked
    // bottom-right in window pixels.  The touch layer owns hit-testing; the
    // renderer draws the circles and consumes the edge-triggered press mask.
    static constexpr int kActionButtonCount = 4;
    // Bit i (of kActionButtonCount) set for exactly one frame per press.
    // 0 = Attack, 1 = Jump, 2 = Sprint, 3 = Crouch.
    int actionButtonPressedMask = 0;
    float actionButtonX[kActionButtonCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    float actionButtonY[kActionButtonCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    float actionButtonRadius = 0.0f;
    // Active levels: Sprint (index 2) is latched by tapping; others follow hold.
    bool actionButtonHeld[kActionButtonCount] = {false, false, false, false};

    // Main-menu / settings taps: the touch layer forwards raw tap edges (down
    // position at lift, window pixels); the renderer owns layout, drawing and
    // hit-testing for those screens.  Consumed exactly once per frame.
    bool menuTapEdge = false;
    float menuTapX = 0.0f;
    float menuTapY = 0.0f;
};

// The interface both backends implement.  Signatures intentionally mirror the
// shape the Vulkan renderer already had, so owning a swapchain, being called
// once per frame with the current size, and rebuilding lazily on resize all
// keep working unchanged.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Stable identifier, used in logs to tell which backend actually came up.
    virtual const char* Name() const = 0;

    // Called once at start-up and then every frame with the current drawable
    // size.  Implementations must be idempotent and cheap when nothing changed.
    virtual bool Initialize(SDL_Window* window, uint64_t width, uint64_t height) = 0;

    // One frame.  Must not block indefinitely: a backend whose device has gone
    // away has to report failure rather than stall the calling thread.
    virtual bool DrawOnce() = 0;

    virtual void Destroy() = 0;

    // OHOS can tear down and recreate its native surface while the application
    // is backgrounded. These callbacks run on the renderer's owning thread;
    // implementations must not retain assumptions about a surface across them.
    virtual void OnApplicationBackground() {}
    virtual bool OnApplicationForeground() { return true; }

    // Optional: feed one frame of touch/mouse camera input.  Default no-op so
    // backends without camera control (Vulkan turntable) stay untouched.
    virtual void PushCameraInput(const CameraInput& input) { (void)input; }

    // Both renderers consume the same gameplay collision/movement state.  The
    // app owns the physics world and keeps it alive for the renderer lifetime.
    virtual void SetPhysicsWorld(physics::JoltGameplayPhysics* world) { (void)world; }

    // The app owns this immutable parsed scene for the entire renderer
    // lifetime. Both backends receive the exact same Mikan-authored snapshot.
    virtual void SetSceneDefinition(const scene::Definition& definition) { (void)definition; }

    // Current top-level screen, used by the main loop to tell the touch layer
    // how to route input.  Defaults to Gameplay so simple backends (Vulkan
    // turntable) never see menu mode.
    virtual UiScreen CurrentScreen() const { return UiScreen::Gameplay; }
};

// Implemented by the backend translation units.
IRenderer* CreateVulkanRenderer();
IRenderer* CreateGlesRenderer();

IRenderer* CreateRenderer(Backend backend);

} // namespace rhi
