#include "island.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <cmath>

namespace island {

static bool check_rfkill_state(const char* target_type) {
    DIR* dir = opendir("/sys/class/rfkill");
    if (!dir) return false;
    struct dirent* ent;
    bool enabled = false;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "rfkill", 6) != 0) continue;
        char type_path[512];
        snprintf(type_path, sizeof(type_path), "/sys/class/rfkill/%s/type", ent->d_name);
        FILE* tf = fopen(type_path, "r");
        if (!tf) continue;
        char type_buf[32] = {0};
        if (fgets(type_buf, sizeof(type_buf), tf)) {
            size_t len = strlen(type_buf);
            while (len > 0 && (type_buf[len - 1] == '\n' || type_buf[len - 1] == '\r')) {
                type_buf[--len] = '\0';
            }
            if (strcmp(type_buf, target_type) == 0) {
                char state_path[512];
                snprintf(state_path, sizeof(state_path), "/sys/class/rfkill/%s/state", ent->d_name);
                FILE* sf = fopen(state_path, "r");
                if (sf) {
                    int st = 0;
                    if (fscanf(sf, "%d", &st) == 1 && st == 1) {
                        enabled = true;
                    }
                    fclose(sf);
                }
            }
        }
        fclose(tf);
        if (enabled) break;
    }
    closedir(dir);
    return enabled;
}

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

    workspaces_.init();
    ws_bubble_w_ = workspaces_.bubble_width();
    int initial_total_w = (int)std::round(ws_bubble_w_ + 8.0f + 110.0f);
    wayland_.create_surface(initial_total_w, 32);
    last_frame_time_ = std::chrono::steady_clock::now();

    // Set MPRIS callback
    mpris_.on_change = [this](const MediaInfo& info) {
        if (info.active && !info.title.empty()) {
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
        if (!in_control_center_) {
            update_target_dimensions();
        }
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

    if (!ipc_.start()) {
        std::cerr << "Failed to start IPC server (continuing without socket)\n";
    }

    update_target_dimensions();

    // Kick off first frame
    wayland_.request_frame_callback();

    return true;
}

void IslandApp::update_target_dimensions() {
    workspaces_.update();
    ws_bubble_w_ = workspaces_.bubble_width();

    bool has_media = mpris_.info().active && !mpris_.info().title.empty();

    if (in_control_center_) {
        width_spring_.set_target(400.0f);
        height_spring_.set_target(330.0f);
        current_mode_ = IslandMode::ControlCenter;
    } else if (notifications_.has_active()) {
        width_spring_.set_target(330.0f);
        height_spring_.set_target(70.0f);
        current_mode_ = IslandMode::Notification;
    } else if (osd_.is_active()) {
        width_spring_.set_target(210.0f);
        height_spring_.set_target(34.0f);
        current_mode_ = IslandMode::HudOSD;
    } else if (expanded_) {
        if (has_media) {
            width_spring_.set_target(330.0f);
            height_spring_.set_target(136.0f);
            current_mode_ = IslandMode::MusicExpanded;
        } else {
            width_spring_.set_target(280.0f);
            height_spring_.set_target(68.0f);
            current_mode_ = IslandMode::IdleExpanded;
        }
    } else if (has_media) {
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
    cc_state_.media_active = mpris_.info().active && !mpris_.info().title.empty();
    cc_state_.dnd_enabled = notifications_.dnd();

    // Update real-time CPU & RAM stats
    sysmon_.update();
    cc_state_.stats = sysmon_.stats();

    // Instantaneous non-blocking rfkill check (0ms latency!)
    cc_state_.wifi_enabled = check_rfkill_state("wlan");
    cc_state_.bluetooth_enabled = check_rfkill_state("bluetooth");

    // Wi-Fi SSID only needs to be checked every few seconds
    if (elapsed > 4.0f || cc_state_.wifi_ssid.empty()) {
        last_cc_poll_time_ = now;
        if (cc_state_.wifi_enabled) {
            FILE* fp = popen("nmcli -t -f NAME,TYPE c show --active 2>/dev/null | grep ':802-11-wireless$' | cut -d: -f1", "r");
            if (fp) {
                char buf[64] = {0};
                if (fgets(buf, sizeof(buf), fp)) {
                    std::string s = buf;
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                    cc_state_.wifi_ssid = s.empty() ? "Wi-Fi" : s;
                }
                pclose(fp);
            }
        } else {
            cc_state_.wifi_ssid = "Wi-Fi";
        }
    }
}

void IslandApp::on_hover(bool hovered) {
    if (notifications_.has_active() || osd_.is_active() || in_control_center_) return;
    if (active_drag_ != DragTarget::None) return;

    if (!hovered) {
        expanded_ = false;
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }

    float left_wing = (ws_bubble_w_ > 0.0f) ? (ws_bubble_w_ + 8.0f) : 0.0f;
    float wing = std::max(left_wing, 0.0f);
    float island_offset_x = wing;
    float cur_w = width_spring_.value;

    if (wayland_.pointer_x_ >= island_offset_x && wayland_.pointer_x_ <= island_offset_x + cur_w) {
        expanded_ = true;
    } else {
        expanded_ = false;
    }

    update_target_dimensions();
    wayland_.request_frame_callback();
}

void IslandApp::on_click(int button, int state, double x, double y) {
    float left_wing = (ws_bubble_w_ > 0.0f) ? (ws_bubble_w_ + 8.0f) : 0.0f;
    float right_wing = 0.0f;
    float wing = std::max(left_wing, right_wing);

    float ws_x = wing - 8.0f - ws_bubble_w_;
    float island_offset_x = wing;
    float cur_w = width_spring_.value;
    float cur_h = height_spring_.value;

    // 0. Workspaces Bubble interaction (Left satellite)
    if (ws_bubble_w_ > 0.0f && x >= ws_x && x < ws_x + ws_bubble_w_ && y <= 32.0) {
        if (button == 272 && state == 1) { // Left Click
            const auto& ws_list = workspaces_.workspaces();
            if (!ws_list.empty()) {
                float per_ws = ws_bubble_w_ / static_cast<float>(ws_list.size());
                int ws_idx = std::clamp(static_cast<int>((x - ws_x) / per_ws), 0, static_cast<int>(ws_list.size()) - 1);
                workspaces_.focus_workspace(ws_list[ws_idx].idx);
                wayland_.request_frame_callback();
            }
        }
        return;
    }

    if (x < island_offset_x || x > island_offset_x + cur_w || y > cur_h) {
        return; // Outside main island
    }

    double cur_x = x - island_offset_x;

    // Right Click handler
    if (button == 273 && state == 1) {
        if (in_control_center_) {
            // Check if right click was on Wi-Fi or Bluetooth chips!
            float chip_w = (cur_w - 40.0f - 12.0f) / 2.0f;
            float chip_h = 38.0f;
            float col1_x = 20.0f;
            float col2_x = 20.0f + chip_w + 12.0f;
            float row1_y = 90.0f;

            if (y >= row1_y && y <= row1_y + chip_h) {
                if (cur_x >= col1_x && cur_x <= col1_x + chip_w) {
                    // Right click on Wi-Fi: open network manager / nmtui
                    int r = system("ghostty -e nmtui & || nm-connection-editor &"); (void)r;
                    return;
                } else if (cur_x >= col2_x && cur_x <= col2_x + chip_w) {
                    // Right click on Bluetooth: open bluetoothctl / blueman
                    int r = system("ghostty -e bluetoothctl & || blueman-manager &"); (void)r;
                    return;
                }
            }
        }

        // Otherwise right click toggles Control Center
        in_control_center_ = !in_control_center_;
        if (in_control_center_) {
            expanded_ = false;
            update_control_center_info();
        }
        update_target_dimensions();
        wayland_.request_frame_callback();
        return;
    }

    if (button != 272) return;

    // Button release: finish drag and release tactile press
    if (state == 0) {
        active_drag_ = DragTarget::None;
        volume_press_spring_.set_target(0.0f);
        volume_bounce_spring_.set_target(0.0f);
        brightness_press_spring_.set_target(0.0f);
        brightness_bounce_spring_.set_target(0.0f);
        wayland_.request_frame_callback();
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

    // 1. Control Center Interactions
    if (in_control_center_) {
        // Header actions (y <= 44.0)
        if (y <= 44.0) {
            // Close button (far right: cur_w - 42)
            if (cur_x >= cur_w - 46.0f) {
                in_control_center_ = false;
                update_target_dimensions();
                wayland_.request_frame_callback();
                return;
            }
            // Lock button (cur_w - 76)
            if (cur_x >= cur_w - 80.0f && cur_x <= cur_w - 50.0f) {
                int r = system("swaylock -f 2>/dev/null || loginctl lock-session &"); (void)r;
                return;
            }
            // Screenshot button (cur_w - 110)
            if (cur_x >= cur_w - 114.0f && cur_x <= cur_w - 84.0f) {
                int r = system("mkdir -p ~/Imágenes/Screenshots && grim ~/Imágenes/Screenshots/screenshot_$(date +%s).png &"); (void)r;
                return;
            }
            // Media toggle button (if media active: cur_w - 144)
            if (cc_state_.media_active && cur_x >= cur_w - 148.0f && cur_x <= cur_w - 118.0f) {
                in_control_center_ = false;
                expanded_ = true;
                update_target_dimensions();
                wayland_.request_frame_callback();
                return;
            }
        }

        // Toggles (2x2 Grid)
        float chip_w = (cur_w - 40.0f - 12.0f) / 2.0f;
        float chip_h = 38.0f;
        float col1_x = 20.0f;
        float col2_x = 20.0f + chip_w + 12.0f;
        float row1_y = 90.0f;
        float row2_y = 136.0f;

        // Row 1: Wi-Fi (Col 1) and Bluetooth (Col 2)
        if (y >= row1_y && y <= row1_y + chip_h) {
            if (cur_x >= col1_x && cur_x <= col1_x + chip_w) {
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
            } else if (cur_x >= col2_x && cur_x <= col2_x + chip_w) {
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
        if (y >= row2_y && y <= row2_y + chip_h) {
            if (cur_x >= col1_x && cur_x <= col1_x + chip_w) {
                osd_.toggle_mute();
                cc_state_.audio_muted = osd_.current_muted();
                wayland_.request_frame_callback();
                return;
            } else if (cur_x >= col2_x && cur_x <= col2_x + chip_w) {
                notifications_.toggle_dnd();
                cc_state_.dnd_enabled = notifications_.dnd();
                wayland_.request_frame_callback();
                return;
            }
        }

        // Volume Slider (continuous generous hitbox: y between 176 and 228, full width)
        if (y >= 176.0 && y < 228.0) {
            float bar_x = 20.0f;
            float bar_w = cur_w - 40.0f;
            float raw_ratio = (static_cast<float>(cur_x) - bar_x) / bar_w;
            float ratio = std::clamp(raw_ratio, 0.0f, 1.0f);
            osd_.set_volume(ratio);
            cc_state_.volume = ratio;
            cc_state_.audio_muted = false;
            active_drag_ = DragTarget::ControlVolume;
            volume_press_spring_.set_target(1.0f);
            if (raw_ratio > 1.0f) {
                volume_bounce_spring_.value = 8.0f;
                volume_bounce_spring_.set_target(0.0f);
            } else if (raw_ratio < 0.0f) {
                volume_bounce_spring_.value = -8.0f;
                volume_bounce_spring_.set_target(0.0f);
            }
            wayland_.request_frame_callback();
            return;
        }

        // Brightness Slider (continuous generous hitbox: y between 228 and 280, full width)
        if (y >= 228.0 && y <= 280.0) {
            float bar_x = 20.0f;
            float bar_w = cur_w - 40.0f;
            float raw_ratio = (static_cast<float>(cur_x) - bar_x) / bar_w;
            float ratio = std::clamp(raw_ratio, 0.0f, 1.0f);
            osd_.set_brightness(ratio);
            cc_state_.brightness = ratio;
            active_drag_ = DragTarget::ControlBrightness;
            brightness_press_spring_.set_target(1.0f);
            if (raw_ratio > 1.0f) {
                brightness_bounce_spring_.value = 8.0f;
                brightness_bounce_spring_.set_target(0.0f);
            } else if (raw_ratio < 0.0f) {
                brightness_bounce_spring_.value = -8.0f;
                brightness_bounce_spring_.set_target(0.0f);
            }
            wayland_.request_frame_callback();
            return;
        }

        return; // Don't collapse inside Control Center
    }

    // 2. Music Expanded Interactions
    if (expanded_) {
        // Control Center button in top right
        if (cur_x >= cur_w - 36.0f && y <= 36.0f) {
            expanded_ = false;
            in_control_center_ = true;
            update_control_center_info();
            update_target_dimensions();
            wayland_.request_frame_callback();
            return;
        }

        // Interactive seek on progress bar
        if (y >= 60.0 && y <= 86.0) {
            float bar_x = 16.0f;
            float bar_w = cur_w - 32.0f;
            float ratio = std::clamp((static_cast<float>(cur_x) - bar_x) / bar_w, 0.0f, 1.0f);
            mpris_.set_position_ratio(ratio);
            active_drag_ = DragTarget::MusicSeek;
            wayland_.request_frame_callback();
            return;
        }

        // Control buttons row: y around 90 - 126
        if (y > 88.0) {
            float mid_x = cur_w / 2.0f;
            if (cur_x < mid_x - 25.0f) {
                mpris_.previous();
            } else if (cur_x > mid_x + 25.0f) {
                mpris_.next();
            } else {
                mpris_.play_pause();
            }
            wayland_.request_frame_callback();
            return;
        }

        return;
    }

    // 3. Idle mode: Click opens Control Center
    if (current_mode_ == IslandMode::Idle || current_mode_ == IslandMode::IdleExpanded) {
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
    float left_wing = (ws_bubble_w_ > 0.0f) ? (ws_bubble_w_ + 8.0f) : 0.0f;
    float right_wing = 0.0f;
    float wing = std::max(left_wing, right_wing);
    float island_offset_x = wing;
    float cur_w = width_spring_.value;
    float cur_x = static_cast<float>(x) - island_offset_x;

    // Handle hover transition between satellite and main island
    if (!notifications_.has_active() && !osd_.is_active() && !in_control_center_ && active_drag_ == DragTarget::None) {
        bool over_island = (x >= island_offset_x && x <= island_offset_x + cur_w && y <= height_spring_.value);
        if (over_island != expanded_) {
            expanded_ = over_island;
            update_target_dimensions();
            wayland_.request_frame_callback();
        }
    }

    if (active_drag_ == DragTarget::None) return;
    (void)y;

    if (active_drag_ == DragTarget::MusicSeek) {
        float bar_x = 16.0f;
        float bar_w = cur_w - 32.0f;
        float ratio = std::clamp((cur_x - bar_x) / bar_w, 0.0f, 1.0f);
        mpris_.set_position_ratio(ratio);
        wayland_.request_frame_callback();
    } else if (active_drag_ == DragTarget::ControlVolume) {
        float bar_x = 20.0f;
        float bar_w = cur_w - 40.0f;
        float raw_ratio = (cur_x - bar_x) / bar_w;
        float ratio = std::clamp(raw_ratio, 0.0f, 1.0f);
        if (raw_ratio > 1.0f) {
            volume_bounce_spring_.set_target(std::min(12.0f, (raw_ratio - 1.0f) * 20.0f));
        } else if (raw_ratio < 0.0f) {
            volume_bounce_spring_.set_target(std::max(-12.0f, raw_ratio * 20.0f));
        } else {
            volume_bounce_spring_.set_target(0.0f);
        }
        osd_.set_volume(ratio);
        cc_state_.volume = ratio;
        cc_state_.audio_muted = false;
        wayland_.request_frame_callback();
    } else if (active_drag_ == DragTarget::ControlBrightness) {
        float bar_x = 20.0f;
        float bar_w = cur_w - 40.0f;
        float raw_ratio = (cur_x - bar_x) / bar_w;
        float ratio = std::clamp(raw_ratio, 0.0f, 1.0f);
        if (raw_ratio > 1.0f) {
            brightness_bounce_spring_.set_target(std::min(12.0f, (raw_ratio - 1.0f) * 20.0f));
        } else if (raw_ratio < 0.0f) {
            brightness_bounce_spring_.set_target(std::max(-12.0f, raw_ratio * 20.0f));
        } else {
            brightness_bounce_spring_.set_target(0.0f);
        }
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

    // Refresh workspaces
    workspaces_.update();
    ws_bubble_w_ = workspaces_.bubble_width();

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
    bool animating_vp = volume_press_spring_.update(dt);
    bool animating_vb = volume_bounce_spring_.update(dt);
    bool animating_bp = brightness_press_spring_.update(dt);
    bool animating_bb = brightness_bounce_spring_.update(dt);

    cc_state_.volume_press = volume_press_spring_.value;
    cc_state_.volume_bounce = volume_bounce_spring_.value;
    cc_state_.brightness_press = brightness_press_spring_.value;
    cc_state_.brightness_bounce = brightness_bounce_spring_.value;

    int island_w = std::max(20, (int)std::round(width_spring_.value));
    float left_wing = (ws_bubble_w_ > 0.0f) ? (ws_bubble_w_ + 8.0f) : 0.0f;
    float right_wing = 0.0f;
    float wing = std::max(left_wing, right_wing);

    int cur_w = (int)std::round(island_w + 2.0f * wing);
    int cur_h = std::max(32, (int)std::round(height_spring_.value));

    // Update Wayland surface size & input regions
    wayland_.resize_surface(cur_w, cur_h);

    std::vector<InputRect> input_rects;
    if (ws_bubble_w_ > 0.0f) {
        int ws_x = (int)std::round(wing - 8.0f - ws_bubble_w_);
        input_rects.push_back({ws_x, 0, (int)std::round(ws_bubble_w_), 32});
    }
    input_rects.push_back({(int)std::round(wing), 0, island_w, cur_h});
    wayland_.set_input_regions(input_rects);

    cairo_t* cr = wayland_.get_cairo_context(cur_w, cur_h);
    if (cr) {
        renderer_.draw(cr, cur_w, cur_h, current_mode_, mpris_.info(), 
                       osd_.state(), notifications_.current(), cc_state_,
                       osd_.is_mic_active(), anim_time_,
                       ws_bubble_w_, workspaces_.workspaces(), workspaces_.active_idx());
        wayland_.commit_frame(cur_w, cur_h);
    }

    // Continue frame loop if animations or interactions are active
    bool needs_next_frame = animating_w || animating_h || 
                           animating_vp || animating_vb || animating_bp || animating_bb ||
                           osd_.is_active() || notifications_.has_active() ||
                           (mpris_.info().active && !mpris_.info().title.empty() && mpris_.info().playing) ||
                           (active_drag_ != DragTarget::None);

    if (needs_next_frame) {
        wayland_.request_frame_callback();
    }
}

void IslandApp::run() {
    struct pollfd fds[6];
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

    int ipc_idx = -1;
    if (ipc_.get_fd() >= 0) {
        ipc_idx = n_fds;
        fds[n_fds].fd = ipc_.get_fd();
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
                         (mpris_.info().active && !mpris_.info().title.empty() && mpris_.info().playing) ||
                         (active_drag_ != DragTarget::None) || in_control_center_ ||
                         (current_mode_ == IslandMode::IdleExpanded);
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

        if (ipc_idx >= 0 && (fds[ipc_idx].revents & POLLIN)) {
            ipc_.process_pending([this](const std::string& cmd) -> std::string {
                return handle_ipc_command(cmd);
            });
        }

        // Handle Mode expiry
        if ((current_mode_ == IslandMode::Notification && !notifications_.has_active()) ||
            (current_mode_ == IslandMode::HudOSD && !osd_.is_active())) {
            update_target_dimensions();
            wayland_.request_frame_callback();
        }

        // Periodic clock update in idle mode or idle expanded mode
        if ((current_mode_ == IslandMode::Idle || current_mode_ == IslandMode::IdleExpanded) &&
            width_spring_.is_settled() && height_spring_.is_settled()) {
            wayland_.request_frame_callback();
        }
    }
}

std::string IslandApp::handle_ipc_command(const std::string& cmd) {
    if (cmd == "toggle-control-center" || cmd == "cc") {
        in_control_center_ = !in_control_center_;
        if (in_control_center_) {
            expanded_ = false;
            update_control_center_info();
        }
        update_target_dimensions();
        wayland_.request_frame_callback();
        return in_control_center_ ? "Control Center opened\n" : "Control Center closed\n";
    } else if (cmd == "toggle-media" || cmd == "expand") {
        expanded_ = !expanded_;
        in_control_center_ = false;
        update_target_dimensions();
        wayland_.request_frame_callback();
        return expanded_ ? "Expanded\n" : "Collapsed\n";
    } else if (cmd == "play-pause") {
        mpris_.play_pause();
        return "Toggled play/pause\n";
    } else if (cmd == "next") {
        mpris_.next();
        return "Skipped to next\n";
    } else if (cmd == "prev" || cmd == "previous") {
        mpris_.previous();
        return "Skipped to previous\n";
    } else if (cmd == "toggle-mute") {
        osd_.toggle_mute();
        cc_state_.audio_muted = osd_.current_muted();
        wayland_.request_frame_callback();
        return cc_state_.audio_muted ? "Muted\n" : "Unmuted\n";
    } else if (cmd == "toggle-wifi") {
        if (cc_state_.wifi_enabled) {
            int r = system("nmcli radio wifi off"); (void)r;
            cc_state_.wifi_enabled = false;
        } else {
            int r = system("nmcli radio wifi on"); (void)r;
            cc_state_.wifi_enabled = true;
        }
        wayland_.request_frame_callback();
        return cc_state_.wifi_enabled ? "Wi-Fi on\n" : "Wi-Fi off\n";
    } else if (cmd == "toggle-bluetooth" || cmd == "toggle-bt") {
        if (cc_state_.bluetooth_enabled) {
            int r = system("bluetoothctl power off"); (void)r;
            cc_state_.bluetooth_enabled = false;
        } else {
            int r = system("bluetoothctl power on"); (void)r;
            cc_state_.bluetooth_enabled = true;
        }
        wayland_.request_frame_callback();
        return cc_state_.bluetooth_enabled ? "Bluetooth on\n" : "Bluetooth off\n";
    } else if (cmd == "toggle-dnd") {
        notifications_.toggle_dnd();
        cc_state_.dnd_enabled = notifications_.dnd();
        wayland_.request_frame_callback();
        return cc_state_.dnd_enabled ? "DND on\n" : "DND off\n";
    } else if (cmd == "close") {
        if (in_control_center_ || expanded_) {
            in_control_center_ = false;
            expanded_ = false;
            update_target_dimensions();
            wayland_.request_frame_callback();
            return "Closed panel\n";
        }
        return "Already closed\n";
    } else if (cmd == "status") {
        std::ostringstream ss;
        ss << "Status: " << (in_control_center_ ? "Control Center" : (expanded_ ? "Music Expanded" : (mpris_.info().active ? "Music Compact" : "Idle"))) << "\n";
        ss << "Media: " << (mpris_.info().active ? (mpris_.info().title + " - " + mpris_.info().artist) : "None") << "\n";
        ss << "Volume: " << static_cast<int>(std::round(cc_state_.volume * 100)) << "%\n";
        ss << "Battery: " << cc_state_.battery_percent << "%\n";
        ss << "CPU: " << static_cast<int>(std::round(cc_state_.stats.cpu_percent)) << "%\n";
        ss << "RAM: " << static_cast<int>(std::round(cc_state_.stats.ram_percent)) << "%\n";
        return ss.str();
    }
    return "Unknown command: " + cmd + "\n";
}

} // namespace island
