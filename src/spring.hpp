#pragma once

#include <cmath>

namespace island {

struct Spring {
    float value = 0.0f;
    float target = 0.0f;
    float velocity = 0.0f;
    float stiffness = 280.0f; // Responsive tension
    float damping = 22.0f;    // Balanced bounce
    float epsilon = 0.3f;

    Spring(float initial = 0.0f, float k = 280.0f, float d = 22.0f)
        : value(initial), target(initial), stiffness(k), damping(d) {}

    void set_target(float t) {
        target = t;
    }

    void reset(float v) {
        value = v;
        target = v;
        velocity = 0.0f;
    }

    bool update(float dt) {
        if (is_settled()) {
            value = target;
            velocity = 0.0f;
            return false;
        }

        // Damped Harmonic Oscillator (Semi-implicit Euler for numerical stability)
        float force = -stiffness * (value - target) - damping * velocity;
        velocity += force * dt;
        value += velocity * dt;

        if (std::abs(value - target) < epsilon && std::abs(velocity) < epsilon * 2.0f) {
            value = target;
            velocity = 0.0f;
            return false; // Settled
        }

        return true; // Still animating
    }

    bool is_settled() const {
        return std::abs(value - target) < epsilon && std::abs(velocity) < epsilon * 2.0f;
    }
};

} // namespace island
