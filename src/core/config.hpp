#pragma once

#include <string>
#include <vector>

namespace island {

// ~/.config/niri-island/config.json. Every key is optional; a missing file is
// written out with the defaults so it can be discovered and edited.
// One bar module: where it goes and whether it shows. The order in the list
// is the order on screen, outwards from the island.
struct ModuleConf {
    std::string id;           // workspaces window search tray notifications network bluetooth volume battery
    std::string side = "right";  // left | right
    bool on = true;
};

struct Config {
    // Appearance
    std::string theme = "adaptive";          // "adaptive" (wallpaper palette) or a fixed one: lavender, ocean, rose, mint, amber, mono
    std::string font = "Inter";
    std::string mono_font = "JetBrainsMono Nerd Font";
    std::string note_font = "Caveat";       // handwriting for notes
    std::string palette_file = "~/.cache/niri-island/colors.json";          // written by the island's backdrop
    std::string wallpaper_file = "~/.local/state/niri-island/wallpaper.json";
    int margin_top = 8;
    bool clock_24h = true;
    bool show_workspaces = true;
    bool show_status = true;                 // wifi / bluetooth / volume / battery capsule
    std::vector<ModuleConf> modules = default_modules();
    bool battery_percent = true;             // the number next to the battery icon
    // The performance island (module "perf"): what it shows, and how.
    std::vector<std::string> perf_metrics = {"cpu", "ram", "temp"};  // cpu ram temp net disk swap
    std::string perf_style = "numbers";      // numbers | rings
    bool perf_split = false;                 // one capsule per metric
    static std::vector<ModuleConf> default_modules();
    std::string bar_layout = "grouped";      // grouped (round the island) | spread (to the edges) | island (one capsule)
    std::string island_style = "floating";
    bool dock_sides = true;                  // attached: the side capsules dock too (false: they stay pills)
    // Shadows under the island and its pills
    bool shadows = true;
    float shadow_opacity = 0.40f;            // 0..1
    float shadow_softness = 18.0f;           // blur radius, px
    float shadow_distance = 4.0f;            // px
    float shadow_angle = 90.0f;              // direction it falls, degrees (90 = down)
    int bar_height = 32;                     // height of the resting island and its side pills, 24-48 px
    float roundness = 1.0f;                  // 0 almost square (still softened) … 1 fully round   // floating | attached (docked to the top edge, notch-like)
    bool reserve_space = true;               // windows keep clear of the island
    int window_gap = 4;                      // px between the island and the windows below (0 = touching)
    bool glass = true;                       // translucent island with compositor blur behind (niri ext-background-effect)
    float island_opacity = 0.80f;            // body opacity when glass is on
    bool blur_xray = false;                  // with niri blur: true shows only the wallpaper, false blurs the windows behind too
    bool compositor_blur = false;            // also ask niri to blur behind (ext-background-effect); off = plain transparency
    float motion = 1.0f;                     // spring speed multiplier, 0 disables motion
    int glance_delay_ms = 140;

    // Notifications
    std::string notification_mode = "auto";  // auto | server | monitor | off
    int notification_timeout_ms = 5500;
    int notification_history = 60;

    // Integrations (shell commands, run with sh -c)
    std::string cmd_lock = "internal";       // "internal" = niri-island's own lock screen; or a command
    std::string cmd_logout = "niri msg action quit --skip-confirmation";
    std::string cmd_suspend = "systemctl suspend";
    std::string cmd_reboot = "systemctl reboot";
    std::string cmd_poweroff = "systemctl poweroff";
    std::string cmd_night_light = "pkill -x wlsunset || setsid -f wlsunset -t 4000 -T 4001";
    std::string cmd_wallpaper_next = "internal";      // "internal" = the island's own backdrop
    std::string cmd_wallpaper_picker = "internal";   // "internal" = niri-island's own picker
    std::string cmd_wallpaper_apply = "internal";     // or a command with {path}
    bool wallpaper_pause_covered = true;
    // The bubbles screensaver, started by the island itself when idle.
    bool screensaver = true;
    int screensaver_minutes = 5;
    int screensaver_lock_minutes = 0;        // idle this long: lock too (0 = never)
    int screensaver_bubbles = 1;             // 0 few, 1 normal, 2 many
    float screensaver_speed = 1.0f;          // 0.5 .. 2            // video wallpapers pause while a window covers the screen
    std::string wallpaper_picker_style = "slices";   // slices (carousel, like skwd-wall) | grid (honeycomb)

    // Suite
    std::vector<std::string> wallpaper_dirs = {"~/Imágenes/wallpapers", "~/Pictures/Wallpapers"};
    std::string wallhaven_dir = "~/Imágenes/wallpapers/wallhaven";  // downloads go here
    std::string wallhaven_key;        // optional API key (NSFW / account settings)
    std::string wallhaven_purity = "100";  // sfw|sketchy|nsfw bits, wallhaven style
    std::string screenshot_dir;   // empty = <Pictures>/Capturas
    std::string recording_dir;    // empty = <Videos>/Grabaciones
    bool screenshot_copy = true;  // also copy every saved capture
    std::string cmd_settings = "";
    std::string terminal = "ghostty";

    // Weather fallback when noctalia's cache is absent
    std::string weather_cache = "~/.cache/noctalia/weather.json";
    double latitude = 0.0;
    double longitude = 0.0;
    std::string location_name;

    static std::string path();
    void load();
    std::string to_json() const;
};

std::string expand_home(const std::string& p);

Config& config();

}  // namespace island
