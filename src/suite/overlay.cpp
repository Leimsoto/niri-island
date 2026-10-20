#include "suite/overlay.hpp"

#include "core/loop.hpp"

namespace island {

void Overlay::open() {
    if (surf_ && !closing_) {
        request();
        return;
    }
    closing_ = false;
    if (fade_snap_) {  // reopened mid fade-out: that picture is stale
        cairo_surface_destroy(fade_snap_);
        fade_snap_ = nullptr;
    }
    if (!surf_) {
        LayerSpec spec;
        spec.layer = layer();
        spec.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        spec.exclusive_zone = -1;
        spec.keyboard = KeyboardMode::Exclusive;
        spec.input_everywhere = true;
        spec.name = name();
        adjust_spec(spec);
        surf_ = wl_.create_layer(spec);
        fade_.reset(0);
        LayerSurface* s = surf_.get();
        s->on_configure = [this] { request(); };
        s->on_closed = [this] { Loop::get().post([this] { surf_.reset(); }); };
        s->on_pointer_enter = [this](double x, double y) {
            pointer_in_ = true;
            px_ = static_cast<float>(x);
            py_ = static_cast<float>(y);
            ui_.motion(px_, py_);
            canvas_motion(px_, py_);
            request();
        };
        s->on_pointer_leave = [this] {
            pointer_in_ = false;
            ui_.leave();
            request();
        };
        s->on_pointer_motion = [this](double x, double y) {
            px_ = static_cast<float>(x);
            py_ = static_cast<float>(y);
            ui_.motion(px_, py_);
            if (!ui_owns_press_) canvas_motion(px_, py_);
            request();
        };
        s->on_button = [this](Button b, bool pressed) {
            if (pressed) {
                ui_owns_press_ = ui_.button(b, true);
                if (!ui_owns_press_) canvas_button(b, true, px_, py_);
            } else {
                if (ui_owns_press_) ui_.button(b, false);
                else canvas_button(b, false, px_, py_);
                ui_owns_press_ = false;
            }
            request();
        };
        s->on_scroll = [this](double, double dy, bool discrete) {
            float steps = discrete ? static_cast<float>(dy) : static_cast<float>(dy) / 12.0f;
            if (!ui_.scroll(steps)) canvas_scroll(steps);
            request();
        };
        s->on_key = [this](xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
            key(sym, utf8, mods);
            request();
        };
    }
    if (instant_open()) fade_.reset(1.0f);
    fade_.set(1.0f);
    opened();
    request();
}

void Overlay::close() {
    if (!surf_ || closing_) return;
    closing_ = true;
    if (fade_snap_) {  // the fade-out starts from what is on screen now
        cairo_surface_destroy(fade_snap_);
        fade_snap_ = nullptr;
    }
    fade_.set(0.0f);
    surf_->set_keyboard(KeyboardMode::None);
    surf_->set_input_rects({});
    request();
}

void Overlay::pump() {
    if (!surf_ || !dirty_ || !surf_->configured() || surf_->frame_pending()) return;
    render();
}

void Overlay::render() {
    double now = now_mono();
    float dt = last_ > 0 ? static_cast<float>(std::clamp(now - last_, 0.0, 0.05)) : 0.016f;
    last_ = now;
    w_ = static_cast<float>(surf_->width());
    h_ = static_cast<float>(surf_->height());
    ui_.begin_frame(dt);
    bool moving = fade_.update(dt);
    cairo_t* cr = surf_->begin_frame();
    if (!cr) {
        ui_.abort_frame();
        return;
    }
    dirty_ = false;
    float a = alpha();
    ui_.set_enabled(!closing_);
    if (a < 0.999f) {
        // Fading: the content is drawn once into a picture, then only that is
        // painted, with the opacity and a slight zoom. Redrawing everything
        // through a full-screen group each frame cost ~35 ms.
        if (!fade_snap_) {
            int sc = surf_->scale();
            fade_snap_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(w_) * sc, static_cast<int>(h_) * sc);
            cairo_surface_set_device_scale(fade_snap_, sc, sc);
            cairo_t* sc_cr = cairo_create(fade_snap_);
            draw(sc_cr, w_, h_, dt);
            cairo_destroy(sc_cr);
        } else {
            ui_.abort_frame();  // keep last frame's regions: nothing was drawn
        }
        // Unscaled: a plain copy with opacity (a full-screen scaled blit cost ~14 ms).
        cairo_set_source_surface(cr, fade_snap_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
        cairo_paint_with_alpha(cr, a);
    } else {
        if (fade_snap_) {
            cairo_surface_destroy(fade_snap_);
            fade_snap_ = nullptr;
        }
        draw(cr, w_, h_, dt);
    }
    surf_->end_frame({});
    static const bool timing = getenv("NIRI_ISLAND_OVLOG") != nullptr;
    if (timing) fprintf(stderr, "[ov %s] %.1f ms\n", name(), (now_mono() - now) * 1000);
    CursorShape c = ui_.cursor();
    if (c == CursorShape::Default && !ui_owns_press_) c = canvas_cursor();
    surf_->set_cursor(c);
    if (closing_ && !moving && a <= 0.01f) {
        // Faded out: drop the surface outside of this call stack.
        Loop::get().post([this] {
            if (closing_) {
                if (fade_snap_) {
                    cairo_surface_destroy(fade_snap_);
                    fade_snap_ = nullptr;
                }
                surf_.reset();
                closing_ = false;
                last_ = 0;
                closed();
            }
        });
        return;
    }
    if (moving || ui_.animating() || ui_.dragging() || animating()) dirty_ = true;
}

}  // namespace island
