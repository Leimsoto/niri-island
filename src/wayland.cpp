#include "wayland.hpp"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace island {

void Buffer::destroy() {
    if (cr) {
        cairo_destroy(cr);
        cr = nullptr;
    }
    if (cairo_surface) {
        cairo_surface_destroy(cairo_surface);
        cairo_surface = nullptr;
    }
    if (shm_data && size > 0) {
        munmap(shm_data, size);
        shm_data = nullptr;
    }
    if (wl_buf) {
        wl_buffer_destroy(wl_buf);
        wl_buf = nullptr;
    }
    busy = false;
    width = 0;
    height = 0;
    size = 0;
}

WaylandContext::WaylandContext() = default;

WaylandContext::~WaylandContext() {
    buffers_[0].destroy();
    buffers_[1].destroy();

    if (frame_callback_) {
        wl_callback_destroy(frame_callback_);
    }
    if (layer_surface_) {
        zwlr_layer_surface_v1_destroy(layer_surface_);
    }
    if (surface_) {
        wl_surface_destroy(surface_);
    }
    if (pointer_) {
        wl_pointer_destroy(pointer_);
    }
    if (seat_) {
        wl_seat_destroy(seat_);
    }
    if (layer_shell_) {
        zwlr_layer_shell_v1_destroy(layer_shell_);
    }
    if (shm_) {
        wl_shm_destroy(shm_);
    }
    if (compositor_) {
        wl_compositor_destroy(compositor_);
    }
    if (registry_) {
        wl_registry_destroy(registry_);
    }
    if (display_) {
        wl_display_disconnect(display_);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = [](void* data, struct wl_registry* reg, uint32_t id, const char* iface, uint32_t ver) {
        auto* self = static_cast<WaylandContext*>(data);
        if (strcmp(iface, wl_compositor_interface.name) == 0) {
            self->compositor_ = (struct wl_compositor*)wl_registry_bind(reg, id, &wl_compositor_interface, std::min(ver, 4u));
        } else if (strcmp(iface, wl_shm_interface.name) == 0) {
            self->shm_ = (struct wl_shm*)wl_registry_bind(reg, id, &wl_shm_interface, 1);
        } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
            self->layer_shell_ = (struct zwlr_layer_shell_v1*)wl_registry_bind(reg, id, &zwlr_layer_shell_v1_interface, std::min(ver, 4u));
        } else if (strcmp(iface, wl_seat_interface.name) == 0) {
            self->seat_ = (struct wl_seat*)wl_registry_bind(reg, id, &wl_seat_interface, std::min(ver, 5u));
        }
    },
    .global_remove = [](void*, struct wl_registry*, uint32_t) {}
};

static const struct wl_pointer_listener pointer_listener = {
    .enter = [](void* data, struct wl_pointer*, uint32_t, struct wl_surface*, wl_fixed_t, wl_fixed_t) {
        auto* self = static_cast<WaylandContext*>(data);
        self->hovered_ = true;
        if (self->on_hover) self->on_hover(true);
    },
    .leave = [](void* data, struct wl_pointer*, uint32_t, struct wl_surface*) {
        auto* self = static_cast<WaylandContext*>(data);
        self->hovered_ = false;
        if (self->on_hover) self->on_hover(false);
    },
    .motion = [](void* data, struct wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
        auto* self = static_cast<WaylandContext*>(data);
        self->pointer_x_ = wl_fixed_to_double(sx);
        self->pointer_y_ = wl_fixed_to_double(sy);
        if (self->on_motion) self->on_motion(self->pointer_x_, self->pointer_y_);
    },
    .button = [](void* data, struct wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
        auto* self = static_cast<WaylandContext*>(data);
        if (self->on_click) self->on_click(button, state, self->pointer_x_, self->pointer_y_);
    },
    .axis = [](void*, struct wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    .frame = [](void*, struct wl_pointer*) {},
    .axis_source = [](void*, struct wl_pointer*, uint32_t) {},
    .axis_stop = [](void*, struct wl_pointer*, uint32_t, uint32_t) {},
    .axis_discrete = [](void*, struct wl_pointer*, uint32_t, int32_t) {},
    .axis_value120 = [](void*, struct wl_pointer*, uint32_t, int32_t) {},
    .axis_relative_direction = [](void*, struct wl_pointer*, uint32_t, uint32_t) {}
};

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = [](void* data, struct zwlr_layer_surface_v1* surface, uint32_t serial, uint32_t width, uint32_t height) {
        auto* self = static_cast<WaylandContext*>(data);
        zwlr_layer_surface_v1_ack_configure(surface, serial);
        self->configured_ = true;
        self->configured_width_ = width;
        self->configured_height_ = height;
        if (self->on_configure) self->on_configure(width, height);
    },
    .closed = [](void*, struct zwlr_layer_surface_v1*) {}
};

static const struct wl_buffer_listener buffer_listener = {
    .release = [](void* data, struct wl_buffer*) {
        auto* buf = static_cast<Buffer*>(data);
        buf->busy = false;
    }
};

static const struct wl_callback_listener frame_listener = {
    .done = [](void* data, struct wl_callback* cb, uint32_t) {
        auto* self = static_cast<WaylandContext*>(data);
        wl_callback_destroy(cb);
        self->frame_callback_ = nullptr;
        if (self->on_frame) self->on_frame();
    }
};

bool WaylandContext::init() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "Failed to connect to Wayland display\n";
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !shm_ || !layer_shell_) {
        std::cerr << "Compositor missing required interfaces (wl_compositor, wl_shm, or zwlr_layer_shell_v1)\n";
        return false;
    }

    if (seat_) {
        pointer_ = wl_seat_get_pointer(seat_);
        if (pointer_) {
            wl_pointer_add_listener(pointer_, &pointer_listener, this);
        }
    }

    return true;
}

void WaylandContext::create_surface(int width, int height) {
    surface_ = wl_compositor_create_surface(compositor_);
    layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell_,
        surface_,
        nullptr, // Default output (focused / primary)
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "niri-island"
    );

    zwlr_layer_surface_v1_add_listener(layer_surface_, &layer_surface_listener, this);

    // Anchor top, centered
    zwlr_layer_surface_v1_set_anchor(layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
    zwlr_layer_surface_v1_set_size(layer_surface_, width, height);
    zwlr_layer_surface_v1_set_margin(layer_surface_, 10, 0, 0, 0); // 10px from top edge
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, 0); // Floating, don't push windows
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);
}

void WaylandContext::resize_surface(int width, int height) {
    if (!layer_surface_) return;
    zwlr_layer_surface_v1_set_size(layer_surface_, width, height);
}

void WaylandContext::set_margin(int top) {
    if (!layer_surface_) return;
    zwlr_layer_surface_v1_set_margin(layer_surface_, top, 0, 0, 0);
}

void WaylandContext::roundtrip() {
    wl_display_roundtrip(display_);
}

int WaylandContext::dispatch() {
    return wl_display_dispatch(display_);
}

int WaylandContext::get_fd() const {
    return wl_display_get_fd(display_);
}

Buffer* WaylandContext::create_buffer(int width, int height) {
    Buffer* buf = &buffers_[current_buffer_];
    if (buf->busy) {
        current_buffer_ = (current_buffer_ + 1) % 2;
        buf = &buffers_[current_buffer_];
    }

    if (buf->width == width && buf->height == height && buf->cairo_surface && buf->cr) {
        return buf;
    }

    buf->destroy();

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    size_t size = stride * height;

    int fd = syscall(SYS_memfd_create, "island-buffer", MFD_CLOEXEC);
    if (fd < 0) {
        std::cerr << "memfd_create failed\n";
        return nullptr;
    }

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return nullptr;
    }

    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, size);
    struct wl_buffer* wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);

    buf->wl_buf = wl_buf;
    buf->shm_data = data;
    buf->size = size;
    buf->width = width;
    buf->height = height;
    buf->busy = false;

    buf->cairo_surface = cairo_image_surface_create_for_data(
        (unsigned char*)data, CAIRO_FORMAT_ARGB32, width, height, stride
    );
    buf->cr = cairo_create(buf->cairo_surface);

    return buf;
}

cairo_t* WaylandContext::get_cairo_context(int width, int height) {
    Buffer* buf = create_buffer(width, height);
    if (!buf) return nullptr;

    // Clear to full transparency
    cairo_set_operator(buf->cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(buf->cr);
    cairo_set_operator(buf->cr, CAIRO_OPERATOR_OVER);

    return buf->cr;
}

void WaylandContext::commit_frame(int width, int height) {
    Buffer* buf = &buffers_[current_buffer_];
    if (!buf || !buf->wl_buf) return;

    cairo_surface_flush(buf->cairo_surface);

    wl_surface_attach(surface_, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, width, height);
    wl_surface_commit(surface_);

    buf->busy = true;
    current_buffer_ = (current_buffer_ + 1) % 2;
}

void WaylandContext::request_frame_callback() {
    if (!surface_ || frame_callback_) return; // No surface yet or already requested
    frame_callback_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_callback_, &frame_listener, this);
    wl_surface_commit(surface_);
}

} // namespace island
