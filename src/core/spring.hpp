#pragma once

#include <algorithm>
#include <cmath>

namespace island {

// Damped harmonic oscillator, integrated in fixed sub-steps so the motion is
// identical whatever the frame rate. `speed` (from the config) scales time.
struct Spring {
    float value = 0.0f;
    float target = 0.0f;
    float velocity = 0.0f;
    float stiffness = 280.0f;
    float damping = 24.0f;
    float epsilon = 0.004f;

    static inline float speed = 1.0f;

    Spring(float initial = 0.0f, float k = 280.0f, float d = 24.0f, float eps = 0.004f)
        : value(initial), target(initial), stiffness(k), damping(d), epsilon(eps) {}

    void set(float t) { target = t; }
    void reset(float v) {
        value = target = v;
        velocity = 0.0f;
    }
    // A tap: jump to `v` and spring back to the current target.
    void kick(float v) {
        value = v;
        velocity = 0.0f;
    }
    void impulse(float v) { velocity += v; }

    bool settled() const { return std::abs(value - target) < epsilon && std::abs(velocity) < epsilon * 4.0f; }

    // Returns true while still moving.
    bool update(float dt) {
        if (speed <= 0.0f) {
            reset(target);
            return false;
        }
        if (settled()) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        dt = std::min(dt, 0.064f) * speed;
        const float step = 1.0f / 240.0f;
        while (dt > 0.0f) {
            float h = std::min(step, dt);
            float force = -stiffness * (value - target) - damping * velocity;
            velocity += force * h;
            value += velocity * h;
            dt -= h;
        }
        if (settled()) {
            value = target;
            velocity = 0.0f;
            return false;
        }
        return true;
    }
};

}  // namespace island
