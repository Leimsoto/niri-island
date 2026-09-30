#pragma once

#include "wayland.hpp"
#include "renderer.hpp"
#include "mpris.hpp"
#include "osd.hpp"
#include "notifications.hpp"
#include "spring.hpp"
#include "ipc.hpp"
#include "sysmon.hpp"
#include "workspaces.hpp"
#include <chrono>

namespace island {

enum class DragTarget {
    None,
    MusicSeek,
    ControlVolume,
    ControlBrightness
};

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
    IpcServer ipc_;
    SystemMonitor sysmon_;
    WorkspaceManager workspaces_;

    Spring width_spring_;
    Spring height_spring_;

    // Tactile Slider Springs
    Spring volume_press_spring_{0.0f, 320.0f, 26.0f};
    Spring volume_bounce_spring_{0.0f, 280.0f, 20.0f};
    Spring brightness_press_spring_{0.0f, 320.0f, 26.0f};
    Spring brightness_bounce_spring_{0.0f, 280.0f, 20.0f};

    // Workspace Elastic Springs
    Spring ws_pill_spring_{0.0f, 300.0f, 22.0f};
    Spring ws_bounce_spring_{0.0f, 260.0f, 18.0f};

    // Notification Elastic Expansion Spring
    Spring notif_expand_spring_{0.0f, 320.0f, 26.0f};
    bool notif_expanded_ = false;

    // Interactive Calendar Offset
    int cal_month_offset_ = 0;

    float ws_bubble_w_ = 0.0f;

    IslandMode current_mode_ = IslandMode::Idle;
    bool expanded_ = false;
    bool in_control_center_ = false;
    bool running_ = true;

    ControlCenterState cc_state_;
    DragTarget active_drag_ = DragTarget::None;

    std::chrono::steady_clock::time_point last_frame_time_;
    std::chrono::steady_clock::time_point last_cc_poll_time_;
    float anim_time_ = 0.0f;

    void activate_app(const std::string& app_name);
    void update_target_dimensions();
    void update_control_center_info();
    void on_frame();
    void on_click(int button, int state, double x, double y);
    void on_motion(double x, double y);
    void on_hover(bool hovered);
    std::string handle_ipc_command(const std::string& cmd);
};

} // namespace island
