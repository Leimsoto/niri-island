#pragma once

#include <functional>
#include <vector>

#include "suite/overlay.hpp"

namespace island {

// A full-screen overlay whose content the app paints (the settings centre):
// drawing, keys and clicks outside any widget are forwarded.
class HostedOverlay : public Overlay {
public:
    HostedOverlay(Wayland& wl, const char* name) : Overlay(wl), name_(name) {}

    std::function<void(cairo_t*, Ui&, float w, float h, float dt)> painter;
    std::function<void(xkb_keysym_t, const std::string&, uint32_t)> on_key;
    std::function<void(float x, float y)> on_backdrop_click;
    std::function<bool()> moving;
    std::function<void()> on_closed;
    float fade() const { return alpha(); }
    void blur(const std::vector<LayerSurface::BlurShape>& shapes) {
        if (surf_) surf_->set_blur(shapes);
    }
    // Closing: no blur behind the fade-out (it lingered until the surface went).
    void close_now() {
        blur({});
        if (surf_) surf_->commit();
        close();
    }

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override {
        if (painter) painter(cr, ui_, w, h, dt);
    }
    void key(xkb_keysym_t s, const std::string& u, uint32_t m) override {
        if (on_key) on_key(s, u, m);
    }
    void canvas_button(Button b, bool pressed, float x, float y) override {
        if (b == Button::Left && pressed && on_backdrop_click) on_backdrop_click(x, y);
    }
    bool animating() const override { return moving && moving(); }
    void closed() override {
        if (on_closed) on_closed();
    }
    const char* name() const override { return name_; }

private:
    const char* name_;
};

}  // namespace island
