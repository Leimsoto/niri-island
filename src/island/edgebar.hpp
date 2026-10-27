#pragma once

#include <functional>
#include <vector>

#include "suite/overlay.hpp"

namespace island {

// A strip anchored to the top-left or top-right corner, for the "spread"
// bar layout: the island owns the middle, these hold the side modules.
class EdgeBar : public Overlay {
public:
    EdgeBar(Wayland& wl, bool left) : Overlay(wl), left_(left) {}

    static constexpr int kWidth = 900, kHeight = 64;  // room for the tallest pills and their shadow

    // Draws the modules and reports where they are (the input region).
    std::function<void(cairo_t*, Ui&, float w, float h, bool left, std::vector<Rect>& input)> painter;
    std::function<bool()> moving;
    bool blur_off = false;
    void restyle();
    static int edge_inset();  // how far the pills sit from the screen edge

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override;
    bool animating() const override { return moving && moving(); }
    uint32_t layer() const override { return ZWLR_LAYER_SHELL_V1_LAYER_TOP; }
    const char* name() const override { return left_ ? "niri-island-bar-left" : "niri-island-bar-right"; }
    void adjust_spec(LayerSpec& s) const override;
    // Reopened after a close: the surface lost its input region, so the cache must go too.
    void opened() override {
        input_.clear();
        request();
    }

private:
    bool left_;
    std::vector<Rect> input_;
};

}  // namespace island
