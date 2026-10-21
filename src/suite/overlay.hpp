#pragma once

#include <memory>
#include <string>

#include "core/spring.hpp"
#include "core/wayland.hpp"
#include "ui/ui.hpp"

namespace island {

// A full-screen layer surface with its own frame loop, fade in/out and Ui.
// The wallpaper picker and the capture editor are overlays.
class Overlay {
public:
    explicit Overlay(Wayland& wl) : wl_(wl) {}
    virtual ~Overlay() {
        if (fade_snap_) cairo_surface_destroy(fade_snap_);
    }

    void open();
    void close();
    // Gone right now, no fade (before a screenshot: it must not be in it).
    void destroy_now();
    bool is_open() const { return surf_ != nullptr && !closing_; }
    bool exists() const { return surf_ != nullptr; }
    // Called from the loop before polling: draws when something changed.
    void pump();
    void request() { dirty_ = true; }
    // Testing aid: a click at surface coordinates, through the Ui.
    void inject_click(float x, float y, Button b = Button::Left) {
        ui_.motion(x, y);
        if (ui_.button(b, true)) {
            ui_.button(b, false);
        } else {  // like a real click: what no widget takes goes to the canvas
            canvas_button(b, true, x, y);
            canvas_button(b, false, x, y);
        }
        request();
    }
    void inject_scroll(float x, float y, float steps) {
        ui_.motion(x, y);
        if (!ui_.scroll(steps)) canvas_scroll(steps);
        request();
    }
    // Something to draw and nothing holding it back: the loop must not sleep.
    bool wants_frame() const { return surf_ && dirty_ && surf_->configured() && !surf_->frame_pending(); }

protected:
    virtual void draw(cairo_t* cr, float w, float h, float dt) = 0;
    virtual void key(xkb_keysym_t, const std::string&, uint32_t) {}
    // Pointer events the Ui did not consume (canvas tools).
    virtual void canvas_button(Button, bool, float, float) {}
    virtual void canvas_motion(float, float) {}
    virtual void canvas_scroll(float) {}
    virtual void opened() {}
    virtual void closed() {}
    virtual bool animating() const { return false; }
    // Shown at full opacity straight away (its first frame matches the screen).
    virtual bool instant_open() const { return false; }
    virtual CursorShape canvas_cursor() const { return CursorShape::Default; }
    virtual uint32_t layer() const { return ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; }
    virtual const char* name() const { return "niri-island-overlay"; }
    // Lets a subclass shape its surface (anchors, size, keyboard…).
    virtual void adjust_spec(LayerSpec&) const {}
    // Fade-in/out alpha of the whole overlay, 0..1.
    float alpha() const { return clamp01(fade_.value); }
    float width() const { return w_; }
    float height() const { return h_; }
    int scale() const { return surf_ ? surf_->scale() : 1; }

    Wayland& wl_;
    Ui ui_;
    std::unique_ptr<LayerSurface> surf_;
    float px_ = -1, py_ = -1;
    bool pointer_in_ = false;
    bool ui_owns_press_ = false;

private:
    Spring fade_{0, 300, 32};
    cairo_surface_t* fade_snap_ = nullptr;  // the content while fading
    bool closing_ = false, dirty_ = false;
    double last_ = 0;
    float w_ = 0, h_ = 0;
    void render();
};

}  // namespace island
