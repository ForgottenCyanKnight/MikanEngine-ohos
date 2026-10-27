#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace animation {

enum class Transition { Locomotion, Crouch, Swim, Jump, Attack, Hit, Death, Weapon, Fire };

inline float Duration(Transition kind)
{
    switch (kind) {
    case Transition::Crouch: return .20f;
    case Transition::Swim: return .24f;
    case Transition::Jump: return .10f;
    case Transition::Attack: return .07f;
    case Transition::Hit: return .045f;
    case Transition::Death: return .12f;
    case Transition::Weapon: return .14f;
    case Transition::Fire: return .035f;
    default: return .18f;
    }
}

// Blend local TRS, before hierarchy composition. Matrix interpolation shears
// bones; quaternion shortest-arc interpolation preserves rigid rotations.
inline void Slerp(const float* from, const float* to, float weight, float* out)
{
    float a[4], b[4];
    const auto normalize = [](const float* q, float* result) {
        float length = 0;
        for (int i = 0; i < 4; ++i) length += q[i] * q[i];
        if (!(length > 1e-12f) || !std::isfinite(length)) {
            result[0] = result[1] = result[2] = 0; result[3] = 1;
        } else {
            const float inverse = 1 / std::sqrt(length);
            for (int i = 0; i < 4; ++i) result[i] = q[i] * inverse;
        }
    };
    normalize(from, a); normalize(to, b);
    float dot = 0;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    if (dot < 0) { for (float& v : b) v = -v; dot = -dot; }
    dot = std::clamp(dot, 0.0f, 1.0f);
    float sourceWeight = 1 - weight, targetWeight = weight;
    if (dot < .9995f) {
        const float angle = std::acos(dot), inverseSin = 1 / std::sin(angle);
        sourceWeight = std::sin((1 - weight) * angle) * inverseSin;
        targetWeight = std::sin(weight * angle) * inverseSin;
    }
    float mixed[4];
    for (int i = 0; i < 4; ++i) mixed[i] = a[i] * sourceWeight + b[i] * targetWeight;
    normalize(mixed, out);
}

class PoseTransition {
public:
    void Reset() { ready_ = false; blending_ = false; elapsed_ = 0; }
    float Weight() const { return weight_; }
    bool Active() const { return blending_; }

    // A new target snapshots the last displayed pose, including an interrupted
    // blend. Clips keep advancing; gameplay timers are never delayed by a fade.
    // An optional per-node mask lets weapon transitions leave the legs alone.
    bool Apply(int key, float dt, float duration, std::vector<float>& translation,
        std::vector<float>& rotation, std::vector<float>& scale,
        const std::vector<std::uint8_t>* mask = nullptr)
    {
        const size_t nodes = translation.size() / 3;
        if (translation.size() != nodes * 3 || rotation.size() != nodes * 4 ||
            scale.size() != nodes * 3 || (mask && mask->size() != nodes)) {
            Reset(); return false;
        }
        const bool changed = ready_ && key != key_ && previousT_.size() == translation.size();
        if (!ready_ || previousT_.size() != translation.size()) {
            blending_ = false;
        } else if (changed) {
            sourceT_ = previousT_; sourceR_ = previousR_; sourceS_ = previousS_;
            elapsed_ = 0;
            duration_ = std::isfinite(duration) ? std::max(0.0f, duration) : 0;
            blending_ = duration_ > 0;
        } else if (blending_ && std::isfinite(dt) && dt > 0) {
            elapsed_ = std::min(duration_, elapsed_ + dt);
        }
        weight_ = blending_ ? std::clamp(elapsed_ / duration_, 0.0f, 1.0f) : 1;
        const float smooth = weight_ * weight_ * (3 - 2 * weight_);
        if (blending_) {
            for (size_t node = 0; node < nodes; ++node) {
                if (mask && !(*mask)[node]) continue;
                for (size_t c = 0; c < 3; ++c) {
                    const size_t i = node * 3 + c;
                    translation[i] = sourceT_[i] + (translation[i] - sourceT_[i]) * smooth;
                    scale[i] = sourceS_[i] + (scale[i] - sourceS_[i]) * smooth;
                }
                Slerp(&sourceR_[node * 4], &rotation[node * 4], smooth, &rotation[node * 4]);
            }
            if (weight_ >= 1) blending_ = false;
        }
        previousT_ = translation; previousR_ = rotation; previousS_ = scale;
        key_ = key; ready_ = true;
        return changed;
    }

private:
    bool ready_ = false, blending_ = false;
    int key_ = 0;
    float elapsed_ = 0, duration_ = 0, weight_ = 1;
    std::vector<float> previousT_, previousR_, previousS_;
    std::vector<float> sourceT_, sourceR_, sourceS_;
};

} // namespace animation
