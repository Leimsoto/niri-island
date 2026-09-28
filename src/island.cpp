#include "island.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

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

    wayland_.on_motion = [this](double x, double y) {
        on_motion(x, y);
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
    } else if (in_control_center_) {
        width_spring_.set_target(350.0f);
        height_spring_.set_target(240.0f);
        current_mode_ = IslandMode::ControlCenter;
    } else if (expanded_) {
        width_spring_.set_target(330.0f);
        height_spring_.set_target(130.0f);
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

void IslandApp::update_control_center_info() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - last_cc_poll_time_).count();

    // Always update instantaneous fields
    cc_state_.volume = osd_.current_volume();
    cc_state_.audio_muted = osd_.current_muted();
    cc_state_.brightness = osd_.current_brightness();
    cc_state_.battery_percent = osd_.current_battery_pct();
    cc_state_.battery_charging = osd_.current_battery_charging();
    cc_state_.media_active = mpris_.info().active;
    cc_state_.dnd_enabled = notifications_.dnd();

    if (elapsed < 3.0f && last_cc_poll_time_.time_since_epoch().count() != 0) {
        return;
    }
    last_cc_poll_time_ = now;

    // Wi-Fi Status & SSID
    FILE* fp = popen("nmcli radio wifi 2>/dev/null", "r");
    if (fp) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            cc_state_.wifi_enabled = (strstr(buf, "enabled") != nullptr);
        }
        pclose(fp);
    }

    if (cc_state_.wifi_enabled) {
        FILE* fp_ssid = popen("nmcli -t -f active,ssid dev wifi 2>/dev/null | grep '^yes' | cut -d: -f2", "r");
        if (fp_ssid) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp_ssid)) {
                std::string s = buf;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                cc_state_.wifi_ssid = s.empty() ? "Wi-Fi" : s;
            } else {
                cc_state_.wifi_ssid = "Wi-Fi";
            }
            pclose(fp_ssid);
        }
    } else {
        cc_state_.wifi_ssid = "Wi-Fi";
    }

    // Bluetooth Status
    FILE* fp_bt = popen("bluetoothctl show 2>/dev/null | grep 'Powered:'", "r");
    if (fp_bt) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp_bt)) {
            cc_state_.bluetooth_enabled = (strstr(buf, "yes") != nullptr);
        }
        pclose(fp_bt);
    }
}

void IslandApp::on_hover(bool hovered) {
    if (notifications_.has_active() || osd_.is_active() || in_control_center_) return;
    if (active_drag_ != DragTarget::None) return;

    if (hovered && mpris_.info().active) {
        expanded_ = true;
    } else if (!hovered) {
        expanded_ = false;
    }
    update_target_dimensions();
    wayland_.request_frame_callback();
}

void IslandApp::on_click(int button, int state, double x, double y) {
    if (button != 272) {
        if (button == 273 && state == 1) { // Right Click toggles Control Center
            in_control_center_ = !in_control_center_;
            if (in_control_center_) {
                expanded_ = false;
                update_control_center_info();
            }
            update_target_dimensions();
            wayland_.request_frame_callback();
            return;
        }
        return;
    }

    // Button release: finish drag
    if (state == 0) {
        active_drag_ = DragTarget::None;
        return;
    }

    // Button press:
    // Dismiss notification on click
    if (notifications_.has_active()) {
        notifications_.dismiss();
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }

    if (osd_.is_active()) return;

    float cur_w = width_spring_.value;

    // 1. Control Center Interactions
    if (in_control_center_) {
        // Header actions
        if (y <= 38.0) {
            // Media toggle button (if media active)
            if (cc_state_.media_active && x >= cur_w - 64.0 && x <= cur_w - 38.0) {
                in_control_center_ = false;
                expanded_ = true;
                update_target_dimensions();
                wayland_.request_frame_callback();
                return;
            }
            // Close button
            if (x >= cur_w - 36.0) {
                in_control_center_ = false;
                update_target_dimensions();
                wayland_.request_frame_callback();
                return;
            }
        }

        // Toggles (2x2 Grid)
        float chip_w = (cur_w - 32.0f - 10.0f) / 2.0f;
        float col1_x = 16.0f;
        float col2_x = 16.0f + chip_w + 10.0f;
        float row1_y = 44.0f;
        float row2_y = 82.0f;

        // Row 1: Wi-Fi (Col 1) and Bluetooth (Col 2)
        if (y >= row1_y && y <= row1_y + 34.0f) {
            if (x >= col1_x && x <= col1_x + chip_w) {
                if (cc_state_.wifi_enabled) {
                    int r = system("nmcli radio wifi off"); (void)r;
                    cc_state_.wifi_enabled = false;
                } else {
                    int r = system("nmcli radio wifi on"); (void)r;
                    cc_state_.wifi_enabled = true;
                }
                update_control_center_info();
                wayland_.request_frame_callback();
                return;
            } else if (x >= col2_x && x <= col2_x + chip_w) {
                if (cc_state_.bluetooth_enabled) {
                    int r = system("bluetoothctl power off"); (void)r;
                    cc_state_.bluetooth_enabled = false;
                } else {
                    int r = system("bluetoothctl power on"); (void)r;
                    cc_state_.bluetooth_enabled = true;
                }
                update_control_center_info();
                wayland_.request_frame_callback();
                return;
            }
        }

        // Row 2: Audio Mute (Col 1) and DND (Col 2)
        if (y >= row2_y && y <= row2_y + 34.0f) {
            if (x >= col1_x && x <= col1_x + chip_w) {
                osd_.toggle_mute();
                cc_state_.audio_muted = osd_.current_muted();
                wayland_.request_frame_callback();
                return;
            } else if (x >= col2_x && x <= col2_x + chip_w) {
                notifications_.toggle_dnd();
                cc_state_.dnd_enabled = notifications_.dnd();
                wayland_.request_frame_callback();
                return;
            }
        }

        // Volume Slider (y between 120 and 152)
        if (y >= 120.0 && y <= 152.0) {
            float bar_x = 16.0f;
            float bar_w = cur_w - 32.0f;
            float ratio = std::clamp((static_cast<float>(x) - bar_x) / bar_w, 0.0f, 1.0f);
            osd_.set_volume(ratio);
            cc_state_.volume = ratio;
            cc_state_.audio_muted = false;
            active_drag_ = DragTarget::ControlVolume;
            wayland_.request_frame_callback();
            return;
        }

        // Brightness Slider (y between 154 and 188)
        if (y >= 154.0 && y <= 188.0) {
            float bar_x = 16.0f;
            float bar_w = cur_w - 32.0f;
            float ratio = std::clamp((static_cast<float>(x) - bar_x) / bar_w, 0.0f, 1.0f);
            osd_.set_brightness(ratio);
            cc_state_.brightness = ratio;
            active_drag_ = DragTarget::ControlBrightness;
            wayland_.request_frame_callback();
            return;
        }

        // Footer Actions: Screenshot and Lock
        if (y >= 192.0 && y <= 228.0) {
            if (x >= cur_w - 76.0f && x <= cur_w - 42.0f) {
                int r = system("grim ~/Pictures/screenshot_$(date +%s).png &"); (void)r;
                return;
            } else if (x >= cur_w - 40.0f) {
                int r = system("noctalia msg session lock &"); (void)r;
                return;
            }
        }

        return; // Don't collapse inside Control Center
    }

    // 2. Music Expanded Interactions
    if (expanded_) {
        // Control Center button in top right
        if (x >= cur_w - 36.0f && y <= 36.0f) {
            expanded_ = false;
            in_control_center_ = true;
            update_control_center_info();
            update_target_dimensions();
            wayland_.request_frame_callback();
            return;
        }

        // Interactive seek on progress bar (generous hitbox: y between 60 and 86)
        if (y >= 60.0 && y <= 86.0) {
            float bar_x = 16.0f;
            float bar_w = cur_w - 32.0f;
            float ratio = std::clamp((static_cast<float>(x) - bar_x) / bar_w, 0.0f, 1.0f);
            mpris_.set_position_ratio(ratio);
            active_drag_ = DragTarget::MusicSeek;
            wayland_.request_frame_callback();
            return;
        }

        // Control buttons row: y around 90 - 126
        if (y > 88.0) {
            float mid_x = cur_w / 2.0f;
            if (x < mid_x - 25.0f) {
                mpris_.previous();
            } else if (x > mid_x + 25.0f) {
                mpris_.next();
            } else {
                mpris_.play_pause();
            }
            wayland_.request_frame_callback();
            return;
        }

        return; // Don't collapse on random interior clicks
    }

    // 3. Idle mode: Click opens Control Center
    if (current_mode_ == IslandMode::Idle) {
        in_control_center_ = true;
        update_control_center_info();
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }

    // 4. Music Compact: Click expands media card
    if (current_mode_ == IslandMode::MusicCompact) {
        expanded_ = true;
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }
}

void IslandApp::on_motion(double x, double y) {
    (void)y;
    if (active_drag_ == DragTarget::None) return;

    float cur_w = width_spring_.value;
    float bar_x = 16.0f;
    float bar_w = cur_w - 32.0f;
    float ratio = std::clamp((static_cast<float>(x) - bar_x) / bar_w, 0.0f, 1.0f);

    if (active_drag_ == DragTarget::MusicSeek) {
        mpris_.set_position_ratio(ratio);
        wayland_.request_frame_callback();
    } else if (active_drag_ == DragTarget::ControlVolume) {
        osd_.set_volume(ratio);
        cc_state_.volume = ratio;
        cc_state_.audio_muted = false;
        wayland_.request_frame_callback();
    } else if (active_drag_ == DragTarget::ControlBrightness) {
        osd_.set_brightness(ratio);
        cc_state_.brightness = ratio;
        wayland_.request_frame_callback();
    }
}

void IslandApp::on_frame() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last_frame_time_).count();
    last_frame_time_ = now;
    if (dt > 0.05f) dt = 0.05f;

    anim_time_ += dt;

    // Advance song position smoothly
    mpris_.tick(dt);

    // Refresh control center info if open
    if (in_control_center_) {
        update_control_center_info();
    }

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
                       osd_.state(), notifications_.current(), cc_state_,
                       osd_.is_mic_active(), anim_time_);
        wayland_.commit_frame(cur_w, cur_h);
    }

    // Continue frame loop if animations or interactions are active
    bool needs_next_frame = animating_w || animating_h || 
                           osd_.is_active() || notifications_.has_active() ||
                           (mpris_.info().active && mpris_.info().playing) ||
                           (active_drag_ != DragTarget::None);

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
                         (mpris_.info().active && mpris_.info().playing) ||
                         (active_drag_ != DragTarget::None) || in_control_center_;
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

        // Periodic clock update in idle mode
        if (current_mode_ == IslandMode::Idle && width_spring_.is_settled() && height_spring_.is_settled()) {
            wayland_.request_frame_callback();
        }
    }
}

} // namespace island
