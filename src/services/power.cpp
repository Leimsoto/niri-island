#include "services/power.hpp"

#include <cmath>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <poll.h>

#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"
#include "services/dbus.hpp"

namespace island {

static std::string read_line(const std::string& path) {
    std::string s;
    if (!read_file(path, s)) return "";
    return trim(s);
}

static double read_num(const std::string& path, double def = -1) {
    std::string s = read_line(path);
    if (s.empty()) return def;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

Power::~Power() {
    if (mon_) {
        Loop::get().unwatch(udev_monitor_get_fd(mon_));
        udev_monitor_unref(mon_);
    }
    if (udev_) udev_unref(udev_);
}

bool Power::init(sd_bus* system_bus) {
    sys_ = system_bus;
    find_devices();
    read_battery(false);
    read_backlight(false);

    udev_ = udev_new();
    if (udev_) {
        mon_ = udev_monitor_new_from_netlink(udev_, "udev");
        if (mon_) {
            udev_monitor_filter_add_match_subsystem_devtype(mon_, "backlight", nullptr);
            udev_monitor_filter_add_match_subsystem_devtype(mon_, "power_supply", nullptr);
            udev_monitor_enable_receiving(mon_);
            Loop::get().watch(udev_monitor_get_fd(mon_), POLLIN, [this](short) { udev_events(); });
        }
    }

    if (sys_) {
        // power-profiles-daemon: new name first, legacy name as fallback.
        const char* candidates[][3] = {
            {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
             "org.freedesktop.UPower.PowerProfiles"},
            {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"}};
        for (auto& c : candidates) {
            char* owner = nullptr;
            sd_bus_error err = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int r = sd_bus_call_method(sys_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                       "NameHasOwner", &err, &reply, "s", c[0]);
            int has = 0;
            if (r >= 0) sd_bus_message_read_basic(reply, 'b', &has);
            sd_bus_message_unref(reply);
            sd_bus_error_free(&err);
            (void)owner;
            if (has) {
                profiles_service_ = c[0];
                profiles_path_ = c[1];
                profiles_iface_ = c[2];
                has_profiles_ = true;
                break;
            }
        }
        if (has_profiles_) {
            sd_bus_match_signal(sys_, nullptr, profiles_service_.c_str(), profiles_path_.c_str(),
                                "org.freedesktop.DBus.Properties", "PropertiesChanged", on_profile_props, this);
            fetch_profile();
        }
    }
    return true;
}

void Power::find_devices() {
    DIR* d = opendir("/sys/class/power_supply");
    if (d) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n[0] == '.') continue;
            std::string p = "/sys/class/power_supply/" + n;
            std::string type = read_line(p + "/type");
            std::string scope = read_line(p + "/scope");
            if (type == "Battery" && scope != "Device" && bat_path_.empty()) bat_path_ = p;
            else if (type == "Mains" && ac_path_.empty()) ac_path_ = p;
        }
        closedir(d);
    }
    d = opendir("/sys/class/backlight");
    if (d) {
        // Prefer firmware/platform interfaces over raw, like logind does.
        int best = -1;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n[0] == '.') continue;
            std::string p = "/sys/class/backlight/" + n;
            std::string type = read_line(p + "/type");
            int score = type == "firmware" ? 3 : type == "platform" ? 2 : 1;
            if (score > best) {
                best = score;
                backlight_ = p;
                backlight_name_ = n;
            }
        }
        closedir(d);
    }
    if (!backlight_.empty()) max_brightness_ = std::max(1, static_cast<int>(read_num(backlight_ + "/max_brightness", 1)));
}

void Power::read_battery(bool notify) {
    BatteryState b;
    if (!ac_path_.empty()) b.plugged = read_num(ac_path_ + "/online", 0) > 0;
    if (!bat_path_.empty()) {
        b.present = read_num(bat_path_ + "/present", 1) > 0;
        b.percent = std::clamp(static_cast<int>(read_num(bat_path_ + "/capacity", 100)), 0, 100);
        b.status = read_line(bat_path_ + "/status");
        double v = read_num(bat_path_ + "/voltage_now", 0);
        double p = read_num(bat_path_ + "/power_now", -1);
        if (p < 0) {
            double c = read_num(bat_path_ + "/current_now", 0);
            p = c * v / 1e6;
        }
        b.watts = static_cast<float>(std::abs(p) / 1e6);
        double e = read_num(bat_path_ + "/energy_now", -1), ef = read_num(bat_path_ + "/energy_full", -1);
        double ed = read_num(bat_path_ + "/energy_full_design", -1);
        if (e < 0) {
            double c = read_num(bat_path_ + "/charge_now", -1), cf = read_num(bat_path_ + "/charge_full", -1);
            double cd = read_num(bat_path_ + "/charge_full_design", -1);
            if (c >= 0 && v > 0) e = c * v / 1e6;
            if (cf >= 0 && v > 0) ef = cf * v / 1e6;
            if (cd >= 0 && v > 0) ed = cd * v / 1e6;
        }
        b.energy_wh = static_cast<float>(std::max(0.0, e) / 1e6);
        b.energy_full_wh = static_cast<float>(std::max(0.0, ef) / 1e6);
        if (ef > 0 && ed > 0) b.health = static_cast<float>(std::min(1.0, ef / ed));
        if (b.status == "Charging" || b.status == "Discharging") b.plugged = b.plugged || b.status == "Charging";
        b.charging = b.status == "Charging";
        if (b.watts > 0.3f) {
            if (b.charging && b.energy_full_wh > b.energy_wh)
                b.minutes = static_cast<int>(std::lround((b.energy_full_wh - b.energy_wh) / b.watts * 60));
            else if (b.status == "Discharging" && b.energy_wh > 0)
                b.minutes = static_cast<int>(std::lround(b.energy_wh / b.watts * 60));
        }
    }
    bool plug_changed = b.plugged != bat_.plugged;
    bool changed = plug_changed || b.percent != bat_.percent || b.status != bat_.status;
    bat_ = b;
    if (notify && plug_changed && on_plug) on_plug(b.plugged);
    if (b.present && !b.plugged && b.status == "Discharging") {
        for (int lvl : {5, 10, 20}) {
            if (b.percent <= lvl && last_warned_ > lvl) {
                last_warned_ = lvl;
                if (notify && on_low) on_low(b.percent);
                break;
            }
        }
    } else if (b.plugged) {
        last_warned_ = 101;
    }
    if (changed && on_change) on_change();
}

void Power::read_backlight(bool notify) {
    if (backlight_.empty()) return;
    int cur = static_cast<int>(read_num(backlight_ + "/brightness", brightness_));
    if (cur == brightness_) return;
    bool first = brightness_ < 0;
    brightness_ = cur;
    if (notify && !first && on_brightness) on_brightness(now_mono() > ignore_brightness_until_);
    if (on_change) on_change();
}

void Power::udev_events() {
    bool bl = false, ps = false;
    while (udev_device* dev = udev_monitor_receive_device(mon_)) {
        const char* sub = udev_device_get_subsystem(dev);
        if (sub && strcmp(sub, "backlight") == 0) bl = true;
        if (sub && strcmp(sub, "power_supply") == 0) ps = true;
        udev_device_unref(dev);
    }
    if (bl) read_backlight(true);
    if (ps) read_battery(true);
}

void Power::poll() {
    read_battery(true);
    read_backlight(true);
}

float Power::brightness() const {
    if (brightness_ < 0) return 0.7f;
    return clamp01(static_cast<float>(brightness_) / max_brightness_);
}

void Power::set_brightness(float v) {
    if (backlight_.empty()) return;
    v = std::clamp(v, 0.01f, 1.0f);
    int target = std::max(1, static_cast<int>(std::lround(v * max_brightness_)));
    brightness_ = target;
    // Our own slider: the uevent that follows is not an OSD moment.
    ignore_brightness_until_ = now_mono() + 0.6;
    static int pending_pct = 0;
    pending_pct = static_cast<int>(std::lround(v * 100));
    auto fallback = [] {
        if (proc::has("brightnessctl")) proc::spawn({"brightnessctl", "-q", "set", std::to_string(pending_pct) + "%"});
    };
    if (sys_ && !logind_failed_) {
        // logind lets the session owner set the backlight without root.
        int r = sd_bus_call_method_async(
            sys_, nullptr, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
            "org.freedesktop.login1.Session", "SetBrightness",
            [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
                if (sd_bus_message_is_method_error(m, nullptr)) {
                    // Not inside a logind session (e.g. started as a user service).
                    static_cast<Power*>(ud)->logind_failed_ = true;
                    if (proc::has("brightnessctl"))
                        proc::spawn({"brightnessctl", "-q", "set", std::to_string(pending_pct) + "%"});
                }
                return 0;
            },
            this, "ssu", "backlight", backlight_name_.c_str(), static_cast<uint32_t>(target));
        if (r < 0) fallback();
    } else {
        fallback();
    }
    if (on_change) on_change();
}

void Power::change_brightness(float delta) { set_brightness(brightness() + delta); }

void Power::fetch_profile() {
    sd_bus_call_method_async(
        sys_, nullptr, profiles_service_.c_str(), profiles_path_.c_str(), "org.freedesktop.DBus.Properties", "Get",
        [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
            auto* self = static_cast<Power*>(ud);
            if (sd_bus_message_is_method_error(m, nullptr)) return 0;
            Value v;
            if (read_variant(m, v) >= 0 && v.kind == Value::Kind::Str) {
                self->profile_ = v.s;
                if (self->on_change) self->on_change();
            }
            return 0;
        },
        this, "ss", profiles_iface_.c_str(), "ActiveProfile");
}

int Power::on_profile_props(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Power*>(ud);
    const char* iface = nullptr;
    if (sd_bus_message_read_basic(m, 's', &iface) < 0) return 0;
    Dict props;
    if (read_dict(m, props) < 0) return 0;
    if (auto v = find(props, "ActiveProfile")) {
        self->profile_ = v->str(self->profile_);
        if (self->on_change) self->on_change();
    }
    return 0;
}

void Power::set_profile(const std::string& p) {
    if (!has_profiles_) {
        if (proc::has("powerprofilesctl")) proc::spawn({"powerprofilesctl", "set", p});
        profile_ = p;
        return;
    }
    profile_ = p;
    sd_bus_call_method_async(sys_, nullptr, profiles_service_.c_str(), profiles_path_.c_str(),
                             "org.freedesktop.DBus.Properties", "Set", nullptr, nullptr, "ssv",
                             profiles_iface_.c_str(), "ActiveProfile", "s", p.c_str());
    if (on_change) on_change();
}

void Power::cycle_profile() {
    set_profile(profile_ == "balanced" ? "performance" : profile_ == "performance" ? "power-saver" : "balanced");
}

}  // namespace island
