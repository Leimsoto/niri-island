#include <algorithm>
#include "core/config.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

#include "core/util.hpp"

namespace island {

using json = nlohmann::json;

Config& config() {
    static Config c;
    return c;
}

std::string expand_home(const std::string& p) {
    if (starts_with(p, "~/")) return home_dir() + p.substr(1);
    return p;
}

std::string Config::path() { return config_dir() + "/config.json"; }

template <typename T>
static void get(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (...) {
        std::cerr << "[config] ignoring invalid value for '" << key << "'\n";
    }
}

void Config::load() {
    std::string text;
    if (!read_file(path(), text)) {
        *this = Config{};
        make_dirs(config_dir());
        write_file_atomic(path(), to_json());
        std::cout << "[config] wrote defaults to " << path() << "\n";
        return;
    }
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "[config] " << path() << " is not valid JSON, using defaults\n";
        return;
    }
    Config c;
    if (auto a = j.find("appearance"); a != j.end() && a->is_object()) {
        get(*a, "theme", c.theme);
        get(*a, "font", c.font);
        get(*a, "mono_font", c.mono_font);
        get(*a, "note_font", c.note_font);
        get(*a, "palette_file", c.palette_file);
        get(*a, "wallpaper_file", c.wallpaper_file);
        get(*a, "margin_top", c.margin_top);
        get(*a, "clock_24h", c.clock_24h);
        get(*a, "show_workspaces", c.show_workspaces);
        get(*a, "show_status", c.show_status);
        get(*a, "battery_percent", c.battery_percent);
        get(*a, "perf_metrics", c.perf_metrics);
        get(*a, "perf_style", c.perf_style);
        get(*a, "perf_split", c.perf_split);
        if (auto m = a->find("modules"); m != a->end() && m->is_array()) {
            std::vector<ModuleConf> mods;
            for (auto& e : *m) {
                if (!e.is_object() || !e.contains("id")) continue;
                ModuleConf mc;
                mc.id = e.value("id", "");
                mc.side = e.value("side", std::string("right")) == "left" ? "left" : "right";
                mc.on = e.value("on", true);
                mods.push_back(mc);
            }
            // modules added in newer versions show up too
            for (auto& d : Config::default_modules())
                if (std::none_of(mods.begin(), mods.end(), [&](const ModuleConf& x) { return x.id == d.id; })) mods.push_back(d);
            c.modules = mods;
        } else {
            // from the older show_* switches
            for (auto& mc : c.modules) {
                if (mc.id == "workspaces") mc.on = c.show_workspaces;
                else if (mc.id == "network" || mc.id == "bluetooth" || mc.id == "volume" || mc.id == "battery") mc.on = c.show_status;
            }
        }
        get(*a, "bar_layout", c.bar_layout);
        get(*a, "island_style", c.island_style);
        get(*a, "dock_sides", c.dock_sides);
        get(*a, "roundness", c.roundness);
        get(*a, "bar_height", c.bar_height);
        get(*a, "shadows", c.shadows);
        get(*a, "shadow_opacity", c.shadow_opacity);
        get(*a, "shadow_softness", c.shadow_softness);
        get(*a, "shadow_distance", c.shadow_distance);
        get(*a, "shadow_angle", c.shadow_angle);
        get(*a, "reserve_space", c.reserve_space);
        get(*a, "window_gap", c.window_gap);
        get(*a, "glass", c.glass);
        get(*a, "island_opacity", c.island_opacity);
        get(*a, "compositor_blur", c.compositor_blur);
        get(*a, "blur_xray", c.blur_xray);
        get(*a, "motion", c.motion);
        get(*a, "glance_delay_ms", c.glance_delay_ms);
    }
    if (auto n = j.find("notifications"); n != j.end() && n->is_object()) {
        get(*n, "mode", c.notification_mode);
        get(*n, "timeout_ms", c.notification_timeout_ms);
        get(*n, "history", c.notification_history);
    }
    if (auto m = j.find("commands"); m != j.end() && m->is_object()) {
        get(*m, "lock", c.cmd_lock);
        get(*m, "logout", c.cmd_logout);
        get(*m, "suspend", c.cmd_suspend);
        get(*m, "reboot", c.cmd_reboot);
        get(*m, "poweroff", c.cmd_poweroff);
        get(*m, "night_light", c.cmd_night_light);
        get(*m, "wallpaper_next", c.cmd_wallpaper_next);
        get(*m, "wallpaper_picker", c.cmd_wallpaper_picker);
        get(*m, "wallpaper_apply", c.cmd_wallpaper_apply);
        get(*m, "settings", c.cmd_settings);
        get(*m, "wallpaper_picker_style", c.wallpaper_picker_style);
        get(*m, "wallpaper_pause_covered", c.wallpaper_pause_covered);
        get(*m, "wallpaper_engine", c.wallpaper_engine);
        get(*m, "wallpaper_engine_audio", c.wallpaper_engine_audio);
        get(*m, "wallpaper_engine_clean", c.wallpaper_engine_clean);
        get(*m, "wallpaper_engine_fps", c.wallpaper_engine_fps);
        get(*m, "wallpaper_engine_quality", c.wallpaper_engine_quality);
        get(*m, "screensaver", c.screensaver);
        get(*m, "screensaver_minutes", c.screensaver_minutes);
        get(*m, "screensaver_lock_minutes", c.screensaver_lock_minutes);
        get(*m, "screensaver_bubbles", c.screensaver_bubbles);
        get(*m, "screensaver_speed", c.screensaver_speed);
        get(*m, "terminal", c.terminal);
    }
    if (auto su = j.find("suite"); su != j.end() && su->is_object()) {
        get(*su, "wallpaper_dirs", c.wallpaper_dirs);
        get(*su, "screenshot_dir", c.screenshot_dir);
        get(*su, "wallhaven_dir", c.wallhaven_dir);
        get(*su, "wallhaven_key", c.wallhaven_key);
        get(*su, "wallhaven_purity", c.wallhaven_purity);
        get(*su, "recording_dir", c.recording_dir);
        get(*su, "screenshot_copy", c.screenshot_copy);
    }
    if (auto w = j.find("weather"); w != j.end() && w->is_object()) {
        get(*w, "cache", c.weather_cache);
        get(*w, "latitude", c.latitude);
        get(*w, "longitude", c.longitude);
        get(*w, "location_name", c.location_name);
    }
    c.motion = std::clamp(c.motion, 0.0f, 3.0f);
    c.margin_top = std::clamp(c.margin_top, 0, 200);
    c.window_gap = std::clamp(c.window_gap, 0, 80);
    c.roundness = std::clamp(c.roundness, 0.0f, 1.0f);
    c.bar_height = std::clamp(c.bar_height, 24, 48);
    c.shadow_opacity = std::clamp(c.shadow_opacity, 0.0f, 1.0f);
    c.shadow_softness = std::clamp(c.shadow_softness, 2.0f, 48.0f);
    c.shadow_distance = std::clamp(c.shadow_distance, 0.0f, 24.0f);
    c.island_opacity = std::clamp(c.island_opacity, 0.3f, 1.0f);
    *this = c;
}

std::vector<ModuleConf> Config::default_modules() {
    return {{"workspaces", "left", true},   {"window", "left", true},     {"search", "left", false},
            {"perf", "right", false},       {"tray", "right", true},      {"notifications", "right", true},
            {"network", "right", true},
            {"bluetooth", "right", true},   {"volume", "right", true},    {"battery", "right", true}};
}

std::string Config::to_json() const {
    json j;
    j["appearance"] = {{"theme", theme},
                       {"font", font},
                       {"mono_font", mono_font},
                       {"note_font", note_font},
                       {"palette_file", palette_file},
                       {"wallpaper_file", wallpaper_file},
                       {"margin_top", margin_top},
                       {"clock_24h", clock_24h},
                       {"show_workspaces", show_workspaces},
                       {"show_status", show_status},
                       {"bar_layout", bar_layout},
                       {"island_style", island_style},
                       {"dock_sides", dock_sides},
                       {"roundness", roundness},
                       {"bar_height", bar_height},
                       {"shadows", shadows},
                       {"shadow_opacity", shadow_opacity},
                       {"shadow_softness", shadow_softness},
                       {"shadow_distance", shadow_distance},
                       {"shadow_angle", shadow_angle},
                       {"reserve_space", reserve_space},
                       {"window_gap", window_gap},
                       {"glass", glass},
                       {"island_opacity", island_opacity},
                       {"compositor_blur", compositor_blur},
                       {"blur_xray", blur_xray},
                       {"motion", motion},
                       {"glance_delay_ms", glance_delay_ms}};
    j["notifications"] = {{"mode", notification_mode},
                          {"timeout_ms", notification_timeout_ms},
                          {"history", notification_history}};
    j["commands"] = {{"lock", cmd_lock},         {"logout", cmd_logout},
                     {"suspend", cmd_suspend},   {"reboot", cmd_reboot},
                     {"poweroff", cmd_poweroff}, {"night_light", cmd_night_light},
                     {"wallpaper_next", cmd_wallpaper_next}, {"wallpaper_picker", cmd_wallpaper_picker},
                     {"wallpaper_apply", cmd_wallpaper_apply},
                     {"settings", cmd_settings},
                     {"wallpaper_picker_style", wallpaper_picker_style},
                     {"wallpaper_pause_covered", wallpaper_pause_covered},
                     {"wallpaper_engine", wallpaper_engine},
                     {"wallpaper_engine_audio", wallpaper_engine_audio},
                     {"wallpaper_engine_clean", wallpaper_engine_clean},
                     {"wallpaper_engine_fps", wallpaper_engine_fps},
                     {"wallpaper_engine_quality", wallpaper_engine_quality},
                     {"screensaver", screensaver},
                     {"screensaver_minutes", screensaver_minutes},
                     {"screensaver_lock_minutes", screensaver_lock_minutes},
                     {"screensaver_bubbles", screensaver_bubbles},
                     {"screensaver_speed", screensaver_speed},
                     {"terminal", terminal}};
    json mods = json::array();
    for (auto& m : modules) mods.push_back({{"id", m.id}, {"side", m.side}, {"on", m.on}});
    j["appearance"]["modules"] = mods;
    j["appearance"]["battery_percent"] = battery_percent;
    j["appearance"]["perf_metrics"] = perf_metrics;
    j["appearance"]["perf_style"] = perf_style;
    j["appearance"]["perf_split"] = perf_split;
    j["suite"] = {{"wallpaper_dirs", wallpaper_dirs},
                  {"screenshot_dir", screenshot_dir},
                  {"wallhaven_dir", wallhaven_dir},
                  {"wallhaven_key", wallhaven_key},
                  {"wallhaven_purity", wallhaven_purity},
                  {"recording_dir", recording_dir},
                  {"screenshot_copy", screenshot_copy}};
    j["weather"] = {{"cache", weather_cache},
                    {"latitude", latitude},
                    {"longitude", longitude},
                    {"location_name", location_name}};
    return j.dump(2) + "\n";
}

}  // namespace island
