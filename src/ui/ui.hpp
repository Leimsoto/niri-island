#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "core/spring.hpp"
#include "core/util.hpp"
#include "core/wayland.hpp"

namespace island {

struct DragEvent {
    float x, y;    // pointer, surface coordinates
    float lx, ly;  // pointer in the coordinates the region was drawn in
    float fx, fy;  // pointer relative to the region, 0..1 inside (unclamped)
    Rect rect;     // the region, surface coordinates
};

// A clickable area registered while drawing. Hit testing uses the regions of
// the frame on screen, so what you click is exactly what was drawn — there
// are no hand-maintained coordinate tables to drift out of sync.
struct Region {
    uint64_t id = 0;
    Rect rect;
    Rect clip;
    float ox = 0, oy = 0, scale = 1;  // transform at registration
    CursorShape cursor = CursorShape::Pointer;
    std::function<void()> on_click;
    std::function<void()> on_right;
    std::function<void()> on_middle;
    std::function<void(const DragEvent&)> on_drag;  // press + motion while held
    std::function<void(const DragEvent&)> on_release;
    std::function<void(float dy)> on_scroll;

    Region& click(std::function<void()> f) { on_click = std::move(f); return *this; }
    Region& right(std::function<void()> f) { on_right = std::move(f); return *this; }
    Region& middle(std::function<void()> f) { on_middle = std::move(f); return *this; }
    Region& drag(std::function<void(const DragEvent&)> f) { on_drag = std::move(f); return *this; }
    Region& release(std::function<void(const DragEvent&)> f) { on_release = std::move(f); return *this; }
    Region& scroll(std::function<void(float)> f) { on_scroll = std::move(f); return *this; }
    Region& cursor_shape(CursorShape c) { cursor = c; return *this; }
};

class Ui {
public:
    // Call once per frame before drawing.
    void begin_frame(float dt);
    // The frame could not be drawn after all: keep hit testing on the last one.
    void abort_frame() { cur_ = last_; }

    // Transform stack applied to registered regions (mirrors cairo translate/scale).
    void push(float ox, float oy, float scale = 1.0f);
    void pop();
    void push_clip(const Rect& local);
    void pop_clip();
    // While disabled, regions are not registered (content fading out).
    void set_enabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }

    Region& add(uint64_t id, const Rect& local);

    // Animated interaction state of a region id, 0..1.
    float hover(uint64_t id);
    float press(uint64_t id);
    bool hovered(uint64_t id) const { return hot_ == id && id != 0; }
    bool held(uint64_t id) const { return active_ == id && id != 0; }
    // Visual tap: makes a region bounce as if pressed (for keyboard / IPC).
    void pulse(uint64_t id);

    bool animating() const { return animating_; }

    // Input, surface coordinates. Return true when a region consumed it.
    void motion(float x, float y);
    void leave();
    bool button(Button b, bool pressed);
    bool scroll(float dy);
    bool dragging() const { return active_ != 0 && active_has_drag_; }
    CursorShape cursor() const;
    float px() const { return px_; }
    float py() const { return py_; }
    bool inside() const { return inside_; }
    // Local coordinates of the pointer under the current transform.
    float local_x() const;
    float local_y() const;
    // Pointer in the drawn coordinates of the region being clicked (valid in callbacks).
    float click_x() const { return click_x_; }
    float click_y() const { return click_y_; }
    // Drop interaction state (mode change).
    void cancel();

private:
    struct Anim {
        Spring hover{0.0f, 520.0f, 40.0f};
        Spring press{0.0f, 700.0f, 30.0f};
        double seen = 0;
    };
    struct Xf {
        float ox, oy, s;
    };

    std::vector<Region> cur_, last_;
    std::vector<Xf> xf_{{0, 0, 1}};
    std::vector<Rect> clips_;
    std::unordered_map<uint64_t, Anim> anims_;
    bool enabled_ = true;
    bool animating_ = false;
    float px_ = -1, py_ = -1;
    bool inside_ = false;
    uint64_t hot_ = 0, active_ = 0;
    bool active_has_drag_ = false;
    Region active_copy_;
    double now_ = 0;
    float click_x_ = 0, click_y_ = 0;
    void set_click_local(const Region& r);

    const Region* hit(float x, float y, bool (*want)(const Region&)) const;
    void update_hot();
    Anim& anim(uint64_t id);
    DragEvent drag_event(const Region& r) const;
};

}  // namespace island
