#include "island.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <algorithm>

namespace island {

IslandApp::IslandApp()
    : width_spring_(110.0f, 260.0f, 22.0f),
      height_spring_(32.0f, 260.0f, 22.0f) {}

IslandApp::~IslandApp() = default;

bool IslandApp::init() {
    if (!wayland_.init()) {
        std::cerr << "Failed to init Wayland\n";
        return false;
    }

    // Set Wayland callbacks
    wayland_.on_frame = [this]() {
        on_frame();
    };

    wayland_.on_click = [this](int button, int state, double x, double y) {
        on_click(button, state, x, y);
    };

    wayland_.on_hover = [this](bool hovered) {
        on_hover(hovered);
    };

    // Initial surface creation with idle default dimensions
    wayland_.create_surface(110, 32);
    last_frame_time_ = std::chrono::steady_clock::now();

    // Set MPRIS callback
    mpris_.on_change = [this](const MediaInfo& info) {
        if (info.active) {
            std::cout << "[Island] Media: " << info.artist << " - " << info.title 
                      << " (" << (info.playing ? "Playing" : "Paused") << ")\n";
        }
        update_target_dimensions();
        wayland_.request_frame_callback();
    };

    if (!mpris_.init()) {
        std::cerr << "Failed to init MPRIS (continuing without media)\n";
    }

    // Set OSD callback
    osd_.on_osd_change = [this](const OsdState& state) {
        if (state.type == OsdType::Volume) {
            std::cout << "[Island] OSD Volume: " << static_cast<int>(std::round(state.value * 100))
                      << "% (" << (state.muted ? "Muted" : "Active") << ")\n";
        } else if (state.type == OsdType::Brightness) {
            std::cout << "[Island] OSD Brightness: " << static_cast<int>(std::round(state.value * 100)) << "%\n";
        } else if (state.type == OsdType::Battery) {
            std::cout << "[Island] OSD Battery: " << static_cast<int>(std::round(state.value * 100)) 
                      << "% (" << (state.is_charging ? "Charging" : "Discharging") << ")\n";
        }
        update_target_dimensions();
        wayland_.request_frame_callback();
    };

    osd_.on_mic_change = [this](bool) {
        wayland_.request_frame_callback();
    };

    if (!osd_.init()) {
        std::cerr << "Failed to init OSD Manager\n";
    }

    // Set Notification callback
    notifications_.on_notification = [this](const NotificationItem&) {
        update_target_dimensions();
        wayland_.request_frame_callback();
    };

    if (!notifications_.init()) {
        std::cerr << "Failed to init Notification Manager (continuing without notifications)\n";
    }

    update_target_dimensions();

    // Kick off first frame
    wayland_.request_frame_callback();

    return true;
}

void IslandApp::update_target_dimensions() {
    if (notifications_.has_active()) {
        width_spring_.set_target(330.0f);
        height_spring_.set_target(70.0f);
        current_mode_ = IslandMode::Notification;
    } else if (osd_.is_active()) {
        width_spring_.set_target(210.0f);
        height_spring_.set_target(34.0f);
        current_mode_ = IslandMode::HudOSD;
    } else if (expanded_) {
        width_spring_.set_target(330.0f);
        height_spring_.set_target(125.0f);
        current_mode_ = IslandMode::MusicExpanded;
    } else if (mpris_.info().active) {
        width_spring_.set_target(240.0f);
        height_spring_.set_target(34.0f);
        current_mode_ = IslandMode::MusicCompact;
    } else {
        width_spring_.set_target(110.0f);
        height_spring_.set_target(32.0f);
        current_mode_ = IslandMode::Idle;
    }
}

void IslandApp::on_hover(bool hovered) {
    if (notifications_.has_active() || osd_.is_active()) return;

    if (hovered && mpris_.info().active) {
        expanded_ = true;
    } else if (!hovered) {
        expanded_ = false;
    }
    update_target_dimensions();
    wayland_.request_frame_callback();
}

void IslandApp::on_click(int button, int state, double x, double y) {
    if (state != 1 || button != 272) return;

    // Dismiss notification on click
    if (notifications_.has_active()) {
        notifications_.dismiss();
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }

    if (osd_.is_active()) return;

    if (expanded_) {
        float cur_w = width_spring_.value;

        // Interactive seek on progress bar (y between 66 and 86)
        if (y >= 66.0 && y <= 86.0) {
            float bar_x = 16.0f;
            float bar_w = cur_w - 32.0f;
            float ratio = std::clamp((static_cast<float>(x) - bar_x) / bar_w, 0.0f, 1.0f);
            mpris_.set_position_ratio(ratio);
            wayland_.request_frame_callback();
            return;
        }

        // Control buttons row: y around 88 - 120
        if (y > 86.0) {
            float mid_x = cur_w / 2.0f;
            if (x < mid_x - 25.0f) {
                mpris_.previous();
            } else if (x > mid_x + 25.0f) {
                mpris_.next();
            } else {
                mpris_.play_pause();
            }
            return;
        }
    }

    // Toggle expansion on body click
    expanded_ = !expanded_;
    update_target_dimensions();
    wayland_.request_frame_callback();
}

void IslandApp::on_frame() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;
    if (dt > 0.05f) dt = 0.05f;

    anim_time_ += dt;

    // Advance song position smoothly
    mpris_.tick(dt);

    // Check if notification or HUD expired
    if ((current_mode_ == IslandMode::Notification && !notifications_.has_active()) ||
        (current_mode_ == IslandMode::HudOSD && !osd_.is_active())) {
        update_target_dimensions();
    }

    bool animating_w = width_spring_.update(dt);
    bool animating_h = height_spring_.update(dt);

    int cur_w = std::max(20, (int)std::round(width_spring_.value));
    int cur_h = std::max(20, (int)std::round(height_spring_.value));

    // Update Wayland surface size & render
    wayland_.resize_surface(cur_w, cur_h);

    cairo_t* cr = wayland_.get_cairo_context(cur_w, cur_h);
    if (cr) {
        renderer_.draw(cr, cur_w, cur_h, current_mode_, mpris_.info(), 
                       osd_.state(), notifications_.current(), 
                       osd_.is_mic_active(), anim_time_);
        wayland_.commit_frame(cur_w, cur_h);
    }

    // Continue frame loop if animations are active
    bool needs_next_frame = animating_w || animating_h || 
                           osd_.is_active() || notifications_.has_active() ||
                           (mpris_.info().active && mpris_.info().playing);

    if (needs_next_frame) {
        wayland_.request_frame_callback();
    }
}

void IslandApp::run() {
    struct pollfd fds[4];
    int n_fds = 2;
    fds[0].fd = wayland_.get_fd();
    fds[0].events = POLLIN;
    fds[1].fd = mpris_.get_fd();
    fds[1].events = POLLIN;

    int udev_idx = -1;
    if (osd_.get_udev_fd() >= 0) {
        udev_idx = n_fds;
        fds[n_fds].fd = osd_.get_udev_fd();
        fds[n_fds].events = POLLIN;
        n_fds++;
    }

    int notif_idx = -1;
    if (notifications_.get_fd() >= 0) {
        notif_idx = n_fds;
        fds[n_fds].fd = notifications_.get_fd();
        fds[n_fds].events = POLLIN;
        n_fds++;
    }

    while (running_) {
        // Prepare Wayland events
        while (wl_display_prepare_read(wayland_.get_display()) != 0) {
            wayland_.dispatch();
        }
        wl_display_flush(wayland_.get_display());

        bool fast_poll = osd_.is_active() || notifications_.has_active() || 
                         (mpris_.info().active && mpris_.info().playing);
        int timeout = fast_poll ? 50 : 500;

        int ret = poll(fds, n_fds, timeout);
        if (ret < 0) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(wayland_.get_display());
            wayland_.dispatch();
        } else {
            wl_display_cancel_read(wayland_.get_display());
        }

        if (fds[1].revents & POLLIN) {
            mpris_.update();
        }

        if (udev_idx >= 0 && (fds[udev_idx].revents & POLLIN)) {
            osd_.process_udev_events();
        }

        if (notif_idx >= 0 && (fds[notif_idx].revents & POLLIN)) {
            notifications_.update();
        }

        // Handle Mode expiry
        if ((current_mode_ == IslandMode::Notification && !notifications_.has_active()) ||
            (current_mode_ == IslandMode::HudOSD && !osd_.is_active())) {
            update_target_dimensions();
            wayland_.request_frame_callback();
        }

        // If in Idle mode, request occasional redraw to update clock
        if (current_mode_ == IslandMode::Idle && width_spring_.is_settled() && height_spring_.is_settled()) {
            wayland_.request_frame_callback();
        }
    }
}

} // namespace island
