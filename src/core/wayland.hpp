#pragma once

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "core/util.hpp"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-background-effect-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

namespace island {

enum class CursorShape { Default, Pointer, Text, Grab, Grabbing, Crosshair, Move, ResizeNW, ResizeNE, ResizeSW, ResizeSE,
                         ResizeN, ResizeS, ResizeW, ResizeE };

enum class KeyboardMode { None, Exclusive, OnDemand };

enum class Button { Left, Right, Middle, Other };

class Wayland;

struct LayerSpec {
    uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    int width = 0, height = 0;          // 0 with opposite anchors = fill
    int margin_top = 0;
    int margin_side = 0;                // left and right
    int exclusive_zone = 0;
    KeyboardMode keyboard = KeyboardMode::None;
    std::string name = "niri-island";
    bool input_everywhere = false;      // full input region (overlays)
};

// One layer-shell surface with its own buffers, input and callbacks. Several
// can live at once: the island, and full-screen overlays (wallpaper picker,
// screenshot editor, region selector).
class LayerSurface {
public:
    ~LayerSurface();

    cairo_t* begin_frame(const Rect& area = {});
    void end_frame(const Rect& damage);
    bool frame_pending() const { return frame_cb_ != nullptr; }
    bool configured() const { return configured_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int scale() const { return scale_; }
    wl_surface* surface() const { return surface_; }

    void set_input_rects(const std::vector<Rect>& rects);
    void set_keyboard(KeyboardMode mode);
    KeyboardMode keyboard_mode() const { return kb_mode_; }
    void set_cursor(CursorShape shape);
    void set_margin(int top, int side = -1);
    void set_exclusive_zone(int zone);
    // Compositor blur behind these shapes (ext-background-effect). Rounded
    // shapes are approximated with thin slices; empty clears it.
    struct BlurShape {
        Rect r;
        float radius = 0;
        bool square_top = false;  // docked: no rounding on top
    };
    void set_blur(const std::vector<BlurShape>& shapes);
    void commit() { if (surface_) wl_surface_commit(surface_); }

    std::function<void()> on_frame;
    std::function<void()> on_configure;
    std::function<void()> on_closed;
    std::function<void(double x, double y)> on_pointer_enter;
    std::function<void()> on_pointer_leave;
    std::function<void(double x, double y)> on_pointer_motion;
    std::function<void(Button b, bool pressed)> on_button;
    std::function<void(double dx, double dy, bool discrete)> on_scroll;
    std::function<void(xkb_keysym_t sym, const std::string& utf8, uint32_t mods)> on_key;
    std::function<void()> on_keyboard_leave;

    // internal state, reached from the C listeners
    struct Buffer {
        wl_buffer* wl = nullptr;
        void* data = nullptr;
        size_t size = 0;
        cairo_surface_t* surface = nullptr;
        cairo_t* cr = nullptr;
        bool busy = false;
        Rect drawn{};
        bool full = true;
    };
    Wayland* wl_ = nullptr;
    LayerSpec spec_;
    wl_surface* surface_ = nullptr;
    zwlr_layer_surface_v1* layer_ = nullptr;
    wl_callback* frame_cb_ = nullptr;
    ext_session_lock_surface_v1* lock_surf_ = nullptr;  // a lock-screen surface instead of a layer
    Buffer buffers_[3];
    int width_ = 0, height_ = 0, scale_ = 1;
    int buf_w_ = 0, buf_h_ = 0;
    bool configured_ = false;
    KeyboardMode kb_mode_ = KeyboardMode::None;
    CursorShape cursor_ = CursorShape::Default;
    int current_ = -1;
    ext_background_effect_surface_v1* blur_ = nullptr;
    std::vector<BlurShape> blur_shapes_;

    void destroy_buffers();
    bool create_buffers();
};

// Globals, the seat and the event pump. Input goes to whichever surface has
// pointer or keyboard focus.
class Wayland {
public:
    Wayland() = default;
    ~Wayland();

    bool init();
    std::unique_ptr<LayerSurface> create_layer(const LayerSpec& spec);
    // A lock-screen surface on one output (ext-session-lock).
    std::unique_ptr<LayerSurface> create_lock_surface(ext_session_lock_v1* lock, wl_output* output);
    bool can_lock() const { return lock_mgr_ != nullptr; }
    const std::vector<wl_output*>& outputs() const { return outputs_; }

    void set_shield(bool on);
    bool shield_active() const { return shield_surface_ != nullptr; }
    bool can_blur() const { return bg_effect_ != nullptr; }
    std::function<void()> on_outside_click;

    int fd() const;
    void dispatch_pending();
    void read_events();
    void roundtrip();

    static constexpr uint32_t MOD_CTRL = 1, MOD_SHIFT = 2, MOD_ALT = 4, MOD_SUPER = 8;

    // internal
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    uint32_t compositor_version_ = 0;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    uint32_t layer_shell_version_ = 0;
    wp_cursor_shape_manager_v1* cursor_mgr_ = nullptr;
    ext_background_effect_manager_v1* bg_effect_ = nullptr;
    ext_session_lock_manager_v1* lock_mgr_ = nullptr;
    std::vector<wl_output*> outputs_;
    wp_cursor_shape_device_v1* cursor_dev_ = nullptr;
    wp_viewporter* viewporter_ = nullptr;

    xkb_context* xkb_ctx_ = nullptr;
    xkb_keymap* xkb_keymap_ = nullptr;
    xkb_state* xkb_state_ = nullptr;
    int repeat_rate_ = 25, repeat_delay_ = 600;
    int repeat_fd_ = -1;
    uint32_t repeat_key_ = 0;

    std::vector<LayerSurface*> surfaces_;
    LayerSurface* pointer_focus_ = nullptr;
    LayerSurface* keyboard_focus_ = nullptr;
    wl_surface* pointer_surface_ = nullptr;
    uint32_t pointer_serial_ = 0;
    double axis_dx_ = 0, axis_dy_ = 0;
    int axis_v120_x_ = 0, axis_v120_y_ = 0;
    bool axis_pending_ = false;

    wl_surface* shield_surface_ = nullptr;
    zwlr_layer_surface_v1* shield_layer_ = nullptr;
    wp_viewport* shield_viewport_ = nullptr;
    wl_buffer* shield_buffer_ = nullptr;

    LayerSurface* find(wl_surface* s) const;
    void forget(LayerSurface* s);
    void apply_cursor();
    void handle_key(uint32_t key, bool pressed);
    void start_repeat(uint32_t key);
    void stop_repeat();
    wl_buffer* make_transparent_pixel();
};

}  // namespace island
