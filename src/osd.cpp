#include "osd.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <unistd.h>

namespace island {

namespace fs = std::filesystem;

OsdManager::OsdManager() = default;

OsdManager::~OsdManager() {
    if (pa_ctx_) {
        pa_context_disconnect(pa_ctx_);
        pa_context_unref(pa_ctx_);
        pa_ctx_ = nullptr;
    }
    if (pa_mainloop_) {
        pa_threaded_mainloop_stop(pa_mainloop_);
        pa_threaded_mainloop_free(pa_mainloop_);
        pa_mainloop_ = nullptr;
    }
    if (udev_mon_) {
        udev_monitor_unref(udev_mon_);
        udev_mon_ = nullptr;
    }
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
}

void OsdManager::pa_state_cb(pa_context* c, void* userdata) {
    auto* self = static_cast<OsdManager*>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(c, pa_subscribe_cb, self);
        pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK, nullptr, nullptr);
        // Query initial volume
        pa_context_get_sink_info_by_name(c, "@DEFAULT_SINK@", pa_sink_info_cb, self);
    }
}

void OsdManager::pa_subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* userdata) {
    auto* self = static_cast<OsdManager*>(userdata);
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
        pa_context_get_sink_info_by_index(c, idx, pa_sink_info_cb, self);
    }
}

void OsdManager::pa_sink_info_cb(pa_context*, const pa_sink_info* i, int eol, void* userdata) {
    auto* self = static_cast<OsdManager*>(userdata);
    if (eol > 0 || !i) return;

    float vol = static_cast<float>(pa_cvolume_avg(&i->volume)) / static_cast<float>(PA_VOLUME_NORM);
    bool muted = (i->mute != 0);

    self->on_pulse_volume(vol, muted);
}

void OsdManager::on_pulse_volume(float volume, bool muted) {
    if (!initial_vol_received_) {
        // Just record initial volume on launch without showing HUD
        initial_vol_received_ = true;
        state_.value = volume;
        state_.muted = muted;
        return;
    }

    // Ignore identical state updates if any
    if (std::abs(state_.value - volume) < 0.005f && state_.muted == muted && state_.type == OsdType::Volume) {
        return;
    }

    state_.type = OsdType::Volume;
    state_.value = volume;
    state_.muted = muted;
    state_.last_change = std::chrono::steady_clock::now();

    if (on_osd_change) {
        on_osd_change(state_);
    }
}

void OsdManager::init_backlight() {
    std::string sys_backlight = "/sys/class/backlight";
    if (!fs::exists(sys_backlight)) return;

    for (const auto& entry : fs::directory_iterator(sys_backlight)) {
        if (entry.is_directory() || entry.is_symlink()) {
            backlight_path_ = entry.path().string();
            break;
        }
    }

    if (backlight_path_.empty()) return;

    // Read max brightness
    std::ifstream max_file(backlight_path_ + "/max_brightness");
    if (max_file.is_open()) {
        max_file >> max_brightness_;
        if (max_brightness_ <= 0) max_brightness_ = 100;
    }

    // Read current brightness
    std::ifstream cur_file(backlight_path_ + "/brightness");
    if (cur_file.is_open()) {
        cur_file >> last_brightness_;
    }
}

void OsdManager::read_brightness() {
    if (backlight_path_.empty()) return;

    int current = 0;
    std::ifstream cur_file(backlight_path_ + "/brightness");
    if (cur_file.is_open() && (cur_file >> current)) {
        if (current != last_brightness_) {
            last_brightness_ = current;
            float ratio = static_cast<float>(current) / static_cast<float>(max_brightness_);
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;

            state_.type = OsdType::Brightness;
            state_.value = ratio;
            state_.muted = false;
            state_.last_change = std::chrono::steady_clock::now();

            if (on_osd_change) {
                on_osd_change(state_);
            }
        }
    }
}

void OsdManager::process_udev_events() {
    if (!udev_mon_) return;

    struct udev_device* dev = nullptr;
    bool had_event = false;
    while ((dev = udev_monitor_receive_device(udev_mon_)) != nullptr) {
        had_event = true;
        udev_device_unref(dev);
    }

    if (had_event) {
        read_brightness();
    }
}

bool OsdManager::init() {
    // 1. Init PulseAudio
    pa_mainloop_ = pa_threaded_mainloop_new();
    if (!pa_mainloop_) {
        std::cerr << "[OSD] Failed to create PulseAudio mainloop\n";
    } else {
        pa_mainloop_api* api = pa_threaded_mainloop_get_api(pa_mainloop_);
        pa_ctx_ = pa_context_new(api, "niri-island");
        if (pa_ctx_) {
            pa_context_set_state_callback(pa_ctx_, pa_state_cb, this);
            pa_threaded_mainloop_lock(pa_mainloop_);
            pa_threaded_mainloop_start(pa_mainloop_);
            pa_context_connect(pa_ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
            pa_threaded_mainloop_unlock(pa_mainloop_);
        }
    }

    // 2. Init Udev Backlight
    init_backlight();

    udev_ = udev_new();
    if (udev_) {
        udev_mon_ = udev_monitor_new_from_netlink(udev_, "udev");
        if (udev_mon_) {
            udev_monitor_filter_add_match_subsystem_devtype(udev_mon_, "backlight", nullptr);
            udev_monitor_enable_receiving(udev_mon_);
            udev_fd_ = udev_monitor_get_fd(udev_mon_);
        }
    }

    initialized_ = true;
    return true;
}

} // namespace island
