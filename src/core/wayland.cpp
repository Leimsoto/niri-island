#include "core/wayland.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "core/loop.hpp"

namespace island {

static int create_shm_fd(size_t size) {
    int fd = memfd_create("niri-island", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── surface listeners ──────────────────────────────────────────────────────

static void buffer_release(void* data, wl_buffer*) { static_cast<LayerSurface::Buffer*>(data)->busy = false; }
static const wl_buffer_listener buffer_listener = {.release = buffer_release};

static void frame_done(void* data, wl_callback* cb, uint32_t) {
    auto* self = static_cast<LayerSurface*>(data);
    wl_callback_destroy(cb);
    self->frame_cb_ = nullptr;
    if (self->on_frame) self->on_frame();
}
static const wl_callback_listener frame_listener = {.done = frame_done};

static void surface_enter(void*, wl_surface*, wl_output*) {}
static void surface_leave(void*, wl_surface*, wl_output*) {}
static void surface_preferred_scale(void* data, wl_surface*, int32_t factor) {
    auto* self = static_cast<LayerSurface*>(data);
    factor = std::clamp(factor, 1, 4);
    if (factor == self->scale_) return;
    self->scale_ = factor;
    self->destroy_buffers();
    if (self->surface_) wl_surface_set_buffer_scale(self->surface_, factor);
    if (self->on_configure) self->on_configure();
}
static void surface_preferred_transform(void*, wl_surface*, uint32_t) {}
static const wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = surface_preferred_transform,
};

static void layer_configure(void* data, zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t w, uint32_t h) {
    auto* self = static_cast<LayerSurface*>(data);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (w > 0) self->width_ = static_cast<int>(w);
    if (h > 0) self->height_ = static_cast<int>(h);
    self->configured_ = true;
    if (self->on_configure) self->on_configure();
}
static void layer_closed(void* data, zwlr_layer_surface_v1*) {
    auto* self = static_cast<LayerSurface*>(data);
    // Usually the output went away. Handle it outside of dispatch.
    Loop::get().post([self] {
        if (self->on_closed) self->on_closed();
    });
}
static const zwlr_layer_surface_v1_listener layer_listener = {.configure = layer_configure, .closed = layer_closed};

static void lock_configure(void* data, ext_session_lock_surface_v1* surface, uint32_t serial, uint32_t w, uint32_t h) {
    auto* self = static_cast<LayerSurface*>(data);
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    if (w > 0) self->width_ = static_cast<int>(w);
    if (h > 0) self->height_ = static_cast<int>(h);
    self->configured_ = true;
    if (self->on_configure) self->on_configure();
}
static const ext_session_lock_surface_v1_listener lock_listener = {.configure = lock_configure};

// ── LayerSurface ───────────────────────────────────────────────────────────

LayerSurface::~LayerSurface() {
    if (wl_) wl_->forget(this);
    if (frame_cb_) wl_callback_destroy(frame_cb_);
    destroy_buffers();
    if (blur_) ext_background_effect_surface_v1_destroy(blur_);
    if (lock_surf_) ext_session_lock_surface_v1_destroy(lock_surf_);
    if (layer_) zwlr_layer_surface_v1_destroy(layer_);
    if (surface_) wl_surface_destroy(surface_);
    if (wl_ && wl_->display_) wl_display_flush(wl_->display_);
}

void LayerSurface::destroy_buffers() {
    for (auto& b : buffers_) {
        if (b.cr) cairo_destroy(b.cr);
        if (b.surface) cairo_surface_destroy(b.surface);
        if (b.data) munmap(b.data, b.size);
        if (b.wl) wl_buffer_destroy(b.wl);
        b = Buffer{};
    }
    buf_w_ = buf_h_ = 0;
    current_ = -1;
}

bool LayerSurface::create_buffers() {
    int pw = width_ * scale_, ph = height_ * scale_;
    if (pw <= 0 || ph <= 0) return false;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    size_t one = static_cast<size_t>(stride) * ph;
    for (auto& b : buffers_) {
        int fd = create_shm_fd(one);
        if (fd < 0) return false;
        void* data = mmap(nullptr, one, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return false;
        }
        wl_shm_pool* pool = wl_shm_create_pool(wl_->shm_, fd, static_cast<int32_t>(one));
        b.wl = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_buffer_add_listener(b.wl, &buffer_listener, &b);
        b.data = data;
        b.size = one;
        b.surface =
            cairo_image_surface_create_for_data(static_cast<unsigned char*>(data), CAIRO_FORMAT_ARGB32, pw, ph, stride);
        b.cr = cairo_create(b.surface);
    }
    buf_w_ = pw;
    buf_h_ = ph;
    return true;
}

cairo_t* LayerSurface::begin_frame(const Rect& area) {
    if (!surface_ || !configured_) return nullptr;
    if (buf_w_ != width_ * scale_ || buf_h_ != height_ * scale_) {
        bool any_busy = false;
        for (auto& b : buffers_) any_busy |= b.busy;
        if (any_busy && buf_w_ > 0) return nullptr;  // resize once the compositor lets go
        destroy_buffers();
        if (!create_buffers()) return nullptr;
    }
    current_ = -1;
    for (int i = 0; i < 3; ++i)
        if (!buffers_[i].busy) {
            current_ = i;
            break;
        }
    if (current_ < 0) return nullptr;
    Buffer& b = buffers_[current_];
    cairo_t* cr = b.cr;
    cairo_identity_matrix(cr);
    cairo_reset_clip(cr);
    cairo_scale(cr, scale_, scale_);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    // Only what this buffer drew last time (and what is about to be drawn)
    // needs clearing; the rest is already transparent.
    if (b.full || area.empty()) {
        cairo_paint(cr);
        b.full = area.empty();
    } else {
        Rect c = b.drawn.unite(area).inset(-2);
        cairo_rectangle(cr, std::floor(c.x), std::floor(c.y), std::ceil(c.w) + 1, std::ceil(c.h) + 1);
        cairo_fill(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    b.drawn = area;
    return cr;
}

void LayerSurface::end_frame(const Rect& damage) {
    if (current_ < 0) return;
    Buffer& b = buffers_[current_];
    cairo_surface_flush(b.surface);
    wl_surface_attach(surface_, b.wl, 0, 0);
    if (damage.empty()) {
        wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        int x = static_cast<int>(std::floor(damage.x)) - 2, y = static_cast<int>(std::floor(damage.y)) - 2;
        int w = static_cast<int>(std::ceil(damage.w)) + 4, h = static_cast<int>(std::ceil(damage.h)) + 4;
        wl_surface_damage_buffer(surface_, x * scale_, y * scale_, w * scale_, h * scale_);
    }
    frame_cb_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_cb_, &frame_listener, this);
    wl_surface_commit(surface_);
    b.busy = true;
    current_ = -1;
}

void LayerSurface::set_input_rects(const std::vector<Rect>& rects) {
    if (!surface_) return;
    wl_region* region = wl_compositor_create_region(wl_->compositor_);
    for (const auto& r : rects) {
        if (r.empty()) continue;
        wl_region_add(region, static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)),
                      static_cast<int>(std::ceil(r.w)), static_cast<int>(std::ceil(r.h)));
    }
    wl_surface_set_input_region(surface_, region);
    wl_region_destroy(region);
}

void LayerSurface::set_keyboard(KeyboardMode mode) {
    if (!layer_ || mode == kb_mode_) return;
    if (mode == KeyboardMode::OnDemand && wl_->layer_shell_version_ < 4) mode = KeyboardMode::Exclusive;
    kb_mode_ = mode;
    uint32_t v = mode == KeyboardMode::Exclusive ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
               : mode == KeyboardMode::OnDemand  ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
                                                 : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_, v);
    if (mode == KeyboardMode::None && wl_->keyboard_focus_ == this) wl_->stop_repeat();
    wl_surface_commit(surface_);
}

void LayerSurface::set_cursor(CursorShape shape) {
    if (shape == cursor_) return;
    cursor_ = shape;
    if (wl_->pointer_focus_ == this) wl_->apply_cursor();
}

void LayerSurface::set_margin(int top, int side) {
    spec_.margin_top = top;
    if (side >= 0) spec_.margin_side = side;
    if (!layer_) return;
    zwlr_layer_surface_v1_set_margin(layer_, top, spec_.margin_side, 0, spec_.margin_side);
    wl_surface_commit(surface_);
}

void LayerSurface::set_blur(const std::vector<BlurShape>& shapes) {
    if (!wl_ || !wl_->bg_effect_ || !surface_) return;
    // Only touch the region when it really moved (it rides on the next commit).
    bool same = shapes.size() == blur_shapes_.size();
    for (size_t i = 0; same && i < shapes.size(); ++i) {
        const auto &a = shapes[i].r, &b = blur_shapes_[i].r;
        same = std::abs(a.x - b.x) < 0.5f && std::abs(a.y - b.y) < 0.5f && std::abs(a.w - b.w) < 0.5f &&
               std::abs(a.h - b.h) < 0.5f && std::abs(shapes[i].radius - blur_shapes_[i].radius) < 0.5f &&
               shapes[i].square_top == blur_shapes_[i].square_top;
    }
    if (same) return;
    blur_shapes_ = shapes;
    if (!blur_) blur_ = ext_background_effect_manager_v1_get_background_effect(wl_->bg_effect_, surface_);
    if (shapes.empty()) {
        ext_background_effect_surface_v1_set_blur_region(blur_, nullptr);
        return;
    }
    wl_region* reg = wl_compositor_create_region(wl_->compositor_);
    for (const auto& sh : shapes) {
        Rect r = sh.r;
        float rad = std::min({sh.radius, r.w / 2, r.h / 2});
        int x = static_cast<int>(std::round(r.x)), y = static_cast<int>(std::round(r.y));
        int w = static_cast<int>(std::round(r.w)), h = static_cast<int>(std::round(r.h));
        int ri = static_cast<int>(std::ceil(rad));
        if (ri <= 0) {
            wl_region_add(reg, x, y, w, h);
            continue;
        }
        // Docked: the concave ears that join the shape to the edge are part
        // of it too (left out, they showed as a sharp corner of plain glass).
        if (sh.square_top) {
            float e = std::min(10.0f, rad);
            int ei = static_cast<int>(std::ceil(e));
            for (int dy = 0; dy < ei; ++dy) {
                // The ear is widest at the edge (e) and narrows to 0 at depth e;
                // measured at the slice's lower row so it stays inside the curve.
                float yy = e - (dy + 1);
                int ew = static_cast<int>(std::floor(e - std::sqrt(std::max(0.0f, e * e - yy * yy))));
                if (ew <= 0) continue;
                wl_region_add(reg, x - ew, y + dy, ew, 1);
                wl_region_add(reg, x + w, y + dy, ew, 1);
            }
        }
        // Middle band, then 2 px slices following the corner arcs.
        int top = sh.square_top ? 0 : ri;
        wl_region_add(reg, x, y + top, w, h - top - ri);
        // 1 px slices, each measured at its outermost row and rounded up, so
        // the region stays inside the curve (no blurred pixels peeking out).
        const int step = 1;
        for (int dy = 0; dy < ri; dy += step) {
            float yy = rad - dy;
            float inset = rad - std::sqrt(std::max(0.0f, rad * rad - yy * yy));
            int in = static_cast<int>(std::ceil(inset));
            int sh_h = std::min(step, ri - dy);
            if (!sh.square_top) wl_region_add(reg, x + in, y + dy, w - 2 * in, sh_h);
            wl_region_add(reg, x + in, y + h - dy - sh_h, w - 2 * in, sh_h);
        }
    }
    ext_background_effect_surface_v1_set_blur_region(blur_, reg);
    wl_region_destroy(reg);
}

void LayerSurface::set_exclusive_zone(int zone) {
    if (spec_.exclusive_zone == zone) return;
    spec_.exclusive_zone = zone;
    if (!layer_) return;
    zwlr_layer_surface_v1_set_exclusive_zone(layer_, zone);
    wl_surface_commit(surface_);
}

// ── seat listeners ─────────────────────────────────────────────────────────

static void ptr_enter(void* data, wl_pointer*, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<Wayland*>(data);
    self->pointer_surface_ = s;
    self->pointer_serial_ = serial;
    self->pointer_focus_ = self->find(s);
    self->apply_cursor();
    if (auto* f = self->pointer_focus_; f && f->on_pointer_enter)
        f->on_pointer_enter(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void ptr_leave(void* data, wl_pointer*, uint32_t, wl_surface* s) {
    auto* self = static_cast<Wayland*>(data);
    LayerSurface* f = self->find(s);
    if (self->pointer_surface_ == s) {
        self->pointer_surface_ = nullptr;
        self->pointer_focus_ = nullptr;
    }
    if (f && f->on_pointer_leave) f->on_pointer_leave();
}
static void ptr_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<Wayland*>(data);
    if (auto* f = self->pointer_focus_; f && f->on_pointer_motion)
        f->on_pointer_motion(wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void ptr_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    auto* self = static_cast<Wayland*>(data);
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (self->shield_surface_ && self->pointer_surface_ == self->shield_surface_) {
        if (pressed && self->on_outside_click) self->on_outside_click();
        return;
    }
    Button b = button == BTN_LEFT ? Button::Left : button == BTN_RIGHT ? Button::Right
             : button == BTN_MIDDLE ? Button::Middle : Button::Other;
    if (auto* f = self->pointer_focus_; f && f->on_button) f->on_button(b, pressed);
}
static void ptr_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* self = static_cast<Wayland*>(data);
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) self->axis_dy_ += v;
    else self->axis_dx_ += v;
    self->axis_pending_ = true;
}
static void ptr_frame(void* data, wl_pointer*) {
    auto* self = static_cast<Wayland*>(data);
    if (!self->axis_pending_) return;
    bool discrete = self->axis_v120_x_ != 0 || self->axis_v120_y_ != 0;
    double dx = discrete ? self->axis_v120_x_ / 120.0 : self->axis_dx_;
    double dy = discrete ? self->axis_v120_y_ / 120.0 : self->axis_dy_;
    self->axis_dx_ = self->axis_dy_ = 0;
    self->axis_v120_x_ = self->axis_v120_y_ = 0;
    self->axis_pending_ = false;
    if (auto* f = self->pointer_focus_; f && f->on_scroll) f->on_scroll(dx, dy, discrete);
}
static void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
static void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
static void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
static void ptr_axis_value120(void* data, wl_pointer*, uint32_t axis, int32_t v) {
    auto* self = static_cast<Wayland*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) self->axis_v120_y_ += v;
    else self->axis_v120_x_ += v;
    self->axis_pending_ = true;
}
static void ptr_axis_rel(void*, wl_pointer*, uint32_t, uint32_t) {}
static const wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_rel,
};

static void kb_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto* self = static_cast<Wayland*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) return;
    xkb_keymap* km =
        xkb_keymap_new_from_string(self->xkb_ctx_, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km) return;
    if (self->xkb_state_) xkb_state_unref(self->xkb_state_);
    if (self->xkb_keymap_) xkb_keymap_unref(self->xkb_keymap_);
    self->xkb_keymap_ = km;
    self->xkb_state_ = xkb_state_new(km);
}
static void kb_enter(void* data, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) {
    auto* self = static_cast<Wayland*>(data);
    self->keyboard_focus_ = self->find(s);
}
static void kb_leave(void* data, wl_keyboard*, uint32_t, wl_surface* s) {
    auto* self = static_cast<Wayland*>(data);
    self->stop_repeat();
    LayerSurface* f = self->find(s);
    if (self->keyboard_focus_ == f) self->keyboard_focus_ = nullptr;
    if (f && f->on_keyboard_leave) f->on_keyboard_leave();
}
static void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    static_cast<Wayland*>(data)->handle_key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
static void kb_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp) {
    auto* self = static_cast<Wayland*>(data);
    if (self->xkb_state_) xkb_state_update_mask(self->xkb_state_, dep, lat, lock, 0, 0, grp);
}
static void kb_repeat(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    auto* self = static_cast<Wayland*>(data);
    self->repeat_rate_ = rate;
    self->repeat_delay_ = delay;
}
static const wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

static void seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<Wayland*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
        self->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self->pointer_, &pointer_listener, self);
        if (self->cursor_mgr_) self->cursor_dev_ = wp_cursor_shape_manager_v1_get_pointer(self->cursor_mgr_, self->pointer_);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self->pointer_) {
        if (self->cursor_dev_) {
            wp_cursor_shape_device_v1_destroy(self->cursor_dev_);
            self->cursor_dev_ = nullptr;
        }
        wl_pointer_release(self->pointer_);
        self->pointer_ = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard_) {
        self->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(self->keyboard_, &keyboard_listener, self);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self->keyboard_) {
        wl_keyboard_release(self->keyboard_);
        self->keyboard_ = nullptr;
    }
}
static void seat_name(void*, wl_seat*, const char*) {}
static const wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void reg_global(void* data, wl_registry* reg, uint32_t id, const char* iface, uint32_t ver) {
    auto* self = static_cast<Wayland*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        self->compositor_version_ = std::min(ver, 6u);
        self->compositor_ =
            static_cast<wl_compositor*>(wl_registry_bind(reg, id, &wl_compositor_interface, self->compositor_version_));
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        self->shm_ = static_cast<wl_shm*>(wl_registry_bind(reg, id, &wl_shm_interface, 1));
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        self->layer_shell_version_ = std::min(ver, 4u);
        self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(reg, id, &zwlr_layer_shell_v1_interface, self->layer_shell_version_));
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && !self->seat_) {
        self->seat_ = static_cast<wl_seat*>(wl_registry_bind(reg, id, &wl_seat_interface, std::min(ver, 8u)));
    } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        self->cursor_mgr_ =
            static_cast<wp_cursor_shape_manager_v1*>(wl_registry_bind(reg, id, &wp_cursor_shape_manager_v1_interface, 1));
    } else if (strcmp(iface, ext_session_lock_manager_v1_interface.name) == 0) {
        self->lock_mgr_ = static_cast<ext_session_lock_manager_v1*>(
            wl_registry_bind(reg, id, &ext_session_lock_manager_v1_interface, 1));
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        self->outputs_.push_back(static_cast<wl_output*>(wl_registry_bind(reg, id, &wl_output_interface, std::min(ver, 3u))));
    } else if (strcmp(iface, ext_background_effect_manager_v1_interface.name) == 0) {
        self->bg_effect_ = static_cast<ext_background_effect_manager_v1*>(
            wl_registry_bind(reg, id, &ext_background_effect_manager_v1_interface, 1));
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        self->viewporter_ = static_cast<wp_viewporter*>(wl_registry_bind(reg, id, &wp_viewporter_interface, 1));
    }
}
static void reg_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

static void shield_configure(void* data, zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t w, uint32_t h) {
    auto* self = static_cast<Wayland*>(data);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (!self->shield_surface_) return;
    if (!self->shield_buffer_) self->shield_buffer_ = self->make_transparent_pixel();
    if (!self->shield_buffer_) return;
    if (self->shield_viewport_)
        wp_viewport_set_destination(self->shield_viewport_, std::max<int>(1, (int)w), std::max<int>(1, (int)h));
    wl_surface_attach(self->shield_surface_, self->shield_buffer_, 0, 0);
    wl_surface_damage_buffer(self->shield_surface_, 0, 0, 1, 1);
    wl_surface_commit(self->shield_surface_);
}
static void shield_closed(void* data, zwlr_layer_surface_v1*) {
    auto* self = static_cast<Wayland*>(data);
    Loop::get().post([self] { self->set_shield(false); });
}
static const zwlr_layer_surface_v1_listener shield_listener = {.configure = shield_configure, .closed = shield_closed};

// ── Wayland ────────────────────────────────────────────────────────────────

Wayland::~Wayland() {
    stop_repeat();
    if (repeat_fd_ >= 0) close(repeat_fd_);
    set_shield(false);
    for (auto* s : surfaces_) s->wl_ = nullptr;
    if (cursor_dev_) wp_cursor_shape_device_v1_destroy(cursor_dev_);
    if (cursor_mgr_) wp_cursor_shape_manager_v1_destroy(cursor_mgr_);
    if (viewporter_) wp_viewporter_destroy(viewporter_);
    if (pointer_) wl_pointer_release(pointer_);
    if (keyboard_) wl_keyboard_release(keyboard_);
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_) xkb_context_unref(xkb_ctx_);
    if (seat_) wl_seat_destroy(seat_);
    if (layer_shell_) zwlr_layer_shell_v1_destroy(layer_shell_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) {
        wl_display_flush(display_);
        wl_display_disconnect(display_);
    }
}

bool Wayland::init() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "[wayland] cannot connect to the display\n";
        return false;
    }
    xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);
    if (!compositor_ || !shm_ || !layer_shell_) {
        std::cerr << "[wayland] compositor lacks wl_compositor, wl_shm or zwlr_layer_shell_v1\n";
        return false;
    }
    if (seat_) {
        wl_seat_add_listener(seat_, &seat_listener, this);
        wl_display_roundtrip(display_);
    }
    repeat_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    Loop::get().watch(repeat_fd_, POLLIN, [this](short) {
        uint64_t n = 0;
        if (read(repeat_fd_, &n, sizeof(n)) <= 0) return;
        if (repeat_key_) handle_key(repeat_key_, true);
    });
    return true;
}

std::unique_ptr<LayerSurface> Wayland::create_layer(const LayerSpec& spec) {
    auto s = std::make_unique<LayerSurface>();
    s->wl_ = this;
    s->spec_ = spec;
    s->width_ = spec.width;
    s->height_ = spec.height;
    s->surface_ = wl_compositor_create_surface(compositor_);
    wl_surface_add_listener(s->surface_, &surface_listener, s.get());
    s->layer_ = zwlr_layer_shell_v1_get_layer_surface(layer_shell_, s->surface_, nullptr, spec.layer, spec.name.c_str());
    zwlr_layer_surface_v1_add_listener(s->layer_, &layer_listener, s.get());
    zwlr_layer_surface_v1_set_anchor(s->layer_, spec.anchor);
    zwlr_layer_surface_v1_set_size(s->layer_, spec.width, spec.height);
    zwlr_layer_surface_v1_set_margin(s->layer_, spec.margin_top, spec.margin_side, 0, spec.margin_side);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer_, spec.exclusive_zone);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    s->kb_mode_ = KeyboardMode::None;
    if (spec.keyboard != KeyboardMode::None) s->set_keyboard(spec.keyboard);
    if (!spec.input_everywhere) {
        // Empty input until the owner says where its content is.
        wl_region* region = wl_compositor_create_region(compositor_);
        wl_surface_set_input_region(s->surface_, region);
        wl_region_destroy(region);
    }
    surfaces_.push_back(s.get());
    wl_surface_commit(s->surface_);
    wl_display_flush(display_);
    return s;
}

std::unique_ptr<LayerSurface> Wayland::create_lock_surface(ext_session_lock_v1* lock, wl_output* output) {
    auto s = std::make_unique<LayerSurface>();
    s->wl_ = this;
    s->spec_.name = "niri-island-lock";
    s->surface_ = wl_compositor_create_surface(compositor_);
    wl_surface_add_listener(s->surface_, &surface_listener, s.get());
    s->lock_surf_ = ext_session_lock_v1_get_lock_surface(lock, s->surface_, output);
    ext_session_lock_surface_v1_add_listener(s->lock_surf_, &lock_listener, s.get());
    s->kb_mode_ = KeyboardMode::Exclusive;  // a lock surface always has the keyboard
    surfaces_.push_back(s.get());
    wl_display_flush(display_);
    return s;
}

LayerSurface* Wayland::find(wl_surface* s) const {
    if (!s) return nullptr;
    for (auto* l : surfaces_)
        if (l->surface_ == s) return l;
    return nullptr;
}

void Wayland::forget(LayerSurface* s) {
    surfaces_.erase(std::remove(surfaces_.begin(), surfaces_.end(), s), surfaces_.end());
    if (pointer_focus_ == s) {
        pointer_focus_ = nullptr;
        pointer_surface_ = nullptr;
    }
    if (keyboard_focus_ == s) {
        keyboard_focus_ = nullptr;
        stop_repeat();
    }
}

void Wayland::roundtrip() { wl_display_roundtrip(display_); }

void Wayland::apply_cursor() {
    if (!cursor_dev_ || !pointer_serial_) return;
    uint32_t s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (pointer_focus_) {
        switch (pointer_focus_->cursor_) {
            case CursorShape::Pointer: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER; break;
            case CursorShape::Text: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT; break;
            case CursorShape::Grab: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB; break;
            case CursorShape::Grabbing: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING; break;
            case CursorShape::Crosshair: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR; break;
            case CursorShape::Move: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE; break;
            case CursorShape::ResizeNW: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE; break;
            case CursorShape::ResizeNE: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE; break;
            case CursorShape::ResizeSW: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE; break;
            case CursorShape::ResizeSE: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE; break;
            case CursorShape::ResizeN: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE; break;
            case CursorShape::ResizeS: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE; break;
            case CursorShape::ResizeW: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE; break;
            case CursorShape::ResizeE: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE; break;
            default: break;
        }
    }
    wp_cursor_shape_device_v1_set_shape(cursor_dev_, pointer_serial_, s);
}

wl_buffer* Wayland::make_transparent_pixel() {
    int fd = create_shm_fd(4);
    if (fd < 0) return nullptr;
    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, 4);
    wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

void Wayland::set_shield(bool on) {
    if (on) {
        if (shield_surface_ || !viewporter_) return;
        shield_surface_ = wl_compositor_create_surface(compositor_);
        shield_viewport_ = wp_viewporter_get_viewport(viewporter_, shield_surface_);
        shield_layer_ = zwlr_layer_shell_v1_get_layer_surface(layer_shell_, shield_surface_, nullptr,
                                                              ZWLR_LAYER_SHELL_V1_LAYER_TOP, "niri-island-shield");
        zwlr_layer_surface_v1_add_listener(shield_layer_, &shield_listener, this);
        zwlr_layer_surface_v1_set_anchor(shield_layer_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(shield_layer_, 0, 0);
        zwlr_layer_surface_v1_set_exclusive_zone(shield_layer_, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(shield_layer_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(shield_surface_);
    } else {
        if (pointer_surface_ && pointer_surface_ == shield_surface_) pointer_surface_ = nullptr;
        if (shield_viewport_) wp_viewport_destroy(shield_viewport_);
        if (shield_layer_) zwlr_layer_surface_v1_destroy(shield_layer_);
        if (shield_surface_) wl_surface_destroy(shield_surface_);
        if (shield_buffer_) wl_buffer_destroy(shield_buffer_);
        shield_viewport_ = nullptr;
        shield_layer_ = nullptr;
        shield_surface_ = nullptr;
        shield_buffer_ = nullptr;
    }
}

void Wayland::handle_key(uint32_t key, bool pressed) {
    if (!xkb_state_) return;
    if (!pressed) {
        if (key == repeat_key_) stop_repeat();
        return;
    }
    uint32_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state_, code);
    char buf[64] = {0};
    xkb_state_key_get_utf8(xkb_state_, code, buf, sizeof(buf));
    uint32_t mods = 0;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= MOD_CTRL;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= MOD_SHIFT;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= MOD_ALT;
    if (xkb_state_mod_name_is_active(xkb_state_, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= MOD_SUPER;
    if (key != repeat_key_ && xkb_keymap_key_repeats(xkb_keymap_, code)) start_repeat(key);
    if (auto* f = keyboard_focus_; f && f->on_key) f->on_key(sym, buf, mods);
}

void Wayland::start_repeat(uint32_t key) {
    if (repeat_rate_ <= 0 || repeat_fd_ < 0) return;
    repeat_key_ = key;
    itimerspec its{};
    its.it_value.tv_sec = repeat_delay_ / 1000;
    its.it_value.tv_nsec = (repeat_delay_ % 1000) * 1000000L;
    long interval_ns = 1000000000L / repeat_rate_;
    its.it_interval.tv_sec = interval_ns / 1000000000L;
    its.it_interval.tv_nsec = interval_ns % 1000000000L;
    timerfd_settime(repeat_fd_, 0, &its, nullptr);
}

void Wayland::stop_repeat() {
    repeat_key_ = 0;
    if (repeat_fd_ < 0) return;
    itimerspec its{};
    timerfd_settime(repeat_fd_, 0, &its, nullptr);
}

int Wayland::fd() const { return display_ ? wl_display_get_fd(display_) : -1; }

void Wayland::dispatch_pending() {
    if (!display_) return;
    wl_display_dispatch_pending(display_);
    if (wl_display_flush(display_) < 0 && errno != EAGAIN) {
        std::cerr << "[wayland] connection lost\n";
        Loop::get().quit();
    }
}

void Wayland::read_events() {
    // Never block here: wl_display_dispatch() waits for data when someone else
    // already drained what poll() announced, and the whole loop would hang.
    while (wl_display_prepare_read(display_) != 0) wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
    if (wl_display_read_events(display_) < 0 && errno != EAGAIN) {
        std::cerr << "[wayland] read failed: " << strerror(errno) << "\n";
        Loop::get().quit();
        return;
    }
    if (wl_display_dispatch_pending(display_) < 0) {
        std::cerr << "[wayland] dispatch failed: " << strerror(errno) << "\n";
        Loop::get().quit();
    }
}

}  // namespace island
