#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Touchscreen gesture classifier for the desktop hosts (standalone GLFW, Unreal overlay). Pure and
// ImGui-free so the whole tap / long-press / pan / fling contract is unit-tested
// (tests/Core/TouchGesturePure.test.cpp); the ImGui glue lives in Ui/TouchInputRouter.cpp.
//
// The machine HOLDS BACK a finger press until it knows what the finger is doing, then emits the
// mouse events ImGui should see:
//   - tap                -> LeftDown + LeftUp at the press point (pointer then stays parked there,
//                           so hover tooltips appear without a mouse)
//   - hold in place      -> RightDown + RightUp (context menu); left is never sent
//   - quick second tap   -> forwarded at the first tap's point so ImGui sees a real double-click
//   - drag (pannable)    -> PanBy deltas, then an exponential-decay fling after a fast release
//   - drag (widget)      -> LeftDown at the press point, then the moves (scrollbars, splitters, DnD)
// Whether a drag pans or goes through to a widget depends on what is under the finger, which only
// the glue knows: Tick() takes a classifier callback that answers it.
//
// Lives at the include/ root beside TouchCellEditGesture.h, which shares kDefaultLongPressSeconds.
//
// Timing: the caller passes ImGui time (an accumulation of io.DeltaTime) to Tick(), so the machine
// is deterministic under the test engine's fast run speed. Raw events are buffered by the On*()
// calls and processed at the next Tick(), stamped with that tick's time.
//
// Emitted button events are not paced here: ImGui's trickle queue (on by default) already spreads a
// down+up pair over two frames and delays a touch-sourced press until after its position is seen.
namespace SmatchetTouchGesturePure {

// Hold time that turns a stationary press into a long-press. Shared with the Android cell-editor
// long-press (TouchCellEditGesture.h) so the two touch paths agree.
constexpr float kDefaultLongPressSeconds = 0.5f;
constexpr float kMinLongPressSeconds = 0.3f;
constexpr float kMaxLongPressSeconds = 1.5f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vec2 MakeVec2(float x, float y) {
    Vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

inline float Length(Vec2 v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

inline Vec2 Sub(Vec2 a, Vec2 b) {
    return MakeVec2(a.x - b.x, a.y - b.y);
}

// Distances are logical pixels and speeds logical px/s; ScaledBy() converts both to physical
// pixels for a high-DPI host.
struct GestureConfig {
    float tapSlopPx = 10.0f;            // movement under this is still a tap / hold
    float longPressSeconds = kDefaultLongPressSeconds;
    float feedbackDelaySeconds = 0.15f; // the progress ring appears after this much hold
    float doubleTapSeconds = 0.30f;     // matches ImGui's default io.MouseDoubleClickTime
    float doubleTapRadiusPx = 24.0f;
    float velocityWindowSeconds = 0.10f;
    float flingMinSpeed = 120.0f;
    float flingMaxSpeed = 6000.0f;
    float flingTimeConstant = 0.325f; // seconds; velocity decays by e^(-dt/tau)
    float flingStopSpeed = 15.0f;
    float axisLockRatio = 2.0f; // a pan this much more along one axis than the other locks to it
};

inline float ClampLongPressSeconds(float seconds) {
    if (!(seconds == seconds)) { // NaN from a hand-edited config
        return kDefaultLongPressSeconds;
    }
    return std::min(kMaxLongPressSeconds, std::max(kMinLongPressSeconds, seconds));
}

inline GestureConfig ScaledBy(const GestureConfig& base, float pixelScale) {
    const float s = (pixelScale > 0.0f) ? pixelScale : 1.0f;
    GestureConfig c = base;
    c.tapSlopPx *= s;
    c.doubleTapRadiusPx *= s;
    c.flingMinSpeed *= s;
    c.flingMaxSpeed *= s;
    c.flingStopSpeed *= s;
    return c;
}

// What the glue found under the press point when the finger moved past the tap slop.
enum class PressClass {
    Pannable,    // scrollable content: scroll it, never send the press
    DragThrough, // a widget that wants a drag (scrollbar, splitter, DnD source...): send the press
    Blocked,     // outside an open modal / popup: do nothing
};

enum class GestureState { Idle, Pending, LongPressFired, DragThrough, TapThrough, Pan, Fling, Blocked };

enum class ActionType {
    MovePointer, // pos
    HidePointer, // move the pointer off-screen (no hover while panning)
    LeftDown,    // pos (pointer is already there)
    LeftUp,
    RightDown, // pos
    RightUp,
    PanBy,  // delta = finger movement; content follows the finger (scroll -= delta)
    PanEnd, // pan / fling finished; the glue drops its scroll targets
};

struct Action {
    ActionType type = ActionType::MovePointer;
    Vec2 pos;
    Vec2 delta;
};

// Least-squares velocity over the samples inside the trailing window. A finger that rested
// longer than the window before lifting reads as zero, so a "place, wait, lift" never flings.
class VelocityTracker {
  public:
    void Reset() { count_ = 0; }

    void AddSample(double t, Vec2 p) {
        if (count_ == kCapacity) {
            for (std::size_t i = 1; i < kCapacity; ++i) {
                samples_[i - 1] = samples_[i];
            }
            --count_;
        }
        samples_[count_].t = t;
        samples_[count_].p = p;
        ++count_;
    }

    Vec2 Estimate(double now, float windowSeconds) const {
        double st = 0.0, sx = 0.0, sy = 0.0, stt = 0.0, stx = 0.0, sty = 0.0;
        int n = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            const double t = samples_[i].t - now; // relative time keeps the sums well-conditioned
            if (t < -static_cast<double>(windowSeconds) - 1e-9) {
                continue;
            }
            st += t;
            sx += samples_[i].p.x;
            sy += samples_[i].p.y;
            stt += t * t;
            stx += t * samples_[i].p.x;
            sty += t * samples_[i].p.y;
            ++n;
        }
        if (n < 2) {
            return Vec2();
        }
        const double denom = n * stt - st * st;
        if (denom <= 1e-12) {
            return Vec2();
        }
        return MakeVec2(static_cast<float>((n * stx - st * sx) / denom), static_cast<float>((n * sty - st * sy) / denom));
    }

  private:
    struct Sample {
        double t = 0.0;
        Vec2 p;
    };
    static constexpr std::size_t kCapacity = 16;
    Sample samples_[kCapacity];
    std::size_t count_ = 0;
};

// One step of exponential fling decay: the distance travelled over dt and the new velocity.
// Integrates v(t) = v0 * e^(-t/tau) exactly, so the result does not depend on the frame rate.
inline Vec2 FlingStep(Vec2& velocity, float dt, float tau) {
    if (dt <= 0.0f || tau <= 0.0f) {
        return Vec2();
    }
    const float decay = std::exp(-dt / tau);
    const float travel = tau * (1.0f - decay);
    const Vec2 disp = MakeVec2(velocity.x * travel, velocity.y * travel);
    velocity.x *= decay;
    velocity.y *= decay;
    return disp;
}

enum class AxisLock { None, X, Y };

class GestureMachine {
  public:
    void SetConfig(const GestureConfig& config) { config_ = config; }
    const GestureConfig& Config() const { return config_; }

    // Raw finger input (first finger only). Buffered; processed at the next Tick().
    void OnMove(Vec2 p) { PushRaw(RawType::Move, p); }
    void OnDown(Vec2 p) { PushRaw(RawType::Down, p); }
    void OnUp(Vec2 p) { PushRaw(RawType::Up, p); }

    // The glue saw the scroll target stop moving on an axis (edge reached / window gone). A fling
    // drops that axis; a finger-driven pan ignores it.
    void OnPanAxisBlocked(bool xAxis) {
        if (state_ != GestureState::Fling) {
            return;
        }
        if (xAxis) {
            velocity_.x = 0.0f;
        } else {
            velocity_.y = 0.0f;
        }
    }

    // Abort whatever is in flight (mouse took over, mode switched off, focus lost). Releases a press
    // that was already sent so ImGui is never left with the left button held.
    void Cancel(std::vector<Action>& out) {
        raw_.clear();
        if (state_ == GestureState::DragThrough || state_ == GestureState::TapThrough) {
            Emit(out, ActionType::LeftUp, finger_);
        } else if (state_ == GestureState::Pan || state_ == GestureState::Fling) {
            Emit(out, ActionType::PanEnd, finger_);
        }
        state_ = GestureState::Idle;
        absorbTap_ = false;
    }

    // Advance to ImGui time `now`: process buffered raw events, then time-driven transitions
    // (long-press, pan delta, fling). `classify(pressPos)` returns a PressClass; it is called at
    // most once per press, never in the same tick as the press itself (so the glue's hover state
    // already reflects the press point).
    template <class Classify>
    void Tick(double now, Classify&& classify, std::vector<Action>& out) {
        const double prevTick = lastTick_;
        lastTick_ = now;
        std::size_t i = 0;
        for (; i < raw_.size(); ++i) {
            if (!ProcessRaw(raw_[i], now, classify, out)) {
                break; // classification deferred to the next tick; keep the rest
            }
        }
        raw_.erase(raw_.begin(), raw_.begin() + static_cast<std::ptrdiff_t>(i));

        switch (state_) {
        case GestureState::Pending:
            if (now - pressTime_ >= config_.longPressSeconds) {
                Emit(out, ActionType::RightDown, pressPos_);
                Emit(out, ActionType::RightUp, pressPos_);
                state_ = GestureState::LongPressFired;
            }
            break;
        case GestureState::Pan:
            EmitPanDelta(out);
            velocity_tracker_.AddSample(now, finger_);
            break;
        case GestureState::Fling: {
            Vec2 disp = FlingStep(velocity_, static_cast<float>(now - prevTick), config_.flingTimeConstant);
            if (Length(velocity_) < config_.flingStopSpeed) {
                Emit(out, ActionType::PanEnd, finger_);
                state_ = GestureState::Idle;
            } else if (disp.x != 0.0f || disp.y != 0.0f) {
                Action a;
                a.type = ActionType::PanBy;
                a.delta = disp;
                out.push_back(a);
            }
            break;
        }
        default:
            break;
        }
    }

    GestureState State() const { return state_; }
    bool IsIdle() const { return state_ == GestureState::Idle && raw_.empty(); }
    Vec2 PressPos() const { return pressPos_; }
    Vec2 FlingVelocity() const { return velocity_; }

    // 0 until the feedback delay, then 0..1 across the remaining hold; 0 when not holding.
    float LongPressProgress(double now) const {
        if (state_ != GestureState::Pending) {
            return 0.0f;
        }
        const double held = now - pressTime_ - config_.feedbackDelaySeconds;
        const double span = config_.longPressSeconds - config_.feedbackDelaySeconds;
        if (held <= 0.0 || span <= 0.0) {
            return 0.0f;
        }
        return static_cast<float>(std::min(1.0, held / span));
    }

  private:
    enum class RawType { Move, Down, Up };
    struct Raw {
        RawType type;
        Vec2 pos;
    };

    void PushRaw(RawType type, Vec2 p) {
        Raw r;
        r.type = type;
        r.pos = p;
        raw_.push_back(r);
    }

    static void Emit(std::vector<Action>& out, ActionType type, Vec2 pos) {
        Action a;
        a.type = type;
        a.pos = pos;
        out.push_back(a);
    }

    void EmitPanDelta(std::vector<Action>& out) {
        Vec2 d = Sub(finger_, panLast_);
        panLast_ = finger_;
        if (axisLock_ == AxisLock::X) {
            d.y = 0.0f;
        } else if (axisLock_ == AxisLock::Y) {
            d.x = 0.0f;
        }
        if (d.x == 0.0f && d.y == 0.0f) {
            return;
        }
        Action a;
        a.type = ActionType::PanBy;
        a.delta = d;
        out.push_back(a);
    }

    AxisLock ChooseAxisLock(Vec2 travel) const {
        const float ax = std::fabs(travel.x);
        const float ay = std::fabs(travel.y);
        if (ax > config_.axisLockRatio * ay) {
            return AxisLock::X;
        }
        if (ay > config_.axisLockRatio * ax) {
            return AxisLock::Y;
        }
        return AxisLock::None;
    }

    // Returns false when the event must wait for the next tick (classification deferred).
    template <class Classify>
    bool ProcessRaw(const Raw& r, double now, Classify& classify, std::vector<Action>& out) {
        switch (r.type) {
        case RawType::Move:
            return ProcessMove(r.pos, now, classify, out);
        case RawType::Down:
            ProcessDown(r.pos, now, out);
            return true;
        case RawType::Up:
            ProcessUp(r.pos, now, out);
            return true;
        }
        return true;
    }

    template <class Classify>
    bool ProcessMove(Vec2 p, double now, Classify& classify, std::vector<Action>& out) {
        switch (state_) {
        case GestureState::Idle:
            finger_ = p;
            Emit(out, ActionType::MovePointer, p); // touch hover before a press (Windows sends one)
            return true;
        case GestureState::Pending: {
            if (Length(Sub(p, pressPos_)) <= config_.tapSlopPx) {
                finger_ = p;
                return true; // pointer stays frozen at the press point
            }
            if (now <= pressTime_) {
                return false; // the glue's hover state does not reflect the press yet
            }
            finger_ = p;
            const PressClass cls = classify(pressPos_);
            if (cls == PressClass::Pannable) {
                state_ = GestureState::Pan;
                absorbTap_ = false;
                panLast_ = pressPos_;
                axisLock_ = ChooseAxisLock(Sub(p, pressPos_));
                velocity_tracker_.Reset();
                velocity_tracker_.AddSample(pressTime_, pressPos_); // the end-of-tick sample adds (now, p)
                Emit(out, ActionType::HidePointer, p);
            } else if (cls == PressClass::DragThrough) {
                state_ = GestureState::DragThrough;
                Emit(out, ActionType::LeftDown, pressPos_);
                Emit(out, ActionType::MovePointer, p);
            } else {
                state_ = GestureState::Blocked;
            }
            return true;
        }
        case GestureState::DragThrough:
            finger_ = p;
            Emit(out, ActionType::MovePointer, p);
            return true;
        case GestureState::Pan:
            finger_ = p;
            return true;
        default:
            return true; // LongPressFired / TapThrough / Blocked / Fling ignore movement
        }
    }

    void ProcessDown(Vec2 p, double now, std::vector<Action>& out) {
        if (state_ != GestureState::Idle && state_ != GestureState::Fling) {
            return; // duplicate down while a press is already tracked
        }
        const bool catchingFling = (state_ == GestureState::Fling);
        if (catchingFling) {
            Emit(out, ActionType::PanEnd, finger_);
            velocity_ = Vec2();
        }
        finger_ = p;
        if (!catchingFling && hasLastTap_ && now - lastTapTime_ <= config_.doubleTapSeconds &&
            Length(Sub(p, lastTapPos_)) <= config_.doubleTapRadiusPx) {
            hasLastTap_ = false;
            state_ = GestureState::TapThrough;
            Emit(out, ActionType::MovePointer, lastTapPos_);
            Emit(out, ActionType::LeftDown, lastTapPos_);
            return;
        }
        state_ = GestureState::Pending;
        pressPos_ = p;
        pressTime_ = now;
        absorbTap_ = catchingFling;
        Emit(out, ActionType::MovePointer, p);
    }

    void ProcessUp(Vec2 p, double now, std::vector<Action>& out) {
        switch (state_) {
        case GestureState::Pending:
            if (!absorbTap_) {
                Emit(out, ActionType::LeftDown, pressPos_);
                Emit(out, ActionType::LeftUp, pressPos_);
                hasLastTap_ = true;
                lastTapTime_ = now;
                lastTapPos_ = pressPos_;
            }
            absorbTap_ = false;
            state_ = GestureState::Idle;
            break;
        case GestureState::DragThrough:
            finger_ = p;
            Emit(out, ActionType::MovePointer, p);
            Emit(out, ActionType::LeftUp, p);
            state_ = GestureState::Idle;
            break;
        case GestureState::TapThrough:
            Emit(out, ActionType::LeftUp, finger_);
            state_ = GestureState::Idle;
            break;
        case GestureState::Pan: {
            finger_ = p;
            EmitPanDelta(out);
            velocity_tracker_.AddSample(now, p);
            Vec2 v = velocity_tracker_.Estimate(now, config_.velocityWindowSeconds);
            if (axisLock_ == AxisLock::X) {
                v.y = 0.0f;
            } else if (axisLock_ == AxisLock::Y) {
                v.x = 0.0f;
            }
            const float speed = Length(v);
            if (speed >= config_.flingMinSpeed) {
                if (speed > config_.flingMaxSpeed) {
                    const float k = config_.flingMaxSpeed / speed;
                    v.x *= k;
                    v.y *= k;
                }
                velocity_ = v;
                state_ = GestureState::Fling;
            } else {
                Emit(out, ActionType::PanEnd, p);
                state_ = GestureState::Idle;
            }
            break;
        }
        default:
            state_ = GestureState::Idle; // LongPressFired / Blocked: the release does nothing
            break;
        }
    }

    GestureConfig config_;
    GestureState state_ = GestureState::Idle;
    std::vector<Raw> raw_;
    double lastTick_ = 0.0;
    double pressTime_ = 0.0;
    Vec2 pressPos_;
    Vec2 finger_;
    Vec2 panLast_;
    AxisLock axisLock_ = AxisLock::None;
    VelocityTracker velocity_tracker_;
    Vec2 velocity_;
    bool absorbTap_ = false;
    bool hasLastTap_ = false;
    double lastTapTime_ = 0.0;
    Vec2 lastTapPos_;
};

} // namespace SmatchetTouchGesturePure
