#include "touch_controller.h"

#include "rhi/rhi.h"

#include <cmath>

namespace {

constexpr float kJoystickScreenAnchorX = 0.16f;  // fraction of width, left-docked
constexpr float kJoystickScreenAnchorY = 0.72f;  // fraction of height, lower area
constexpr float kJoystickRadiusFraction = 0.11f; // of min(w, h)
constexpr float kJoystickCaptureFactor = 1.5f;   // grab zone = 1.5x base

// Action button cluster (bottom-right arc, MikanEngine RenderTouchControls
// layout).  Attack is the lower-right anchor; Jump/Sprint/Crouch sit outside
// it so the four actions keep the same meaning in both RHI paths.
constexpr float kActionButtonReferenceHeight = 1080.0f;
constexpr float kActionButtonMinScale = 0.70f;
constexpr float kActionButtonMaxScale = 1.0f;
constexpr float kActionButtonRadiusAtReference = 84.0f;
constexpr float kActionButtonRingPadding = 5.0f;

// Per-event clamps from MikanEngine InputController: a lost sample or a torn
// batch must never teleport the camera or explode the zoom.
constexpr float kMaxLookDeltaPerEvent = 50.0f;   // px
constexpr float kMaxZoomDeltaPerEvent = 100.0f;  // px

} // namespace

void TouchController::Relayout(float width, float height)
{
    width_ = width > 0.0f ? width : 1.0f;
    height_ = height > 0.0f ? height : 1.0f;
    baseRadius_ = kJoystickRadiusFraction *
        (width_ < height_ ? width_ : height_);
    knobRadius_ = baseRadius_ * 0.55f;
    baseX_ = width_ * kJoystickScreenAnchorX;
    baseY_ = height_ * kJoystickScreenAnchorY;
    if (!stickActive_) {
        knobX_ = baseX_;
        knobY_ = baseY_;
    }
    float layoutScale = height_ / kActionButtonReferenceHeight;
    layoutScale = layoutScale < kActionButtonMinScale ? kActionButtonMinScale : layoutScale;
    layoutScale = layoutScale > kActionButtonMaxScale ? kActionButtonMaxScale : layoutScale;
    const float minRightInset = 96.0f * layoutScale;
    float safeRightInset = width_ * 0.08f;
    if (safeRightInset < minRightInset) {
        safeRightInset = minRightInset;
    }
    actionBtnRadius_ = kActionButtonRadiusAtReference * layoutScale;
    const float rightColumnX = width_ - safeRightInset - actionBtnRadius_
        - kActionButtonRingPadding * layoutScale;
    const float leftColumnX = rightColumnX - 200.0f * layoutScale;
    actionBtnX_[0] = rightColumnX;  // Attack: bottom-right primary action.
    actionBtnY_[0] = height_ - 180.0f * layoutScale;
    actionBtnX_[1] = leftColumnX;   // Jump: outside Attack, lower-left.
    actionBtnY_[1] = height_ - 120.0f * layoutScale;
    actionBtnX_[2] = leftColumnX;   // Sprint: outside Attack, upper-left.
    actionBtnY_[2] = height_ - 320.0f * layoutScale;
    actionBtnX_[3] = rightColumnX;  // Crouch: outside Attack, upper-right.
    actionBtnY_[3] = height_ - 380.0f * layoutScale;
}

void TouchController::SetViewport(float width, float height)
{
    Relayout(width, height);
}

int TouchController::FindTracked(SDL_FingerID fingerId) const
{
    for (int i = 0; i < kTouchPointCount; ++i) {
        if (tracked_[i].active && tracked_[i].fingerId == fingerId) {
            return i;
        }
    }
    return -1;
}

void TouchController::TrackDown(SDL_FingerID fingerId, float x, float y, bool cameraEligible)
{
    int index = FindTracked(fingerId);
    if (index < 0) {
        for (int i = 0; i < kTouchPointCount; ++i) {
            if (!tracked_[i].active) {
                index = i;
                break;
            }
        }
    }
    if (index < 0) {
        return;
    }
    TrackedTouch& point = tracked_[index];
    point.active = true;
    point.cameraEligible = cameraEligible;
    point.fingerId = fingerId;
    point.x = x;
    point.y = y;
}

void TouchController::TrackMotion(SDL_FingerID fingerId, float x, float y)
{
    const int index = FindTracked(fingerId);
    if (index < 0) {
        return;
    }
    tracked_[index].x = x;
    tracked_[index].y = y;
}

void TouchController::TrackUp(SDL_FingerID fingerId)
{
    const int index = FindTracked(fingerId);
    if (index < 0) {
        return;
    }
    TrackedTouch& point = tracked_[index];
    point.active = false;
    point.cameraEligible = false;
    point.fingerId = static_cast<SDL_FingerID>(-1);
    point.x = 0.0f;
    point.y = 0.0f;
}

bool TouchController::UpdatePinchState()
{
    // The pinch pair is derived from the live table on every event, never
    // assigned once at touchdown -- role slots cannot go stale.
    int first = -1;
    int second = -1;
    for (int i = 0; i < kTouchPointCount; ++i) {
        const TrackedTouch& point = tracked_[i];
        if (!point.active || !point.cameraEligible) {
            continue;
        }
        if (first < 0) {
            first = i;
        } else {
            second = i;
            break;
        }
    }

    if (first >= 0 && second >= 0) {
        const float dx = tracked_[first].x - tracked_[second].x;
        const float dy = tracked_[first].y - tracked_[second].y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance)) {
            return pinching_;
        }
        if (!pinching_) {
            // Entering the pinch only establishes the baseline so the second
            // finger's touchdown never jumps the zoom or leaks into look.
            pinching_ = true;
            pinchLastDistance_ = distance;
            lookDeltaX_ = 0.0f;
            lookDeltaY_ = 0.0f;
            lookFinger_ = static_cast<SDL_FingerID>(-1);
            SDL_Log("SDL3_TOUCH stage=pinch_start distance=%.0f", distance);
        } else {
            zoomDelta_ += distance - pinchLastDistance_;
            pinchLastDistance_ = distance;
        }
        return true;
    }

    if (pinching_) {
        pinching_ = false;
        pinchLastDistance_ = 0.0f;
        lookDeltaX_ = 0.0f;
        lookDeltaY_ = 0.0f;
        // After the pinch, if one finger remains, keep rotating the camera
        // with it -- rebaseline from its CURRENT position so the lift instant
        // is never mistaken for a drag (Mikan UpdateTouchPinchState).
        if (first >= 0) {
            lookFinger_ = tracked_[first].fingerId;
            lookLastX_ = tracked_[first].x;
            lookLastY_ = tracked_[first].y;
            SDL_Log("SDL3_TOUCH stage=pinch_end frame_residual=%.0f look_finger=%llu",
                zoomDelta_, (unsigned long long)lookFinger_);
        } else {
            lookFinger_ = static_cast<SDL_FingerID>(-1);
            SDL_Log("SDL3_TOUCH stage=pinch_end frame_residual=%.0f", zoomDelta_);
        }
    }
    return false;
}

void TouchController::HandleEvent(const SDL_Event& event, float width, float height)
{
    Relayout(width, height);

    if (event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
        event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
        sawFinger_ = true;
        // Real panel touches already arrive in screen space (verified across
        // three on-device rounds: an x flip made the left-drawn stick answer
        // touches on the right).  Note uitest-injected events behave mirrored
        // relative to real touches on this port, so automated tap tests cannot
        // arbitrate this -- only real fingers can.
        const float x = event.tfinger.x * width_;
        const float y = event.tfinger.y * height_;
        const SDL_FingerID finger = event.tfinger.fingerID;
        // A canceled touch means the lift was lost at the platform layer;
        // treating it as UP keeps the tracked table free of phantom fingers.
        const bool isUp = event.type == SDL_EVENT_FINGER_UP ||
                          event.type == SDL_EVENT_FINGER_CANCELED;

        // Menu/settings: no stick, no look, no action buttons -- every finger
        // is a potential tap.  Down records the candidate, lift queues the tap
        // at the down position.
        if (screen_ != rhi::UiScreen::Gameplay) {
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                if (!tapDown_) {
                    tapDown_ = true;
                    tapFinger_ = finger;
                    tapDownX_ = x;
                    tapDownY_ = y;
                }
            } else if (isUp) {
                if (tapDown_ && finger == tapFinger_) {
                    tapDown_ = false;
                    tapFinger_ = static_cast<SDL_FingerID>(-1);
                    tapPending_ = true;
                    tapX_ = tapDownX_;
                    tapY_ = tapDownY_;
                }
            }
            return;
        }

        if (event.type == SDL_EVENT_FINGER_DOWN) {
            // The gameplay HUD right-edge buttons (SET at 0.93w,0.26h and
            // BAG at 0.93w,0.42h, radius 0.055*mn) claim their finger before
            // everything else: a press there must never drag the camera or
            // fire an action.  The renderer decides which button the lift
            // position hits and switches screens accordingly.
            if (menuBtnFinger_ == static_cast<SDL_FingerID>(-1)) {
                const float mn = width_ < height_ ? width_ : height_;
                const float mhalf = 0.055f * mn;
                const float mdx = std::fabs(x - width_ * 0.93f);
                if (mdx <= mhalf) {
                    const float setDy = std::fabs(y - height_ * 0.26f);
                    const float bagDy = std::fabs(y - height_ * 0.42f);
                    if (setDy <= mhalf || bagDy <= mhalf) {
                        menuBtnFinger_ = finger;
                        SDL_Log("SDL3_TOUCH stage=menu_claim finger=%llu",
                            (unsigned long long)finger);
                        return;
                    }
                }
            }
            // Action buttons claim their finger before the stick/look layers:
            // a press inside a button must never drag the camera.
            for (int i = 0; i < rhi::CameraInput::kActionButtonCount; ++i) {
                if (actionBtnFinger_[i] != static_cast<SDL_FingerID>(-1)) {
                    continue;
                }
                const float dx = x - actionBtnX_[i];
                const float dy = y - actionBtnY_[i];
                const float r = actionBtnRadius_;
                if (dx * dx + dy * dy <= r * r) {
                    actionBtnFinger_[i] = finger;
                    actionBtnHeld_[i] = true;
                    if (i == 2) {
                        sprintEnabled_ = !sprintEnabled_;
                    }
                    actionBtnPressedMask_ |= (1 << i);
                    TrackDown(finger, x, y, false);
                    return;
                }
            }
            const float dx = x - baseX_;
            const float dy = y - baseY_;
            const bool inJoystick =
                dx * dx + dy * dy <= baseRadius_ * kJoystickCaptureFactor *
                                         baseRadius_ * kJoystickCaptureFactor;
            if (inJoystick && !stickActive_) {
                // Engine behaviour: the stick jumps to the touchdown point and
                // is captured by that finger until it lifts.
                stickActive_ = true;
                stickFinger_ = finger;
                baseX_ = x;
                baseY_ = y;
                knobX_ = x;
                knobY_ = y;
                moveX_ = 0.0f;
                moveY_ = 0.0f;
                TrackDown(finger, x, y, false);
                return;
            }
            TrackDown(finger, x, y, true);
            // Outside joystick and buttons: camera-eligible.  It becomes the
            // look finger only when the (derived) pinch state says so; with
            // two eligible fingers already down the event only feeds the
            // pinch baseline.
            if (!UpdatePinchState()) {
                lookFinger_ = finger;
                lookLastX_ = x;
                lookLastY_ = y;
            }
            SDL_Log("SDL3_TOUCH down finger=%llu eligible=1 pinched=%d",
                (unsigned long long)finger, pinching_ ? 1 : 0);
            return;
        }

        if (event.type == SDL_EVENT_FINGER_MOTION) {
            if (finger == menuBtnFinger_) {
                // Held SET finger: no look, no stick, no zoom.
                return;
            }
            TrackMotion(finger, x, y);
            // Diagnostics: every 60th motion event, dump finger + tracked
            // table state so "raw batches" (SDL3_OHOS_TOUCH) can be compared
            // against what the app actually consumes.
            static int motionLogCounter = 0;
            if ((++motionLogCounter % 60) == 0) {
                int activeCount = 0;
                for (int i = 0; i < kTouchPointCount; ++i) {
                    if (tracked_[i].active) {
                        ++activeCount;
                    }
                }
                SDL_Log("SDL3_TOUCH motion#%d finger=%llu x=%.0f y=%.0f tracked=%d pinched=%d zoom=%.0f",
                    motionLogCounter, (unsigned long long)finger, x, y,
                    activeCount, pinching_ ? 1 : 0, zoomDelta_);
            }
            if (stickActive_ && finger == stickFinger_) {
                float dx = x - baseX_;
                float dy = y - baseY_;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > baseRadius_) {
                    dx = dx / dist * baseRadius_;
                    dy = dy / dist * baseRadius_;
                }
                knobX_ = baseX_ + dx;
                knobY_ = baseY_ + dy;
                moveX_ = dx / baseRadius_;
                moveY_ = dy / baseRadius_;
            }
            // While pinching, NO finger may accumulate look deltas: two
            // cameras fighting was the misjudged-rotation bug.  Zoom is
            // handled inside UpdatePinchState from the tracked positions.
            if (UpdatePinchState()) {
                return;
            }
            if (finger == lookFinger_) {
                float dx = x - lookLastX_;
                float dy = y - lookLastY_;
                dx = dx < -kMaxLookDeltaPerEvent ? -kMaxLookDeltaPerEvent
                                                 : (dx > kMaxLookDeltaPerEvent ? kMaxLookDeltaPerEvent : dx);
                dy = dy < -kMaxLookDeltaPerEvent ? -kMaxLookDeltaPerEvent
                                                 : (dy > kMaxLookDeltaPerEvent ? kMaxLookDeltaPerEvent : dy);
                lookDeltaX_ += dx;
                lookDeltaY_ += dy;
                lookLastX_ = x;
                lookLastY_ = y;
            }
            return;
        }

        if (isUp) {
            if (finger == menuBtnFinger_) {
                menuBtnFinger_ = static_cast<SDL_FingerID>(-1);
                // Report at the lift position (same convention as menus).
                tapPending_ = true;
                tapX_ = x;
                tapY_ = y;
                return;
            }
            for (int i = 0; i < rhi::CameraInput::kActionButtonCount; ++i) {
                if (actionBtnFinger_[i] == finger) {
                    actionBtnFinger_[i] = static_cast<SDL_FingerID>(-1);
                    actionBtnHeld_[i] = false;
                    TrackUp(finger);
                    UpdatePinchState();
                    return;
                }
            }
            if (stickActive_ && finger == stickFinger_) {
                stickActive_ = false;
                stickFinger_ = static_cast<SDL_FingerID>(-1);
                moveX_ = 0.0f;
                moveY_ = 0.0f;
                knobX_ = baseX_;
                knobY_ = baseY_;
            }
            const bool wasLook = lookFinger_ == finger;
            TrackUp(finger);
            // Recompute the derived state: leaving a pinch rebaselines look
            // onto the surviving finger; all fingers gone clears everything.
            UpdatePinchState();
            if (wasLook && lookFinger_ == finger) {
                // Only clear when no surviving finger was rebaselined onto
                // look (a duplicate UP for a removed finger must not kill
                // the rebaselined assignment).
                lookFinger_ = static_cast<SDL_FingerID>(-1);
            }
            return;
        }
        return;
    }

    // Mouse fallback for pointer-driven testing; dropped as soon as a real
    // finger has been seen (SDL synthesises mouse events from touch, and
    // counting both would double every drag).
    if (sawFinger_) {
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        mouseLooking_ = true;
        lookLastX_ = static_cast<float>(event.button.x);
        lookLastY_ = static_cast<float>(event.button.y);
    } else if (event.type == SDL_EVENT_MOUSE_MOTION && mouseLooking_) {
        lookDeltaX_ += static_cast<float>(event.motion.x) - lookLastX_;
        lookDeltaY_ += static_cast<float>(event.motion.y) - lookLastY_;
        lookLastX_ = static_cast<float>(event.motion.x);
        lookLastY_ = static_cast<float>(event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
               event.button.button == SDL_BUTTON_LEFT) {
        mouseLooking_ = false;
    }
}

rhi::CameraInput TouchController::FrameInput()
{
    rhi::CameraInput input{};
    // Menu/settings: gameplay channels are frozen (no stick deltas accumulated
    // anyway) and the HUD overlay is off; forward at most one queued tap.
    if (screen_ != rhi::UiScreen::Gameplay) {
        input.showOverlay = false;
        input.menuTapEdge = tapPending_;
        input.menuTapX = tapX_;
        input.menuTapY = tapY_;
        tapPending_ = false;
        return input;
    }
    input.lookDeltaX = lookDeltaX_;
    input.lookDeltaY = lookDeltaY_;
    input.zoomDelta = zoomDelta_;
    // Gameplay SET taps ride the same edge channel the menus use.
    input.menuTapEdge = tapPending_;
    input.menuTapX = tapX_;
    input.menuTapY = tapY_;
    tapPending_ = false;
    input.moveX = moveX_;
    input.moveY = moveY_;
    input.stickBaseX = baseX_;
    input.stickBaseY = baseY_;
    input.stickKnobX = knobX_;
    input.stickKnobY = knobY_;
    input.stickRadius = baseRadius_;
    input.knobRadius = knobRadius_;
    input.stickActive = stickActive_;
    input.showOverlay = true;
    for (int i = 0; i < rhi::CameraInput::kActionButtonCount; ++i) {
        input.actionButtonX[i] = actionBtnX_[i];
        input.actionButtonY[i] = actionBtnY_[i];
        // Both renderers use this level for speed, animation and highlighting.
        input.actionButtonHeld[i] = i == 2 ? sprintEnabled_ : actionBtnHeld_[i];
    }
    input.actionButtonRadius = actionBtnRadius_;
    // Edge-triggered presses queued since the last frame; the renderer
    // consumes them exactly once per DrawOnce.
    input.actionButtonPressedMask = actionBtnPressedMask_;
    actionBtnPressedMask_ = 0;
    lookDeltaX_ = 0.0f;
    lookDeltaY_ = 0.0f;
    zoomDelta_ = 0.0f;
    return input;
}
