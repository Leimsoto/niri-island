#pragma once

#include <wayland-client.h>
#include <cairo.h>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace island {

struct InputRect {
    int x;
    int y;
    int width;
    int height;
};

struct Buffer {
    struct wl_buffer* wl_buf = nullptr;
    void* shm_data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    bool busy = false;
    cairo_surface_t* cairo_surface = nullptr;
    cairo_t* cr = nullptr;

    void destroy();
};

class WaylandContext {
public:
    WaylandContext();
    ~WaylandContext();

    bool init();
    void create_surface(int width, int height);
    void resize_surface(int width, int height);
    void set_margin(int top);
    void set_input_regions(const std::vector<InputRect>& rects);

    void roundtrip();
    int dispatch();
    int get_fd() const;
    struct wl_display* get_display() const { return display_; }

    // Rendering
    cairo_t* get_cairo_context(int width, int height);
    void commit_frame(int width, int height);
    void request_frame_callback();

    // Callbacks
    std::function<void(int width, int height)> on_configure;
    std::function<void(bool hovered)> on_hover;
    std::function<void(int button, int state, double x, double y)> on_click;
    std::function<void(double x, double y)> on_motion;
    std::function<void()> on_frame;

    bool is_hovered() const { return hovered_; }
    int configured_width() const { return configured_width_; }
    int configured_height() const { return configured_height_; }

public:
    struct wl_display* display_ = nullptr;
    struct wl_registry* registry_ = nullptr;
    struct wl_compositor* compositor_ = nullptr;
    struct wl_shm* shm_ = nullptr;
    struct wl_seat* seat_ = nullptr;
    struct wl_pointer* pointer_ = nullptr;
    struct zwlr_layer_shell_v1* layer_shell_ = nullptr;

    struct wl_surface* surface_ = nullptr;
    struct zwlr_layer_surface_v1* layer_surface_ = nullptr;
    struct wl_callback* frame_callback_ = nullptr;

    Buffer buffers_[2];
    int current_buffer_ = 0;

    int configured_width_ = 0;
    int configured_height_ = 0;
    bool configured_ = false;
    bool hovered_ = false;
    double pointer_x_ = 0.0;
    double pointer_y_ = 0.0;

private:
    Buffer* create_buffer(int width, int height);
};

} // namespace island
