#pragma once

#include <cstdint>

#include <functional>
#include <libudev.h>
#include <string>
#include <systemd/sd-bus.h>

namespace island {

struct BatteryState {
    bool present = false;
    int percent = 100;
    bool charging = false;     // AC connected and not full
    bool plugged = false;      // AC connected
    std::string status;        // Charging, Discharging, Full, Not charging
    float watts = 0;
    float energy_wh = 0, energy_full_wh = 0;
    int minutes = -1;          // to full or to empty
    float health = -1;         // energy_full / energy_full_design
};

class Power {
public:
    ~Power();
    bool init(sd_bus* system_bus);

    const BatteryState& battery() const { return bat_; }
    float brightness() const;
    bool has_backlight() const { return !backlight_.empty(); }
    void set_brightness(float v);
    void change_brightness(float delta);

    const std::string& profile() const { return profile_; }
    bool has_profiles() const { return has_profiles_; }
    void set_profile(const std::string& p);
    void cycle_profile();

    // Re-reads sysfs (battery telemetry drifts without uevents).
    void poll();

    std::function<void(bool plugged)> on_plug;        // AC connected / removed
    std::function<void(int percent)> on_low;          // crossed 20 / 10 / 5 %
    std::function<void(bool user)> on_brightness;     // backlight changed
    std::function<void()> on_change;

private:
    sd_bus* sys_ = nullptr;
    udev* udev_ = nullptr;
    udev_monitor* mon_ = nullptr;
    std::string bat_path_, ac_path_, backlight_, backlight_name_;
    int max_brightness_ = 1;
    int brightness_ = -1;
    double ignore_brightness_until_ = 0;
    bool logind_failed_ = false;
    BatteryState bat_;
    int last_warned_ = 101;
    std::string profile_ = "balanced";
    bool has_profiles_ = false;
    std::string profiles_service_, profiles_path_, profiles_iface_;

    void find_devices();
    void read_battery(bool notify);
    void read_backlight(bool notify);
    void udev_events();
    void fetch_profile();
    static int on_profile_props(sd_bus_message* m, void* ud, sd_bus_error*);
};

}  // namespace island
