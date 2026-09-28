#pragma once

#include "wayland.hpp"
#include "renderer.hpp"
#include "mpris.hpp"
#include "osd.hpp"
#include "notifications.hpp"
#include "spring.hpp"
#include <chrono>

namespace island {

class IslandApp {
public:
    IslandApp();
    ~IslandApp();

    bool init();
    void run();

private:
    WaylandContext wayland_;
    MprisClient mpris_;
    OsdManager osd_;
    NotificationManager notifications_;
    Renderer renderer_;

    Spring width_spring_;
    Spring height_spring_;

    IslandMode current_mode_ = IslandMode::Idle;
    bool expanded_ = false;
    bool running_ = true;

    std::chrono::steady_clock::time_point last_frame_time_;
    float anim_time_ = 0.0f;

    void update_target_dimensions();
    void on_frame();
    void on_click(int button, int state, double x, double y);
    void on_hover(bool hovered);
};

} // namespace island
