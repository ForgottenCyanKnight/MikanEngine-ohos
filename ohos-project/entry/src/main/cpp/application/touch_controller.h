// Touch control layer for the OHOS demo.
//
// Ported from MikanEngine's src/Core InputController touch half (the variant
// that works on the Android build), stripped of glm/imgui:
//
//   - a virtual movement joystick docked lower-left;
//   - every finger outside the joystick/buttons is camera-eligible and is
//     tracked in a table from DOWN to UP;
//   - the pinch state is DERIVED from that table on every event (>= 2
//     camera-eligible fingers = pinch), not assigned once at touchdown: role
//     slots cannot go stale, and a look finger drifting mid-gesture can never
//     turn into rotation because look accumulation is hard-gated while
//     pinching (Mikan UpdateTouchPinchState semantics);
//   - FINGER_CANCELED is treated as FINGER_UP so a lost lift can never leave
//     a phantom tracked finger behind.
//
// SDL finger events arrive normalised to [0, 1]; they are scaled by the
// window size the same way the engine does.  Y grows downward on both sides.
// Once a real finger has been seen, synthetic mouse events (SDL synthesises
// them from touch) are ignored so a single drag is never counted twice.

#pragma once

#include <SDL3/SDL.h>

#include "rhi/rhi.h"  // rhi::CameraInput (button layout lives in the frame input)

class TouchController {
public:
    TouchController()
    {
        for (SDL_FingerID& finger : actionBtnFinger_) {
            finger = static_cast<SDL_FingerID>(-1);
        }
    }

    void HandleEvent(const SDL_Event& event, float width, float height);

    // Keeps the overlay geometry in sync with the window every frame.  Without
    // this the stick sits at its constructed default until the first event
    // happens to run Relayout, so at launch it draws away from its home
    // position with a stale radius.
    void SetViewport(float width, float height);
    void SetWeaponEquipped(bool equipped) { weaponEquipped_ = equipped; }

    // Sets the screen the renderer currently shows.  Outside gameplay this
    // layer stops driving stick/look/buttons and only reports tap edges.
    // Called every frame, so the phantom-claim reset below must only run on
    // an actual screen change: an unconditional reset here used to wipe the
    // SET/BAG claim between a DOWN and its UP (the tap then never fired).
    void SetScreen(rhi::UiScreen screen)
    {
        if (screen == screen_) {
            return;
        }
        screen_ = screen;
        // A pressed gameplay SET/MENU button must not survive a screen switch.
        menuBtnFinger_ = static_cast<SDL_FingerID>(-1);
    }

    // Returns the accumulated deltas for one frame and resets them.
    rhi::CameraInput FrameInput();

private:
    void Relayout(float width, float height);

    // --- tracked touch table (Mikan InputController::touchPoints) -----------
    struct TrackedTouch {
        bool active = false;
        bool cameraEligible = false;  // outside joystick and action buttons
        SDL_FingerID fingerId = static_cast<SDL_FingerID>(-1);
        float x = 0.0f;
        float y = 0.0f;
    };
    static constexpr int kTouchPointCount = 8;
    TrackedTouch tracked_[kTouchPointCount];

    int FindTracked(SDL_FingerID fingerId) const;
    void TrackDown(SDL_FingerID fingerId, float x, float y, bool cameraEligible);
    void TrackMotion(SDL_FingerID fingerId, float x, float y);
    void TrackUp(SDL_FingerID fingerId);
    // Recomputes the derived pinch state from the table.  Returns true while
    // two camera-eligible fingers are down; accumulates zoomDelta_ from the
    // pair distance change and rebaselines look on transitions (exactly the
    // engine's UpdateTouchPinchState).
    bool UpdatePinchState();

    float width_ = 1.0f;
    float height_ = 1.0f;

    // Joystick state (left-docked).
    float baseX_ = 0.0f;
    float baseY_ = 0.0f;
    float baseRadius_ = 60.0f;
    float knobRadius_ = 34.0f;
    float knobX_ = 0.0f;
    float knobY_ = 0.0f;
    float moveX_ = 0.0f;  // [-1, 1], +x = right, +y = down
    float moveY_ = 0.0f;
    bool stickActive_ = false;
    SDL_FingerID stickFinger_ = static_cast<SDL_FingerID>(-1);

    // Look layer: primary drag finger.  Cleared while pinching; reassigned to
    // a surviving finger with a fresh baseline when the pinch ends.
    SDL_FingerID lookFinger_ = static_cast<SDL_FingerID>(-1);
    float lookLastX_ = 0.0f;
    float lookLastY_ = 0.0f;
    bool pinching_ = false;
    float pinchLastDistance_ = 0.0f;

    // Per-frame accumulators.
    float lookDeltaX_ = 0.0f;
    float lookDeltaY_ = 0.0f;
    float zoomDelta_ = 0.0f;

    // Action buttons (bottom-right cluster, MikanEngine TouchAction style):
    // 0 = Attack, 1 = Jump, 2 = Sprint, 3 = Crouch.  A finger that lands inside a button is
    // captured by it until lift; presses queue an edge into pressedMask_.
    float actionBtnX_[rhi::CameraInput::kActionButtonCount] = {};
    float actionBtnY_[rhi::CameraInput::kActionButtonCount] = {};
    float actionBtnRadius_ = 0.0f;
    bool actionBtnHeld_[rhi::CameraInput::kActionButtonCount] = {};
    // Single-touch devices: Sprint is a tap toggle, independent of finger lift.
    bool sprintEnabled_ = false;
    SDL_FingerID actionBtnFinger_[rhi::CameraInput::kActionButtonCount] = {};
    int actionBtnPressedMask_ = 0;

    bool weaponEquipped_ = false;
    bool sawFinger_ = false;  // disables the mouse fallback forever once set
    bool mouseLooking_ = false;

    // Gameplay top-right SET/MENU buttons: claim their finger like an action
    // button; the lift reports a tap edge for the renderer's hit-test.
    SDL_FingerID menuBtnFinger_ = static_cast<SDL_FingerID>(-1);

    // Menu/settings mode: raw tap forwarding.  A tap = a finger that goes down
    // and lifts while this layer is in menu mode; the down position is
    // reported (stable hit-test target, immune to lift-time drift).
    rhi::UiScreen screen_ = rhi::UiScreen::Gameplay;
    bool tapDown_ = false;
    SDL_FingerID tapFinger_ = static_cast<SDL_FingerID>(-1);
    float tapDownX_ = 0.0f;
    float tapDownY_ = 0.0f;
    bool tapPending_ = false;
    float tapX_ = 0.0f;
    float tapY_ = 0.0f;
};
