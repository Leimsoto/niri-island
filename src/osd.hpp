#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <pulse/pulseaudio.h>
#include <libudev.h>

namespace island {

enum class OsdType {
    Volume,
    Brightness,
    Battery
};

struct OsdState {
    OsdType type = OsdType::Volume;
    float value = 0.0f; // 0.0 to 1.0
    bool muted = false;
    bool is_charging = false;
    std::string custom_text;
    std::chrono::steady_clock::time_point last_change;

    bool is_active(float timeout_sec = 2.0f) const {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - last_change).count();
        return elapsed < timeout_sec;
    }
};

class OsdManager {
public:
    OsdManager();
    ~OsdManager();

    bool init();

    int get_udev_fd() const { return udev_fd_; }
    void process_udev_events();

    const OsdState& state() const { return state_; }
    bool is_active() const { return state_.is_active(); }
    bool is_mic_active() const { return mic_active_; }

    float current_volume() const { return current_volume_; }
    bool current_muted() const { return current_muted_; }
    float current_brightness() const;
    int current_battery_pct() const { return last_battery_pct_ >= 0 ? last_battery_pct_ : 100; }
    bool current_battery_charging() const { return last_ac_online_ == 1; }

    void set_volume(float vol);
    void toggle_mute();
    void set_brightness(float brightness);

    std::function<void(const OsdState& state)> on_osd_change;
    std::function<void(bool active)> on_mic_change;

private:
    OsdState state_;
    bool initialized_ = false;
    bool initial_vol_received_ = false;
    bool mic_active_ = false;
    float current_volume_ = 0.5f;
    bool current_muted_ = false;

    // PulseAudio (Volume + Mic Monitoring)
    pa_threaded_mainloop* pa_mainloop_ = nullptr;
    pa_context* pa_ctx_ = nullptr;

    static void pa_state_cb(pa_context* c, void* userdata);
    static void pa_subscribe_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* userdata);
    static void pa_sink_info_cb(pa_context* c, const pa_sink_info* i, int eol, void* userdata);
    static void pa_source_output_cb(pa_context* c, const pa_source_output_info* i, int eol, void* userdata);

    void on_pulse_volume(float volume, bool muted);
    void check_active_sources();

    // Udev (Brightness + Battery / Power Supply)
    struct udev* udev_ = nullptr;
    struct udev_monitor* udev_mon_ = nullptr;
    int udev_fd_ = -1;

    std::string backlight_path_;
    int max_brightness_ = 100;
    int last_brightness_ = -1;

    std::string battery_path_;
    std::string ac_path_;
    int last_ac_online_ = -1;
    int last_battery_pct_ = -1;

    void init_backlight();
    void read_brightness();

    void init_power_supply();
    void check_power_supply();
};

} // namespace island
