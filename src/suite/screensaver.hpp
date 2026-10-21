#pragma once

#include <functional>
#include <map>
#include <vector>

#include "suite/overlay.hpp"

namespace island {

// "Bubbles", after the Windows Vista screensaver: soap bubbles drifting over
// the desktop, bouncing off each other and the edges, their rims slowly
// changing colour. Any key, click or real pointer movement ends it.
class Screensaver : public Overlay {
public:
    explicit Screensaver(Wayland& wl) : Overlay(wl) {}
    ~Screensaver() override;
    void start();
    void stop() {
        if (!is_open()) return;
        close();
        if (on_stop) on_stop();  // the others (one per screen) go too
    }
    wl_output* output = nullptr;      // which screen (none: the compositor picks)
    std::function<void()> on_stop;

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override;
    void key(xkb_keysym_t, const std::string&, uint32_t) override { stop(); }
    void canvas_button(Button, bool pressed, float, float) override {
        if (pressed) stop();
    }
    void canvas_motion(float x, float y) override;
    void canvas_scroll(float) override { stop(); }
    void opened() override;
    bool animating() const override { return true; }
    const char* name() const override { return "niri-island-screensaver"; }
    void adjust_spec(LayerSpec& s) const override { s.output = output; }

private:
    struct Bubble {
        float x, y, vx, vy, r;
        float hue, hue_speed;
        float born;
        float cruise;        // speed it settles at after the puff
        bool ghost = false;  // now and then one passes through the others for a moment
        float clear_t = 0;
        float ghost_until = 4;  // solid by then anyway
        float turn = 0;         // rad/s its path bends once out of the line
    };
    std::vector<Bubble> bubbles_;
    std::map<int, cairo_surface_t*> sprites_;  // per hue step and size
    float w_ = 0, h_ = 0, t_ = 0;
    float px0_ = -1, py0_ = -1;
    double opened_at_ = 0;
    float next_ghost_ = 8;
    void spawn(float w, float h);
    cairo_surface_t* sprite(float r, float hue);
};

}  // namespace island
