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
        } else {
            std::cout << "[Island] OSD Brightness: " << static_cast<int>(std::round(state.value * 100)) << "%\n";
        }
        update_target_dimensions();
        wayland_.request_frame_callback();
    };

    if (!osd_.init()) {
        std::cerr << "Failed to init OSD Manager\n";
    }

    update_target_dimensions();

    // Kick off first frame
    wayland_.request_frame_callback();

    return true;
}

void IslandApp::update_target_dimensions() {
    if (osd_.is_active()) {
        width_spring_.set_target(210.0f);
        height_spring_.set_target(34.0f);
        current_mode_ = IslandMode::HudOSD;
    } else if (expanded_) {
        width_spring_.set_target(330.0f);
        height_spring_.set_target(115.0f);
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
    if (osd_.is_active()) return; // Don't interrupt active HUD

    if (hovered && mpris_.info().active) {
        expanded_ = true;
    } else if (!hovered) {
        expanded_ = false;
    }
    update_target_dimensions();
    wayland_.request_frame_callback();
}

void IslandApp::on_click(int button, int state, double x, double y) {
    // Only handle button press (state == 1), left click (button == 272 / BTN_LEFT in Linux evdev)
    if (state != 1 || button != 272) return;

    if (osd_.is_active()) return;

    if (expanded_) {
        float cur_w = width_spring_.value;
        // Control buttons row: y around 80 - 110
        if (y >= 75.0) {
            float mid_x = cur_w / 2.0f;
            if (x < mid_x - 20.0f) {
                mpris_.previous();
            } else if (x > mid_x + 20.0f) {
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
    if (dt > 0.05f) dt = 0.05f; // Clamp delta to avoid physics explosion on stalls

    anim_time_ += dt;

    // Check if HUD expired
    if (current_mode_ == IslandMode::HudOSD && !osd_.is_active()) {
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
        renderer_.draw(cr, cur_w, cur_h, current_mode_, mpris_.info(), osd_.state(), anim_time_);
        wayland_.commit_frame(cur_w, cur_h);
    }

    // Continue frame loop if physics are moving, or audio equalizer is dancing, or HUD is active
    bool needs_next_frame = animating_w || animating_h || osd_.is_active() ||
                           (current_mode_ == IslandMode::MusicCompact && mpris_.info().playing);

    if (needs_next_frame) {
        wayland_.request_frame_callback();
    }
}

void IslandApp::run() {
    struct pollfd fds[3];
    int n_fds = 2;
    fds[0].fd = wayland_.get_fd();
    fds[0].events = POLLIN;
    fds[1].fd = mpris_.get_fd();
    fds[1].events = POLLIN;

    if (osd_.get_udev_fd() >= 0) {
        fds[2].fd = osd_.get_udev_fd();
        fds[2].events = POLLIN;
        n_fds = 3;
    }

    while (running_) {
        // Prepare Wayland events
        while (wl_display_prepare_read(wayland_.get_display()) != 0) {
            wayland_.dispatch();
        }
        wl_display_flush(wayland_.get_display());

        int timeout = osd_.is_active() ? 50 : 500;
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

        if (n_fds == 3 && (fds[2].revents & POLLIN)) {
            osd_.process_udev_events();
        }

        // Handle HUD timeout transition
        if (current_mode_ == IslandMode::HudOSD && !osd_.is_active()) {
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
